/*
 * ab_profiles_js.c -- QuickJS marshaling for the platform redraw profiles.
 * Exposes the `nes`, `gb`, `md`, `snes`, `msx`, `pce` globals. ALL logic
 * lives in runtimes/common/ab_profiles.c, shared with the Lua, Python and
 * Ruby runtimes; this file only converts objects to the core's structs and
 * back.
 *
 * Language-shaped differences from the Lua binding, all deliberate:
 *   - configuration failures THROW (a missing binding or region set, a
 *     malformed replace_sprite call) instead of returning nil + reason --
 *     that is how the rest of the ab surface reports in JS;
 *   - transient conditions return null (draw on a frame-read failure,
 *     snes.tick/draw when the core has no frame yet);
 *   - multi-value returns are objects: sprite_bounds -> {x0,y0,x1,y1},
 *     msx.mode -> {mode, description}, snes.frame_size -> {w, h}.
 */
#include <stdint.h>
#include <string.h>

#include "quickjs.h"

#include "../../sdk/active_bezel.h"
#include "../common/ab_render.h"
#include "../common/ab_profiles.h"
#include "../common/ab_nes.h"
#include "../common/ab_gb.h"
#include "../common/ab_md.h"
#include "../common/ab_msx.h"

#define PROF_FN(name) static JSValue name(JSContext *ctx, JSValueConst this_val, \
                                          int argc, JSValueConst *argv)
#define PROF_UNUSED() (void)this_val; (void)argc

/* ------------------------------------------------------------- helpers -- */

static double jopt_num(JSContext *ctx, JSValueConst obj, const char *key,
                       double def) {
  if (!JS_IsObject(obj)) return def;
  JSValue v = JS_GetPropertyStr(ctx, obj, key);
  double d = def;
  if (!JS_IsUndefined(v) && !JS_IsException(v)) {
    if (JS_ToFloat64(ctx, &d, v) < 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      d = def;
    }
  }
  JS_FreeValue(ctx, v);
  return d;
}

static int jopt_int(JSContext *ctx, JSValueConst obj, const char *key, int def) {
  double d = jopt_num(ctx, obj, key, (double)def);
  if (!(d > -2147483649.0 && d < 2147483648.0)) return def;
  return (int)d;
}

/* Tri-state field: absent = -1 (follow the registers), else 0/1. */
static int jopt_tri(JSContext *ctx, JSValueConst obj, const char *key) {
  if (!JS_IsObject(obj)) return -1;
  JSValue v = JS_GetPropertyStr(ctx, obj, key);
  int out = -1;
  if (!JS_IsUndefined(v) && !JS_IsException(v)) out = JS_ToBool(ctx, v) ? 1 : 0;
  JS_FreeValue(ctx, v);
  return out;
}

static int jopt_bool(JSContext *ctx, JSValueConst obj, const char *key, int def) {
  const int tri = jopt_tri(ctx, obj, key);
  return tri < 0 ? def : tri;
}

static int jopt_present(JSContext *ctx, JSValueConst obj, const char *key) {
  if (!JS_IsObject(obj)) return 0;
  JSValue v = JS_GetPropertyStr(ctx, obj, key);
  const int present = !JS_IsUndefined(v) && !JS_IsException(v);
  JS_FreeValue(ctx, v);
  return present;
}

static int jopt_ints(JSContext *ctx, JSValueConst obj, const char *key,
                     int *out, int max, int mask) {
  if (!JS_IsObject(obj)) return 0;
  JSValue arr = JS_GetPropertyStr(ctx, obj, key);
  int n = 0;
  if (JS_IsObject(arr)) {
    JSValue lenv = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t len = 0;
    if (JS_ToUint32(ctx, &len, lenv) < 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      len = 0;
    }
    JS_FreeValue(ctx, lenv);
    for (uint32_t i = 0; i < len && n < max; i++) {
      JSValue item = JS_GetPropertyUint32(ctx, arr, i);
      int32_t value = 0;
      if (JS_ToInt32(ctx, &value, item) == 0) out[n++] = (int)value & mask;
      else JS_FreeValue(ctx, JS_GetException(ctx));
      JS_FreeValue(ctx, item);
    }
  }
  JS_FreeValue(ctx, arr);
  return n;
}

static void jset_int(JSContext *ctx, JSValue obj, const char *key, int v) {
  JS_SetPropertyStr(ctx, obj, key, JS_NewInt32(ctx, v));
}

static void jset_bool(JSContext *ctx, JSValue obj, const char *key, int v) {
  JS_SetPropertyStr(ctx, obj, key, JS_NewBool(ctx, v));
}

static JSValue bounds_object(JSContext *ctx, int ok, const int b[4]) {
  if (!ok) return JS_NULL;
  JSValue obj = JS_NewObject(ctx);
  jset_int(ctx, obj, "x0", b[0]);
  jset_int(ctx, obj, "y0", b[1]);
  jset_int(ctx, obj, "x1", b[2]);
  jset_int(ctx, obj, "y1", b[3]);
  return obj;
}

typedef struct {
  const char *name;
  ab_prof_id id;
  const char *key_field;
  const char *key_desc;
  int tile_mask;
} jsp_desc;

static const jsp_desc JS_PROFS[AB_PROF_COUNT] = {
  [AB_PROF_NES] = { "nes", AB_PROF_NES, "tiles", "tile ids", ~0 },
  [AB_PROF_GB]  = { "gb",  AB_PROF_GB,  "tiles", "tile ids", ~0 },
  /* v1 LIMITATION, on purpose: the kit registry keys tiles 0..255 -- see
   * the md profile notes in runtimes/lua/README.md. */
  [AB_PROF_MD]  = { "md",  AB_PROF_MD,  "tiles", "tile ids", 0xFF },
  [AB_PROF_MSX] = { "msx", AB_PROF_MSX, "tiles", "sprite pattern ids", ~0 },
  [AB_PROF_PCE] = { "pce", AB_PROF_PCE, "patterns", "sprite pattern numbers", ~0 },
};

static JSValue ensure_bound(JSContext *ctx, const jsp_desc *d) {
  if (!ab_prof_bound(d->id))
    return JS_ThrowPlainError(ctx, "%s: call %s.bind() in init() first",
                              d->name, d->name);
  return JS_UNDEFINED;
}
#define ENSURE_BOUND(d) do { \
    JSValue e_ = ensure_bound(ctx, d); \
    if (JS_IsException(e_)) return e_; \
  } while (0)

static JSValue prof_bind(JSContext *ctx, int (*bind)(const char **)) {
  const char *err = NULL;
  if (!bind(&err)) return JS_ThrowPlainError(ctx, "%s", err);
  return JS_TRUE;
}

static JSValue prof_replace_sprite(JSContext *ctx, const jsp_desc *d,
                                   JSValueConst opts) {
  ENSURE_BOUND(d);
  if (!JS_IsObject(opts))
    return JS_ThrowTypeError(ctx, "%s.replace_sprite: pass an object of options",
                             d->name);

  ab_sub_rule rule;
  memset(&rule, 0, sizeof(rule));
  rule.tile_count = jopt_ints(ctx, opts, d->key_field, rule.tiles,
                              AB_SUB_MAX_TILES, d->tile_mask);
  if (rule.tile_count <= 0 && strcmp(d->key_field, "tiles") != 0)
    rule.tile_count = jopt_ints(ctx, opts, "tiles", rule.tiles,
                                AB_SUB_MAX_TILES, d->tile_mask);
  if (rule.tile_count <= 0)
    return JS_ThrowTypeError(ctx,
      "%s.replace_sprite: `%s` must be a non-empty array of %s",
      d->name, d->key_field, d->key_desc);
  rule.exclude_count = jopt_ints(ctx, opts, "anchor_exclude",
                                 rule.anchor_exclude, AB_SUB_MAX_TILES,
                                 d->tile_mask);

  JSValue img = JS_GetPropertyStr(ctx, opts, "image");
  if (!JS_IsObject(img)) {
    JS_FreeValue(ctx, img);
    return JS_ThrowTypeError(ctx,
      "%s.replace_sprite: `image` must be the object returned by ab.image()",
      d->name);
  }
  rule.texture = jopt_int(ctx, img, "texture", 0);
  rule.tex_w   = jopt_int(ctx, img, "width", 0);
  rule.tex_h   = jopt_int(ctx, img, "height", 0);
  JS_FreeValue(ctx, img);

  rule.base_w = jopt_int(ctx, opts, "base_w", 0);
  rule.base_h = jopt_int(ctx, opts, "base_h", 0);
  rule.ring   = (double)jopt_int(ctx, opts, "ring", 0);

  const int id = ab_prof_add_rule(d->id, &rule);
  if (!id)
    return JS_ThrowPlainError(ctx,
      "%s.replace_sprite: registry full or invalid rule", d->name);
  return JS_NewInt32(ctx, id);
}

static JSValue prof_remove(JSContext *ctx, const jsp_desc *d,
                           JSValueConst idv) {
  ENSURE_BOUND(d);
  int32_t id = 0;
  if (JS_ToInt32(ctx, &id, idv) < 0) JS_FreeValue(ctx, JS_GetException(ctx));
  return JS_NewBool(ctx, ab_prof_remove_rule(d->id, (int)id));
}

static JSValue prof_clear(JSContext *ctx, const jsp_desc *d) {
  ENSURE_BOUND(d);
  ab_prof_clear_rules(d->id);
  return JS_UNDEFINED;
}

static void read_view(JSContext *ctx, int argc, JSValueConst *argv,
                      ab_prof_view *v, double def_scale) {
  v->x = 0; v->y = 0; v->scale = def_scale;
  if (argc > 0) {
    v->x = jopt_num(ctx, argv[0], "x", 0);
    v->y = jopt_num(ctx, argv[0], "y", 0);
    v->scale = jopt_num(ctx, argv[0], "scale", def_scale);
  }
}

/* ---------------------------------------------------------------- NES -- */

PROF_FN(js_nes_bind) {
  PROF_UNUSED(); (void)argv;
  return prof_bind(ctx, ab_prof_nes_bind);
}
PROF_FN(js_nes_replace) {
  PROF_UNUSED();
  return prof_replace_sprite(ctx, &JS_PROFS[AB_PROF_NES], argv[0]);
}
PROF_FN(js_nes_remove) {
  PROF_UNUSED();
  return prof_remove(ctx, &JS_PROFS[AB_PROF_NES], argv[0]);
}
PROF_FN(js_nes_clear) {
  PROF_UNUSED(); (void)argv;
  return prof_clear(ctx, &JS_PROFS[AB_PROF_NES]);
}
PROF_FN(js_nes_draw) {
  PROF_UNUSED();
  ENSURE_BOUND(&JS_PROFS[AB_PROF_NES]);
  ab_prof_view v;
  read_view(ctx, argc, argv, &v, 4.0);
  ab_prof_nes_result r;
  const char *err = NULL;
  if (!ab_prof_nes_draw(&v, &r, &err)) return JS_NULL;
  JSValue obj = JS_NewObject(ctx);
  jset_int(ctx, obj, "bg_quads", r.bg_quads);
  jset_int(ctx, obj, "spr_quads", r.spr_quads);
  jset_int(ctx, obj, "hd_drawn", r.hd_drawn);
  jset_int(ctx, obj, "sprites_replaced", r.sprites_replaced);
  return obj;
}
PROF_FN(js_nes_bounds) {
  PROF_UNUSED(); (void)argv;
  ENSURE_BOUND(&JS_PROFS[AB_PROF_NES]);
  int b[4];
  return bounds_object(ctx, ab_prof_nes_sprite_bounds(b), b);
}

/* ----------------------------------------------------------------- GB -- */

PROF_FN(js_gb_bind) {
  PROF_UNUSED(); (void)argv;
  return prof_bind(ctx, ab_prof_gb_bind);
}
PROF_FN(js_gb_replace) {
  PROF_UNUSED();
  return prof_replace_sprite(ctx, &JS_PROFS[AB_PROF_GB], argv[0]);
}
PROF_FN(js_gb_remove) {
  PROF_UNUSED();
  return prof_remove(ctx, &JS_PROFS[AB_PROF_GB], argv[0]);
}
PROF_FN(js_gb_clear) {
  PROF_UNUSED(); (void)argv;
  return prof_clear(ctx, &JS_PROFS[AB_PROF_GB]);
}
PROF_FN(js_gb_draw) {
  PROF_UNUSED();
  ENSURE_BOUND(&JS_PROFS[AB_PROF_GB]);
  ab_prof_view v;
  read_view(ctx, argc, argv, &v, 7.0);
  ab_prof_gb_result r;
  const char *err = NULL;
  if (!ab_prof_gb_draw(&v, &r, &err)) return JS_NULL;
  JSValue obj = JS_NewObject(ctx);
  jset_int(ctx, obj, "bg_quads", r.bg_quads);
  jset_int(ctx, obj, "spr_quads", r.spr_quads);
  jset_int(ctx, obj, "hd_drawn", r.hd_drawn);
  jset_int(ctx, obj, "sprites_replaced", r.sprites_replaced);
  return obj;
}
PROF_FN(js_gb_bounds) {
  PROF_UNUSED(); (void)argv;
  ENSURE_BOUND(&JS_PROFS[AB_PROF_GB]);
  int b[4];
  return bounds_object(ctx, ab_prof_gb_sprite_bounds(b), b);
}

/* ----------------------------------------------------------------- MD -- */

PROF_FN(js_md_bind) {
  PROF_UNUSED(); (void)argv;
  return prof_bind(ctx, ab_prof_md_bind);
}
PROF_FN(js_md_replace) {
  PROF_UNUSED();
  return prof_replace_sprite(ctx, &JS_PROFS[AB_PROF_MD], argv[0]);
}
PROF_FN(js_md_remove) {
  PROF_UNUSED();
  return prof_remove(ctx, &JS_PROFS[AB_PROF_MD], argv[0]);
}
PROF_FN(js_md_clear) {
  PROF_UNUSED(); (void)argv;
  return prof_clear(ctx, &JS_PROFS[AB_PROF_MD]);
}
PROF_FN(js_md_draw) {
  PROF_UNUSED();
  ENSURE_BOUND(&JS_PROFS[AB_PROF_MD]);
  ab_prof_view v;
  read_view(ctx, argc, argv, &v, 4.0);
  ab_prof_md_result r;
  const char *err = NULL;
  if (!ab_prof_md_draw(&v, &r, &err)) return JS_NULL;
  JSValue obj = JS_NewObject(ctx);
  jset_int(ctx, obj, "quads", r.quads);
  jset_int(ctx, obj, "hd_drawn", r.hd_drawn);
  jset_int(ctx, obj, "sprites_replaced", r.sprites_replaced);
  return obj;
}
PROF_FN(js_md_bounds) {
  PROF_UNUSED(); (void)argv;
  ENSURE_BOUND(&JS_PROFS[AB_PROF_MD]);
  int b[4];
  return bounds_object(ctx, ab_prof_md_sprite_bounds(b), b);
}

/* ---------------------------------------------------------------- MSX -- */

PROF_FN(js_msx_bind) {
  PROF_UNUSED(); (void)argv;
  return prof_bind(ctx, ab_prof_msx_bind);
}
PROF_FN(js_msx_replace) {
  PROF_UNUSED();
  return prof_replace_sprite(ctx, &JS_PROFS[AB_PROF_MSX], argv[0]);
}
PROF_FN(js_msx_remove) {
  PROF_UNUSED();
  return prof_remove(ctx, &JS_PROFS[AB_PROF_MSX], argv[0]);
}
PROF_FN(js_msx_clear) {
  PROF_UNUSED(); (void)argv;
  return prof_clear(ctx, &JS_PROFS[AB_PROF_MSX]);
}

/* msx.mode() -> {mode, description} ({mode: null} when unsupported) | null */
PROF_FN(js_msx_mode) {
  PROF_UNUSED(); (void)argv;
  ENSURE_BOUND(&JS_PROFS[AB_PROF_MSX]);
  int mode = 0;
  const char *desc = NULL;
  JSValue obj;
  switch (ab_prof_msx_mode(&mode, &desc)) {
    case AB_PROF_MSX_MODE_OK:
      obj = JS_NewObject(ctx);
      jset_int(ctx, obj, "mode", mode);
      JS_SetPropertyStr(ctx, obj, "description", JS_NewString(ctx, desc));
      return obj;
    case AB_PROF_MSX_MODE_UNSUPPORTED:
      obj = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, obj, "mode", JS_NULL);
      JS_SetPropertyStr(ctx, obj, "description", JS_NewString(ctx, desc));
      return obj;
    default:
      return JS_NULL;
  }
}

PROF_FN(js_msx_draw) {
  PROF_UNUSED();
  ENSURE_BOUND(&JS_PROFS[AB_PROF_MSX]);
  ab_prof_msx_view v;
  read_view(ctx, argc, argv, &v.v, 3.0);
  v.fit_width = argc > 0 ? jopt_bool(ctx, argv[0], "fit_width", 0) : 0;
  ab_prof_msx_result r;
  const char *err = NULL;
  if (!ab_prof_msx_draw(&v, &r, &err)) return JS_NULL;

  JSValue obj = JS_NewObject(ctx);
  if (!r.supported) {
    jset_int(ctx, obj, "quads", 0);
    jset_int(ctx, obj, "hd_drawn", 0);
    jset_int(ctx, obj, "sprites_replaced", 0);
    jset_bool(ctx, obj, "supported", 0);
    JS_SetPropertyStr(ctx, obj, "mode", JS_NULL);
    return obj;
  }
  jset_int(ctx, obj, "quads", r.quads);
  jset_int(ctx, obj, "hd_drawn", r.hd_drawn);
  jset_int(ctx, obj, "sprites_replaced", r.sprites_replaced);
  jset_bool(ctx, obj, "supported", 1);
  jset_int(ctx, obj, "mode", r.mode);
  jset_int(ctx, obj, "width", r.width);
  jset_bool(ctx, obj, "per_line", r.per_line);
  jset_bool(ctx, obj, "vram_replay", r.vram_replay);
  jset_bool(ctx, obj, "retained", r.retained);
  return obj;
}

PROF_FN(js_msx_sprites) {
  PROF_UNUSED(); (void)argv;
  ENSURE_BOUND(&JS_PROFS[AB_PROF_MSX]);
  ab_prof_msx_sprite spr[AB_MSX_SPRITES];
  const int n = ab_prof_msx_sprites(spr, AB_MSX_SPRITES);
  if (n < 0) return JS_NULL;
  JSValue arr = JS_NewArray(ctx);
  for (int i = 0; i < n; i++) {
    JSValue obj = JS_NewObject(ctx);
    jset_int(ctx, obj, "index", spr[i].index);
    jset_int(ctx, obj, "x", spr[i].x);
    jset_int(ctx, obj, "y", spr[i].y);
    jset_int(ctx, obj, "pattern", spr[i].pattern);
    jset_int(ctx, obj, "colour", spr[i].colour);
    JS_SetPropertyUint32(ctx, arr, (uint32_t)i, obj);
  }
  return arr;
}

PROF_FN(js_msx_bounds) {
  PROF_UNUSED(); (void)argv;
  ENSURE_BOUND(&JS_PROFS[AB_PROF_MSX]);
  int b[4];
  return bounds_object(ctx, ab_prof_msx_sprite_bounds(b), b);
}

/* ---------------------------------------------------------------- PCE -- */

PROF_FN(js_pce_bind) {
  PROF_UNUSED(); (void)argv;
  return prof_bind(ctx, ab_prof_pce_bind);
}
PROF_FN(js_pce_replace) {
  PROF_UNUSED();
  return prof_replace_sprite(ctx, &JS_PROFS[AB_PROF_PCE], argv[0]);
}
PROF_FN(js_pce_remove) {
  PROF_UNUSED();
  return prof_remove(ctx, &JS_PROFS[AB_PROF_PCE], argv[0]);
}
PROF_FN(js_pce_clear) {
  PROF_UNUSED(); (void)argv;
  return prof_clear(ctx, &JS_PROFS[AB_PROF_PCE]);
}

PROF_FN(js_pce_draw) {
  PROF_UNUSED();
  ENSURE_BOUND(&JS_PROFS[AB_PROF_PCE]);
  ab_prof_pce_view v;
  ab_prof_pce_view_init(&v);
  if (argc > 0) {
    v.v.x = jopt_num(ctx, argv[0], "x", 0);
    v.v.y = jopt_num(ctx, argv[0], "y", 0);
    v.v.scale = jopt_num(ctx, argv[0], "scale", 4.0);
    v.height = jopt_int(ctx, argv[0], "height", 224);
    v.force_bg = jopt_tri(ctx, argv[0], "bg");
    v.force_sprites = jopt_tri(ctx, argv[0], "sprites");
    v.fb_width = jopt_int(ctx, argv[0], "fb_width", 0);
    if (jopt_present(ctx, argv[0], "pal_delta_row"))
      v.pal_delta_row = jopt_int(ctx, argv[0], "pal_delta_row", 0);
    if (jopt_present(ctx, argv[0], "pal_delta"))
      v.pal_delta = jopt_int(ctx, argv[0], "pal_delta", 0);
    v.no_linepix = jopt_tri(ctx, argv[0], "no_linepix");
    v.no_paldeltas = jopt_tri(ctx, argv[0], "no_paldeltas");
  }
  ab_prof_pce_result r;
  const char *err = NULL;
  if (!ab_prof_pce_draw(&v, &r, &err)) return JS_NULL;
  JSValue obj = JS_NewObject(ctx);
  jset_int(ctx, obj, "quads", r.quads);
  jset_int(ctx, obj, "hd_drawn", r.hd_drawn);
  jset_int(ctx, obj, "sprites_replaced", r.sprites_replaced);
  jset_int(ctx, obj, "width", r.width);
  jset_int(ctx, obj, "height", r.height);
  return obj;
}

/* pce.sprite_bounds(height = 224, fb_width = 0) */
PROF_FN(js_pce_bounds) {
  PROF_UNUSED();
  ENSURE_BOUND(&JS_PROFS[AB_PROF_PCE]);
  int height = 224, fbw = 0;
  if (argc > 0 && JS_ToInt32(ctx, &height, argv[0]) < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    height = 224;
  }
  if (argc > 1 && JS_ToInt32(ctx, &fbw, argv[1]) < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    fbw = 0;
  }
  int b[4];
  return bounds_object(ctx, ab_prof_pce_sprite_bounds(height, fbw, b), b);
}

PROF_FN(js_pce_geometry) {
  PROF_UNUSED(); (void)argv;
  ENSURE_BOUND(&JS_PROFS[AB_PROF_PCE]);
  ab_prof_pce_geometry g;
  if (!ab_prof_pce_get_geometry(&g)) return JS_NULL;
  JSValue obj = JS_NewObject(ctx);
  jset_int(ctx, obj, "width", g.width);
  jset_bool(ctx, obj, "bg", g.bg);
  jset_bool(ctx, obj, "sprites", g.sprites);
  jset_int(ctx, obj, "bat_w", g.bat_w);
  jset_int(ctx, obj, "bat_h", g.bat_h);
  jset_int(ctx, obj, "scroll_x", g.scroll_x);
  jset_int(ctx, obj, "scroll_y", g.scroll_y);
  return obj;
}

/* --------------------------------------------------------------- SNES -- */

PROF_FN(js_snes_bind) {
  PROF_UNUSED(); (void)argv;
  const char *err = NULL;
  if (!ab_prof_snes_bind(&err)) return JS_ThrowPlainError(ctx, "%s", err);
  return JS_TRUE;
}

static JSValue snes_ensure_bound(JSContext *ctx) {
  if (!ab_prof_snes_bound())
    return JS_ThrowPlainError(ctx, "snes: call snes.bind() in init() first");
  return JS_UNDEFINED;
}
#define SNES_ENSURE_BOUND() do { \
    JSValue e_ = snes_ensure_bound(ctx); \
    if (JS_IsException(e_)) return e_; \
  } while (0)

PROF_FN(js_snes_set_hd_tiles) {
  PROF_UNUSED();
  SNES_ENSURE_BOUND();
  /* Accept an ArrayBuffer/TypedArray (ab.asset returns a Uint8Array) or a
   * binary string. */
  size_t n = 0;
  uint8_t *bytes = NULL;
  const char *str = NULL;
  JSValue buf = JS_UNDEFINED;
  size_t byte_offset = 0, byte_length = 0, bytes_per_element = 0;

  buf = JS_GetTypedArrayBuffer(ctx, argv[0], &byte_offset, &byte_length,
                               &bytes_per_element);
  if (!JS_IsException(buf)) {
    size_t total = 0;
    uint8_t *base = JS_GetArrayBuffer(ctx, &total, buf);
    JS_FreeValue(ctx, buf);
    if (base) { bytes = base + byte_offset; n = byte_length; }
  } else {
    JS_FreeValue(ctx, JS_GetException(ctx));
    bytes = JS_GetArrayBuffer(ctx, &n, argv[0]);
    if (!bytes) {
      str = JS_ToCStringLen(ctx, &n, argv[0]);
      if (!str) return JS_EXCEPTION;
    }
  }

  int idxn = 0, rgban = 0;
  const char *err = NULL;
  const int ok = ab_prof_snes_set_hd_tiles(bytes ? (const void *)bytes
                                                 : (const void *)str,
                                           n, &idxn, &rgban, &err);
  if (str) JS_FreeCString(ctx, str);
  if (!ok) return JS_ThrowPlainError(ctx, "%s", err);
  JSValue obj = JS_NewObject(ctx);
  jset_int(ctx, obj, "indexed", idxn);
  jset_int(ctx, obj, "rgba", rgban);
  return obj;
}

PROF_FN(js_snes_tick) {
  PROF_UNUSED();
  SNES_ENSURE_BOUND();
  int compare = 1;
  if (argc > 0) compare = jopt_bool(ctx, argv[0], "compare", 1);
  ab_prof_snes_tick_result r;
  const char *err = NULL;
  switch (ab_prof_snes_tick(compare, &r, &err)) {
    case AB_PROF_SNES_NOT_READY:
      return JS_NULL;
    case AB_PROF_SNES_PLAIN:
      return JS_NewObject(ctx);
    case AB_PROF_SNES_M7: {
      JSValue obj = JS_NewObject(ctx);
      jset_int(ctx, obj, "w", r.w);
      jset_int(ctx, obj, "h", r.h);
      jset_int(ctx, obj, "m7start", r.m7start);
      jset_int(ctx, obj, "m7stop", r.m7stop);
      jset_bool(ctx, obj, "plane_rebuilt", r.plane_rebuilt);
      return obj;
    }
    default:
      return JS_ThrowPlainError(ctx, "%s", err ? err : "snes: tick failed");
  }
}

PROF_FN(js_snes_draw) {
  PROF_UNUSED();
  SNES_ENSURE_BOUND();
  double x = 0, y = 0, scale = 1;
  if (argc > 0) {
    x = jopt_num(ctx, argv[0], "x", 0);
    y = jopt_num(ctx, argv[0], "y", 0);
    scale = jopt_num(ctx, argv[0], "scale", 1);
  }
  ab_prof_snes_draw_result r;
  const char *err = NULL;
  if (!ab_prof_snes_draw(x, y, scale, &r, &err)) {
    if (err) return JS_ThrowPlainError(ctx, "%s", err);
    return JS_NULL;
  }
  JSValue obj = JS_NewObject(ctx);
  jset_int(ctx, obj, "w", r.w);
  jset_int(ctx, obj, "h", r.h);
  jset_int(ctx, obj, "quads", r.quads);
  return obj;
}

PROF_FN(js_snes_frame_size) {
  PROF_UNUSED(); (void)argv;
  SNES_ENSURE_BOUND();
  int w = 0, h = 0;
  if (!ab_prof_snes_frame_size(&w, &h)) return JS_NULL;
  JSValue obj = JS_NewObject(ctx);
  jset_int(ctx, obj, "w", w);
  jset_int(ctx, obj, "h", h);
  return obj;
}

/* -------------------------------------------------------------- wiring -- */

static const JSCFunctionListEntry NES_FUNCS[] = {
  JS_CFUNC_DEF("bind", 0, js_nes_bind),
  JS_CFUNC_DEF("replace_sprite", 1, js_nes_replace),
  JS_CFUNC_DEF("remove_replacement", 1, js_nes_remove),
  JS_CFUNC_DEF("clear_replacements", 0, js_nes_clear),
  JS_CFUNC_DEF("draw", 1, js_nes_draw),
  JS_CFUNC_DEF("sprite_bounds", 0, js_nes_bounds),
};

static const JSCFunctionListEntry GB_FUNCS[] = {
  JS_CFUNC_DEF("bind", 0, js_gb_bind),
  JS_CFUNC_DEF("replace_sprite", 1, js_gb_replace),
  JS_CFUNC_DEF("remove_replacement", 1, js_gb_remove),
  JS_CFUNC_DEF("clear_replacements", 0, js_gb_clear),
  JS_CFUNC_DEF("draw", 1, js_gb_draw),
  JS_CFUNC_DEF("sprite_bounds", 0, js_gb_bounds),
};

static const JSCFunctionListEntry MD_FUNCS[] = {
  JS_CFUNC_DEF("bind", 0, js_md_bind),
  JS_CFUNC_DEF("replace_sprite", 1, js_md_replace),
  JS_CFUNC_DEF("remove_replacement", 1, js_md_remove),
  JS_CFUNC_DEF("clear_replacements", 0, js_md_clear),
  JS_CFUNC_DEF("draw", 1, js_md_draw),
  JS_CFUNC_DEF("sprite_bounds", 0, js_md_bounds),
};

static const JSCFunctionListEntry MSX_FUNCS[] = {
  JS_CFUNC_DEF("bind", 0, js_msx_bind),
  JS_CFUNC_DEF("replace_sprite", 1, js_msx_replace),
  JS_CFUNC_DEF("remove_replacement", 1, js_msx_remove),
  JS_CFUNC_DEF("clear_replacements", 0, js_msx_clear),
  JS_CFUNC_DEF("draw", 1, js_msx_draw),
  JS_CFUNC_DEF("mode", 0, js_msx_mode),
  JS_CFUNC_DEF("sprites", 0, js_msx_sprites),
  JS_CFUNC_DEF("sprite_bounds", 0, js_msx_bounds),
};

static const JSCFunctionListEntry PCE_FUNCS[] = {
  JS_CFUNC_DEF("bind", 0, js_pce_bind),
  JS_CFUNC_DEF("replace_sprite", 1, js_pce_replace),
  JS_CFUNC_DEF("remove_replacement", 1, js_pce_remove),
  JS_CFUNC_DEF("clear_replacements", 0, js_pce_clear),
  JS_CFUNC_DEF("draw", 1, js_pce_draw),
  JS_CFUNC_DEF("sprite_bounds", 2, js_pce_bounds),
  JS_CFUNC_DEF("geometry", 0, js_pce_geometry),
};

static const JSCFunctionListEntry SNES_FUNCS[] = {
  JS_CFUNC_DEF("bind", 0, js_snes_bind),
  JS_CFUNC_DEF("draw", 1, js_snes_draw),
  JS_CFUNC_DEF("frame_size", 0, js_snes_frame_size),
  JS_CFUNC_DEF("set_hd_tiles", 1, js_snes_set_hd_tiles),
  JS_CFUNC_DEF("tick", 1, js_snes_tick),
};

void ab_profiles_js_open(JSContext *ctx, JSValue global) {
  JSValue obj;

  obj = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, obj, NES_FUNCS,
                             (int)(sizeof(NES_FUNCS) / sizeof(NES_FUNCS[0])));
  jset_int(ctx, obj, "WIDTH", AB_NES_W);
  jset_int(ctx, obj, "HEIGHT", AB_NES_H);
  jset_int(ctx, obj, "OVERSCAN_TOP", AB_NES_OVERSCAN_TOP);
  JS_SetPropertyStr(ctx, global, "nes", obj);

  obj = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, obj, GB_FUNCS,
                             (int)(sizeof(GB_FUNCS) / sizeof(GB_FUNCS[0])));
  jset_int(ctx, obj, "WIDTH", AB_GB_W);
  jset_int(ctx, obj, "HEIGHT", AB_GB_H);
  JS_SetPropertyStr(ctx, global, "gb", obj);

  obj = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, obj, MD_FUNCS,
                             (int)(sizeof(MD_FUNCS) / sizeof(MD_FUNCS[0])));
  jset_int(ctx, obj, "MAX_WIDTH", AB_MD_MAX_W);
  jset_int(ctx, obj, "MAX_HEIGHT", AB_MD_MAX_H);
  JS_SetPropertyStr(ctx, global, "md", obj);

  obj = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, obj, SNES_FUNCS,
                             (int)(sizeof(SNES_FUNCS) / sizeof(SNES_FUNCS[0])));
  JS_SetPropertyStr(ctx, global, "snes", obj);

  obj = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, obj, MSX_FUNCS,
                             (int)(sizeof(MSX_FUNCS) / sizeof(MSX_FUNCS[0])));
  jset_int(ctx, obj, "WIDTH", AB_MSX_W);
  /* SCREEN 6/7 frames are MAX_WIDTH wide; msx.draw reports the actual width
   * of the frame it just drew so a bezel does not have to guess. */
  jset_int(ctx, obj, "MAX_WIDTH", AB_MSX_MAXW);
  jset_int(ctx, obj, "HEIGHT", AB_MSX_H);
  jset_int(ctx, obj, "BORDER", AB_MSX_BORDER);
  jset_int(ctx, obj, "DISPLAY_WIDTH", AB_MSX_DISPLAY);
  JS_SetPropertyStr(ctx, global, "msx", obj);

  obj = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, obj, PCE_FUNCS,
                             (int)(sizeof(PCE_FUNCS) / sizeof(PCE_FUNCS[0])));
  /* WIDTH is the common case, not a constant: the VDC's display width is
   * programmable and pce.geometry().width is the live value. */
  jset_int(ctx, obj, "WIDTH", 256);
  jset_int(ctx, obj, "HEIGHT", 224);
  JS_SetPropertyStr(ctx, global, "pce", obj);
}

/* Free everything the profiles own. Call from the runtime's shutdown. */
void ab_profiles_js_shutdown(void) {
  ab_prof_nes_shutdown();
  ab_prof_gb_shutdown();
  ab_prof_md_shutdown();
  ab_prof_msx_shutdown();
  ab_prof_pce_shutdown();
  ab_prof_snes_shutdown();
}
