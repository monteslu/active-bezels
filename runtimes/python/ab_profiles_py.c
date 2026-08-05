/*
 * ab_profiles_py.c -- MicroPython marshaling for the platform redraw
 * profiles. Exposes the `nes`, `gb`, `md`, `snes`, `msx`, `pce` modules
 * (registered as importable modules AND injected as globals, the same
 * convenience the `ab` module gets). ALL logic lives in
 * runtimes/common/ab_profiles.c, shared with the Lua, JS and Ruby runtimes;
 * this file only converts dicts to the core's structs and back.
 *
 * Language-shaped differences from the Lua binding, all deliberate:
 *   - configuration failures RAISE (RuntimeError for a missing binding or
 *     region set, ValueError for malformed arguments) instead of returning
 *     nil + reason -- that is how every other ab.* Python call reports;
 *   - transient conditions return None (draw on a frame-read failure,
 *     snes.tick/draw when the core has no frame yet);
 *   - multi-value returns are tuples (sprite_bounds, msx.mode, frame_size).
 */
#include <stdint.h>
#include <string.h>

#include "py/runtime.h"
#include "py/objmodule.h"
#include "py/objstr.h"

#include "../../sdk/active_bezel.h"
#include "../common/ab_render.h"
#include "../common/ab_profiles.h"
#include "../common/ab_nes.h"
#include "../common/ab_gb.h"
#include "../common/ab_md.h"
#include "../common/ab_msx.h"

/* ------------------------------------------------------------- helpers -- */

/* Keys come from C, not from the script source, so the embed port's qstr
 * generator never sees them -- intern at runtime instead of MP_QSTR_*. */
static qstr pq(const char *name) { return qstr_from_str(name); }

static void pdict_put(mp_obj_t d, const char *key, mp_obj_t value) {
  mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(pq(key)), value);
}

static mp_map_elem_t *pdict_get(mp_obj_t dict, const char *key) {
  if (!mp_obj_is_type(dict, &mp_type_dict)) return NULL;
  return mp_map_lookup(mp_obj_dict_get_map(dict),
                       MP_OBJ_NEW_QSTR(pq(key)), MP_MAP_LOOKUP);
}

static void opt_num(mp_obj_t dict, const char *key, double *v) {
  mp_map_elem_t *e = pdict_get(dict, key);
  if (e) *v = (double)mp_obj_get_float(e->value);
}

static int opt_int(mp_obj_t dict, const char *key, int def) {
  mp_map_elem_t *e = pdict_get(dict, key);
  return e ? (int)mp_obj_get_int(e->value) : def;
}

static int opt_bool(mp_obj_t dict, const char *key, int def) {
  mp_map_elem_t *e = pdict_get(dict, key);
  return e ? (mp_obj_is_true(e->value) ? 1 : 0) : def;
}

/* Tri-state field: absent = -1 (follow the registers), else 0/1. */
static int opt_tri(mp_obj_t dict, const char *key) {
  mp_map_elem_t *e = pdict_get(dict, key);
  if (!e) return -1;
  return mp_obj_is_true(e->value) ? 1 : 0;
}

static int opt_ints(mp_obj_t dict, const char *key, int *out, int max,
                    int mask) {
  mp_map_elem_t *e = pdict_get(dict, key);
  if (!e) return 0;
  size_t count = 0;
  mp_obj_t *items = NULL;
  mp_obj_get_array(e->value, &count, &items);
  int n = 0;
  for (size_t i = 0; i < count && n < max; i++)
    out[n++] = (int)mp_obj_get_int(items[i]) & mask;
  return n;
}

static mp_obj_t bounds_tuple(int ok, const int b[4]) {
  if (!ok) return mp_const_none;
  mp_obj_t items[4] = {
    mp_obj_new_int(b[0]), mp_obj_new_int(b[1]),
    mp_obj_new_int(b[2]), mp_obj_new_int(b[3]),
  };
  return mp_obj_new_tuple(4, items);
}

typedef struct {
  const char *name;
  ab_prof_id id;
  const char *key_field;    /* "tiles", or "patterns" on PCE */
  const char *key_desc;
  int tile_mask;
} pyp_desc;

static const pyp_desc PY_PROFS[AB_PROF_COUNT] = {
  [AB_PROF_NES] = { "nes", AB_PROF_NES, "tiles", "tile ids", ~0 },
  [AB_PROF_GB]  = { "gb",  AB_PROF_GB,  "tiles", "tile ids", ~0 },
  /* v1 LIMITATION, on purpose: the kit registry keys tiles 0..255 -- see
   * the md profile notes in runtimes/lua/README.md. */
  [AB_PROF_MD]  = { "md",  AB_PROF_MD,  "tiles", "tile ids", 0xFF },
  [AB_PROF_MSX] = { "msx", AB_PROF_MSX, "tiles", "sprite pattern ids", ~0 },
  [AB_PROF_PCE] = { "pce", AB_PROF_PCE, "patterns", "sprite pattern numbers", ~0 },
};

static void ensure_bound(const pyp_desc *d) {
  if (!ab_prof_bound(d->id))
    mp_raise_msg_varg(&mp_type_RuntimeError,
                      MP_ERROR_TEXT("%s: call %s.bind() in init() first"),
                      d->name, d->name);
}

static mp_obj_t prof_bind(const pyp_desc *d, int (*bind)(const char **)) {
  (void)d;
  const char *err = NULL;
  if (!bind(&err))
    mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s"), err);
  return mp_const_true;
}

static mp_obj_t prof_replace_sprite(const pyp_desc *d, mp_obj_t opts) {
  ensure_bound(d);
  if (!mp_obj_is_type(opts, &mp_type_dict))
    mp_raise_msg_varg(&mp_type_ValueError,
                      MP_ERROR_TEXT("%s.replace_sprite: pass a dict of options"),
                      d->name);

  ab_sub_rule rule;
  memset(&rule, 0, sizeof(rule));
  rule.tile_count = opt_ints(opts, d->key_field, rule.tiles,
                             AB_SUB_MAX_TILES, d->tile_mask);
  if (rule.tile_count <= 0 && strcmp(d->key_field, "tiles") != 0)
    rule.tile_count = opt_ints(opts, "tiles", rule.tiles,
                               AB_SUB_MAX_TILES, d->tile_mask);
  if (rule.tile_count <= 0)
    mp_raise_msg_varg(&mp_type_ValueError,
      MP_ERROR_TEXT("%s.replace_sprite: '%s' must be a non-empty list of %s"),
      d->name, d->key_field, d->key_desc);
  rule.exclude_count = opt_ints(opts, "anchor_exclude", rule.anchor_exclude,
                                AB_SUB_MAX_TILES, d->tile_mask);

  mp_map_elem_t *img = pdict_get(opts, "image");
  if (!img || !mp_obj_is_type(img->value, &mp_type_dict))
    mp_raise_msg_varg(&mp_type_ValueError,
      MP_ERROR_TEXT("%s.replace_sprite: 'image' must be the dict returned "
                    "by ab.image()"), d->name);
  rule.texture = (int32_t)opt_int(img->value, "texture", 0);
  rule.tex_w   = opt_int(img->value, "width", 0);
  rule.tex_h   = opt_int(img->value, "height", 0);

  rule.base_w = opt_int(opts, "base_w", 0);
  rule.base_h = opt_int(opts, "base_h", 0);
  rule.ring   = (double)opt_int(opts, "ring", 0);

  const int id = ab_prof_add_rule(d->id, &rule);
  if (!id)
    mp_raise_msg_varg(&mp_type_RuntimeError,
      MP_ERROR_TEXT("%s.replace_sprite: registry full or invalid rule"),
      d->name);
  return mp_obj_new_int(id);
}

static mp_obj_t prof_remove(const pyp_desc *d, mp_obj_t id) {
  ensure_bound(d);
  return mp_obj_new_bool(ab_prof_remove_rule(d->id, (int)mp_obj_get_int(id)));
}

static mp_obj_t prof_clear(const pyp_desc *d) {
  ensure_bound(d);
  ab_prof_clear_rules(d->id);
  return mp_const_none;
}

static void read_view(size_t n, const mp_obj_t *a, ab_prof_view *v,
                      double def_scale) {
  v->x = 0; v->y = 0; v->scale = def_scale;
  if (n > 0) {
    opt_num(a[0], "x", &v->x);
    opt_num(a[0], "y", &v->y);
    opt_num(a[0], "scale", &v->scale);
  }
}

/* ---------------------------------------------------------------- NES -- */

static mp_obj_t nes_bind_fn(void) {
  return prof_bind(&PY_PROFS[AB_PROF_NES], ab_prof_nes_bind);
}
static MP_DEFINE_CONST_FUN_OBJ_0(nes_bind_obj, nes_bind_fn);

static mp_obj_t nes_replace_fn(mp_obj_t opts) {
  return prof_replace_sprite(&PY_PROFS[AB_PROF_NES], opts);
}
static MP_DEFINE_CONST_FUN_OBJ_1(nes_replace_obj, nes_replace_fn);

static mp_obj_t nes_remove_fn(mp_obj_t id) {
  return prof_remove(&PY_PROFS[AB_PROF_NES], id);
}
static MP_DEFINE_CONST_FUN_OBJ_1(nes_remove_obj, nes_remove_fn);

static mp_obj_t nes_clear_fn(void) { return prof_clear(&PY_PROFS[AB_PROF_NES]); }
static MP_DEFINE_CONST_FUN_OBJ_0(nes_clear_obj, nes_clear_fn);

static mp_obj_t nes_draw_fn(size_t n, const mp_obj_t *a) {
  ensure_bound(&PY_PROFS[AB_PROF_NES]);
  ab_prof_view v;
  read_view(n, a, &v, 4.0);
  ab_prof_nes_result r;
  const char *err = NULL;
  if (!ab_prof_nes_draw(&v, &r, &err)) return mp_const_none;
  mp_obj_t d = mp_obj_new_dict(4);
  pdict_put(d, "bg_quads", mp_obj_new_int(r.bg_quads));
  pdict_put(d, "spr_quads", mp_obj_new_int(r.spr_quads));
  pdict_put(d, "hd_drawn", mp_obj_new_int(r.hd_drawn));
  pdict_put(d, "sprites_replaced", mp_obj_new_int(r.sprites_replaced));
  return d;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(nes_draw_obj, 0, 1, nes_draw_fn);

static mp_obj_t nes_bounds_fn(void) {
  ensure_bound(&PY_PROFS[AB_PROF_NES]);
  int b[4];
  return bounds_tuple(ab_prof_nes_sprite_bounds(b), b);
}
static MP_DEFINE_CONST_FUN_OBJ_0(nes_bounds_obj, nes_bounds_fn);

/* ----------------------------------------------------------------- GB -- */

static mp_obj_t gb_bind_fn(void) {
  return prof_bind(&PY_PROFS[AB_PROF_GB], ab_prof_gb_bind);
}
static MP_DEFINE_CONST_FUN_OBJ_0(gb_bind_obj, gb_bind_fn);

static mp_obj_t gb_replace_fn(mp_obj_t opts) {
  return prof_replace_sprite(&PY_PROFS[AB_PROF_GB], opts);
}
static MP_DEFINE_CONST_FUN_OBJ_1(gb_replace_obj, gb_replace_fn);

static mp_obj_t gb_remove_fn(mp_obj_t id) {
  return prof_remove(&PY_PROFS[AB_PROF_GB], id);
}
static MP_DEFINE_CONST_FUN_OBJ_1(gb_remove_obj, gb_remove_fn);

static mp_obj_t gb_clear_fn(void) { return prof_clear(&PY_PROFS[AB_PROF_GB]); }
static MP_DEFINE_CONST_FUN_OBJ_0(gb_clear_obj, gb_clear_fn);

static mp_obj_t gb_draw_fn(size_t n, const mp_obj_t *a) {
  ensure_bound(&PY_PROFS[AB_PROF_GB]);
  ab_prof_view v;
  read_view(n, a, &v, 7.0);
  ab_prof_gb_result r;
  const char *err = NULL;
  if (!ab_prof_gb_draw(&v, &r, &err)) return mp_const_none;
  mp_obj_t d = mp_obj_new_dict(4);
  pdict_put(d, "bg_quads", mp_obj_new_int(r.bg_quads));
  pdict_put(d, "spr_quads", mp_obj_new_int(r.spr_quads));
  pdict_put(d, "hd_drawn", mp_obj_new_int(r.hd_drawn));
  pdict_put(d, "sprites_replaced", mp_obj_new_int(r.sprites_replaced));
  return d;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gb_draw_obj, 0, 1, gb_draw_fn);

static mp_obj_t gb_bounds_fn(void) {
  ensure_bound(&PY_PROFS[AB_PROF_GB]);
  int b[4];
  return bounds_tuple(ab_prof_gb_sprite_bounds(b), b);
}
static MP_DEFINE_CONST_FUN_OBJ_0(gb_bounds_obj, gb_bounds_fn);

/* ----------------------------------------------------------------- MD -- */

static mp_obj_t md_bind_fn(void) {
  return prof_bind(&PY_PROFS[AB_PROF_MD], ab_prof_md_bind);
}
static MP_DEFINE_CONST_FUN_OBJ_0(md_bind_obj, md_bind_fn);

static mp_obj_t md_replace_fn(mp_obj_t opts) {
  return prof_replace_sprite(&PY_PROFS[AB_PROF_MD], opts);
}
static MP_DEFINE_CONST_FUN_OBJ_1(md_replace_obj, md_replace_fn);

static mp_obj_t md_remove_fn(mp_obj_t id) {
  return prof_remove(&PY_PROFS[AB_PROF_MD], id);
}
static MP_DEFINE_CONST_FUN_OBJ_1(md_remove_obj, md_remove_fn);

static mp_obj_t md_clear_fn(void) { return prof_clear(&PY_PROFS[AB_PROF_MD]); }
static MP_DEFINE_CONST_FUN_OBJ_0(md_clear_obj, md_clear_fn);

static mp_obj_t md_draw_fn(size_t n, const mp_obj_t *a) {
  ensure_bound(&PY_PROFS[AB_PROF_MD]);
  ab_prof_view v;
  read_view(n, a, &v, 4.0);
  ab_prof_md_result r;
  const char *err = NULL;
  if (!ab_prof_md_draw(&v, &r, &err)) return mp_const_none;
  mp_obj_t d = mp_obj_new_dict(3);
  pdict_put(d, "quads", mp_obj_new_int(r.quads));
  pdict_put(d, "hd_drawn", mp_obj_new_int(r.hd_drawn));
  pdict_put(d, "sprites_replaced", mp_obj_new_int(r.sprites_replaced));
  return d;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(md_draw_obj, 0, 1, md_draw_fn);

static mp_obj_t md_bounds_fn(void) {
  ensure_bound(&PY_PROFS[AB_PROF_MD]);
  int b[4];
  return bounds_tuple(ab_prof_md_sprite_bounds(b), b);
}
static MP_DEFINE_CONST_FUN_OBJ_0(md_bounds_obj, md_bounds_fn);

/* ---------------------------------------------------------------- MSX -- */

static mp_obj_t msx_bind_fn(void) {
  return prof_bind(&PY_PROFS[AB_PROF_MSX], ab_prof_msx_bind);
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_bind_obj, msx_bind_fn);

static mp_obj_t msx_replace_fn(mp_obj_t opts) {
  return prof_replace_sprite(&PY_PROFS[AB_PROF_MSX], opts);
}
static MP_DEFINE_CONST_FUN_OBJ_1(msx_replace_obj, msx_replace_fn);

static mp_obj_t msx_remove_fn(mp_obj_t id) {
  return prof_remove(&PY_PROFS[AB_PROF_MSX], id);
}
static MP_DEFINE_CONST_FUN_OBJ_1(msx_remove_obj, msx_remove_fn);

static mp_obj_t msx_clear_fn(void) { return prof_clear(&PY_PROFS[AB_PROF_MSX]); }
static MP_DEFINE_CONST_FUN_OBJ_0(msx_clear_obj, msx_clear_fn);

/* msx.mode() -> (screen_mode_number, description) | (None, reason) | None */
static mp_obj_t msx_mode_fn(void) {
  ensure_bound(&PY_PROFS[AB_PROF_MSX]);
  int mode = 0;
  const char *desc = NULL;
  switch (ab_prof_msx_mode(&mode, &desc)) {
    case AB_PROF_MSX_MODE_OK: {
      mp_obj_t items[2] = { mp_obj_new_int(mode),
                            mp_obj_new_str(desc, strlen(desc)) };
      return mp_obj_new_tuple(2, items);
    }
    case AB_PROF_MSX_MODE_UNSUPPORTED: {
      mp_obj_t items[2] = { mp_const_none,
                            mp_obj_new_str(desc, strlen(desc)) };
      return mp_obj_new_tuple(2, items);
    }
    default:
      return mp_const_none;
  }
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_mode_obj, msx_mode_fn);

static mp_obj_t msx_draw_fn(size_t n, const mp_obj_t *a) {
  ensure_bound(&PY_PROFS[AB_PROF_MSX]);
  ab_prof_msx_view v;
  read_view(n, a, &v.v, 3.0);
  v.fit_width = n > 0 ? opt_bool(a[0], "fit_width", 0) : 0;
  ab_prof_msx_result r;
  const char *err = NULL;
  if (!ab_prof_msx_draw(&v, &r, &err)) return mp_const_none;

  if (!r.supported) {
    mp_obj_t d = mp_obj_new_dict(5);
    pdict_put(d, "quads", mp_obj_new_int(0));
    pdict_put(d, "hd_drawn", mp_obj_new_int(0));
    pdict_put(d, "sprites_replaced", mp_obj_new_int(0));
    pdict_put(d, "supported", mp_const_false);
    pdict_put(d, "mode", mp_const_none);
    return d;
  }

  mp_obj_t d = mp_obj_new_dict(9);
  pdict_put(d, "quads", mp_obj_new_int(r.quads));
  pdict_put(d, "hd_drawn", mp_obj_new_int(r.hd_drawn));
  pdict_put(d, "sprites_replaced", mp_obj_new_int(r.sprites_replaced));
  pdict_put(d, "supported", mp_const_true);
  pdict_put(d, "mode", mp_obj_new_int(r.mode));
  pdict_put(d, "width", mp_obj_new_int(r.width));
  pdict_put(d, "per_line", mp_obj_new_bool(r.per_line));
  pdict_put(d, "vram_replay", mp_obj_new_bool(r.vram_replay));
  pdict_put(d, "retained", mp_obj_new_bool(r.retained));
  return d;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(msx_draw_obj, 0, 1, msx_draw_fn);

static mp_obj_t msx_sprites_fn(void) {
  ensure_bound(&PY_PROFS[AB_PROF_MSX]);
  ab_prof_msx_sprite spr[AB_MSX_SPRITES];
  const int n = ab_prof_msx_sprites(spr, AB_MSX_SPRITES);
  if (n < 0) return mp_const_none;
  mp_obj_t list = mp_obj_new_list(0, NULL);
  for (int i = 0; i < n; i++) {
    mp_obj_t d = mp_obj_new_dict(5);
    pdict_put(d, "index", mp_obj_new_int(spr[i].index));
    pdict_put(d, "x", mp_obj_new_int(spr[i].x));
    pdict_put(d, "y", mp_obj_new_int(spr[i].y));
    pdict_put(d, "pattern", mp_obj_new_int(spr[i].pattern));
    pdict_put(d, "colour", mp_obj_new_int(spr[i].colour));
    mp_obj_list_append(list, d);
  }
  return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_sprites_obj, msx_sprites_fn);

static mp_obj_t msx_bounds_fn(void) {
  ensure_bound(&PY_PROFS[AB_PROF_MSX]);
  int b[4];
  return bounds_tuple(ab_prof_msx_sprite_bounds(b), b);
}
static MP_DEFINE_CONST_FUN_OBJ_0(msx_bounds_obj, msx_bounds_fn);

/* ---------------------------------------------------------------- PCE -- */

static mp_obj_t pce_bind_fn(void) {
  return prof_bind(&PY_PROFS[AB_PROF_PCE], ab_prof_pce_bind);
}
static MP_DEFINE_CONST_FUN_OBJ_0(pce_bind_obj, pce_bind_fn);

static mp_obj_t pce_replace_fn(mp_obj_t opts) {
  return prof_replace_sprite(&PY_PROFS[AB_PROF_PCE], opts);
}
static MP_DEFINE_CONST_FUN_OBJ_1(pce_replace_obj, pce_replace_fn);

static mp_obj_t pce_remove_fn(mp_obj_t id) {
  return prof_remove(&PY_PROFS[AB_PROF_PCE], id);
}
static MP_DEFINE_CONST_FUN_OBJ_1(pce_remove_obj, pce_remove_fn);

static mp_obj_t pce_clear_fn(void) { return prof_clear(&PY_PROFS[AB_PROF_PCE]); }
static MP_DEFINE_CONST_FUN_OBJ_0(pce_clear_obj, pce_clear_fn);

static mp_obj_t pce_draw_fn(size_t n, const mp_obj_t *a) {
  ensure_bound(&PY_PROFS[AB_PROF_PCE]);
  ab_prof_pce_view v;
  ab_prof_pce_view_init(&v);
  if (n > 0) {
    opt_num(a[0], "x", &v.v.x);
    opt_num(a[0], "y", &v.v.y);
    opt_num(a[0], "scale", &v.v.scale);
    v.height = opt_int(a[0], "height", 224);
    v.force_bg = opt_tri(a[0], "bg");
    v.force_sprites = opt_tri(a[0], "sprites");
    v.fb_width = opt_int(a[0], "fb_width", 0);
    v.pal_delta_row = opt_int(a[0], "pal_delta_row", AB_PROF_PCE_UNSET);
    v.pal_delta = opt_int(a[0], "pal_delta", AB_PROF_PCE_UNSET);
    v.no_linepix = opt_tri(a[0], "no_linepix");
    v.no_paldeltas = opt_tri(a[0], "no_paldeltas");
  }
  ab_prof_pce_result r;
  const char *err = NULL;
  if (!ab_prof_pce_draw(&v, &r, &err)) return mp_const_none;
  mp_obj_t d = mp_obj_new_dict(5);
  pdict_put(d, "quads", mp_obj_new_int(r.quads));
  pdict_put(d, "hd_drawn", mp_obj_new_int(r.hd_drawn));
  pdict_put(d, "sprites_replaced", mp_obj_new_int(r.sprites_replaced));
  pdict_put(d, "width", mp_obj_new_int(r.width));
  pdict_put(d, "height", mp_obj_new_int(r.height));
  return d;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pce_draw_obj, 0, 1, pce_draw_fn);

/* pce.sprite_bounds(height=224, fb_width=0) */
static mp_obj_t pce_bounds_fn(size_t n, const mp_obj_t *a) {
  ensure_bound(&PY_PROFS[AB_PROF_PCE]);
  const int height = n > 0 ? (int)mp_obj_get_int(a[0]) : 224;
  const int fbw = n > 1 ? (int)mp_obj_get_int(a[1]) : 0;
  int b[4];
  return bounds_tuple(ab_prof_pce_sprite_bounds(height, fbw, b), b);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pce_bounds_obj, 0, 2, pce_bounds_fn);

static mp_obj_t pce_geometry_fn(void) {
  ensure_bound(&PY_PROFS[AB_PROF_PCE]);
  ab_prof_pce_geometry g;
  if (!ab_prof_pce_get_geometry(&g)) return mp_const_none;
  mp_obj_t d = mp_obj_new_dict(7);
  pdict_put(d, "width", mp_obj_new_int(g.width));
  pdict_put(d, "bg", mp_obj_new_bool(g.bg));
  pdict_put(d, "sprites", mp_obj_new_bool(g.sprites));
  pdict_put(d, "bat_w", mp_obj_new_int(g.bat_w));
  pdict_put(d, "bat_h", mp_obj_new_int(g.bat_h));
  pdict_put(d, "scroll_x", mp_obj_new_int(g.scroll_x));
  pdict_put(d, "scroll_y", mp_obj_new_int(g.scroll_y));
  return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(pce_geometry_obj, pce_geometry_fn);

/* --------------------------------------------------------------- SNES -- */

static void snes_ensure_bound(void) {
  if (!ab_prof_snes_bound())
    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("snes: call snes.bind() in init() first"));
}

static mp_obj_t snes_bind_fn(void) {
  const char *err = NULL;
  if (!ab_prof_snes_bind(&err))
    mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s"), err);
  return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(snes_bind_obj, snes_bind_fn);

static mp_obj_t snes_set_hd_tiles_fn(mp_obj_t blob) {
  snes_ensure_bound();
  size_t n = 0;
  const char *data = mp_obj_str_get_data(blob, &n);
  int idxn = 0, rgban = 0;
  const char *err = NULL;
  if (!ab_prof_snes_set_hd_tiles(data, n, &idxn, &rgban, &err))
    mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("%s"), err);
  mp_obj_t items[2] = { mp_obj_new_int(idxn), mp_obj_new_int(rgban) };
  return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(snes_set_hd_tiles_obj, snes_set_hd_tiles_fn);

static mp_obj_t snes_tick_fn(size_t n, const mp_obj_t *a) {
  snes_ensure_bound();
  int compare = 1;
  if (n > 0) compare = opt_bool(a[0], "compare", 1);
  ab_prof_snes_tick_result r;
  const char *err = NULL;
  switch (ab_prof_snes_tick(compare, &r, &err)) {
    case AB_PROF_SNES_NOT_READY:
      return mp_const_none;
    case AB_PROF_SNES_PLAIN:
      return mp_obj_new_dict(0);
    case AB_PROF_SNES_M7: {
      mp_obj_t d = mp_obj_new_dict(5);
      pdict_put(d, "w", mp_obj_new_int(r.w));
      pdict_put(d, "h", mp_obj_new_int(r.h));
      pdict_put(d, "m7start", mp_obj_new_int(r.m7start));
      pdict_put(d, "m7stop", mp_obj_new_int(r.m7stop));
      pdict_put(d, "plane_rebuilt", mp_obj_new_bool(r.plane_rebuilt));
      return d;
    }
    default:
      mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s"),
                        err ? err : "snes: tick failed");
      return mp_const_none;   /* unreachable */
  }
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(snes_tick_obj, 0, 1, snes_tick_fn);

static mp_obj_t snes_draw_fn(size_t n, const mp_obj_t *a) {
  snes_ensure_bound();
  double x = 0, y = 0, scale = 1;
  if (n > 0) {
    opt_num(a[0], "x", &x);
    opt_num(a[0], "y", &y);
    opt_num(a[0], "scale", &scale);
  }
  ab_prof_snes_draw_result r;
  const char *err = NULL;
  if (!ab_prof_snes_draw(x, y, scale, &r, &err)) {
    if (err) mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s"), err);
    return mp_const_none;
  }
  mp_obj_t d = mp_obj_new_dict(3);
  pdict_put(d, "w", mp_obj_new_int(r.w));
  pdict_put(d, "h", mp_obj_new_int(r.h));
  pdict_put(d, "quads", mp_obj_new_int(r.quads));
  return d;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(snes_draw_obj, 0, 1, snes_draw_fn);

static mp_obj_t snes_frame_size_fn(void) {
  snes_ensure_bound();
  int w = 0, h = 0;
  if (!ab_prof_snes_frame_size(&w, &h)) return mp_const_none;
  mp_obj_t items[2] = { mp_obj_new_int(w), mp_obj_new_int(h) };
  return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(snes_frame_size_obj, snes_frame_size_fn);

/* -------------------------------------------------------------- wiring -- */

static mp_obj_t make_module(const char *name) {
  mp_obj_t mod = mp_obj_new_module(pq(name));
  mp_store_global(pq(name), mod);
  return MP_OBJ_FROM_PTR(mp_obj_module_get_globals(mod));
}

void ab_profiles_py_register(void) {
  mp_obj_t d;

  d = make_module("nes");
  pdict_put(d, "bind", MP_OBJ_FROM_PTR(&nes_bind_obj));
  pdict_put(d, "replace_sprite", MP_OBJ_FROM_PTR(&nes_replace_obj));
  pdict_put(d, "remove_replacement", MP_OBJ_FROM_PTR(&nes_remove_obj));
  pdict_put(d, "clear_replacements", MP_OBJ_FROM_PTR(&nes_clear_obj));
  pdict_put(d, "draw", MP_OBJ_FROM_PTR(&nes_draw_obj));
  pdict_put(d, "sprite_bounds", MP_OBJ_FROM_PTR(&nes_bounds_obj));
  pdict_put(d, "WIDTH", mp_obj_new_int(AB_NES_W));
  pdict_put(d, "HEIGHT", mp_obj_new_int(AB_NES_H));
  pdict_put(d, "OVERSCAN_TOP", mp_obj_new_int(AB_NES_OVERSCAN_TOP));

  d = make_module("gb");
  pdict_put(d, "bind", MP_OBJ_FROM_PTR(&gb_bind_obj));
  pdict_put(d, "replace_sprite", MP_OBJ_FROM_PTR(&gb_replace_obj));
  pdict_put(d, "remove_replacement", MP_OBJ_FROM_PTR(&gb_remove_obj));
  pdict_put(d, "clear_replacements", MP_OBJ_FROM_PTR(&gb_clear_obj));
  pdict_put(d, "draw", MP_OBJ_FROM_PTR(&gb_draw_obj));
  pdict_put(d, "sprite_bounds", MP_OBJ_FROM_PTR(&gb_bounds_obj));
  pdict_put(d, "WIDTH", mp_obj_new_int(AB_GB_W));
  pdict_put(d, "HEIGHT", mp_obj_new_int(AB_GB_H));

  d = make_module("md");
  pdict_put(d, "bind", MP_OBJ_FROM_PTR(&md_bind_obj));
  pdict_put(d, "replace_sprite", MP_OBJ_FROM_PTR(&md_replace_obj));
  pdict_put(d, "remove_replacement", MP_OBJ_FROM_PTR(&md_remove_obj));
  pdict_put(d, "clear_replacements", MP_OBJ_FROM_PTR(&md_clear_obj));
  pdict_put(d, "draw", MP_OBJ_FROM_PTR(&md_draw_obj));
  pdict_put(d, "sprite_bounds", MP_OBJ_FROM_PTR(&md_bounds_obj));
  pdict_put(d, "MAX_WIDTH", mp_obj_new_int(AB_MD_MAX_W));
  pdict_put(d, "MAX_HEIGHT", mp_obj_new_int(AB_MD_MAX_H));

  d = make_module("snes");
  pdict_put(d, "bind", MP_OBJ_FROM_PTR(&snes_bind_obj));
  pdict_put(d, "draw", MP_OBJ_FROM_PTR(&snes_draw_obj));
  pdict_put(d, "frame_size", MP_OBJ_FROM_PTR(&snes_frame_size_obj));
  pdict_put(d, "set_hd_tiles", MP_OBJ_FROM_PTR(&snes_set_hd_tiles_obj));
  pdict_put(d, "tick", MP_OBJ_FROM_PTR(&snes_tick_obj));

  d = make_module("msx");
  pdict_put(d, "bind", MP_OBJ_FROM_PTR(&msx_bind_obj));
  pdict_put(d, "replace_sprite", MP_OBJ_FROM_PTR(&msx_replace_obj));
  pdict_put(d, "remove_replacement", MP_OBJ_FROM_PTR(&msx_remove_obj));
  pdict_put(d, "clear_replacements", MP_OBJ_FROM_PTR(&msx_clear_obj));
  pdict_put(d, "draw", MP_OBJ_FROM_PTR(&msx_draw_obj));
  pdict_put(d, "mode", MP_OBJ_FROM_PTR(&msx_mode_obj));
  pdict_put(d, "sprites", MP_OBJ_FROM_PTR(&msx_sprites_obj));
  pdict_put(d, "sprite_bounds", MP_OBJ_FROM_PTR(&msx_bounds_obj));
  pdict_put(d, "WIDTH", mp_obj_new_int(AB_MSX_W));
  pdict_put(d, "MAX_WIDTH", mp_obj_new_int(AB_MSX_MAXW));
  pdict_put(d, "HEIGHT", mp_obj_new_int(AB_MSX_H));
  pdict_put(d, "BORDER", mp_obj_new_int(AB_MSX_BORDER));
  pdict_put(d, "DISPLAY_WIDTH", mp_obj_new_int(AB_MSX_DISPLAY));

  d = make_module("pce");
  pdict_put(d, "bind", MP_OBJ_FROM_PTR(&pce_bind_obj));
  pdict_put(d, "replace_sprite", MP_OBJ_FROM_PTR(&pce_replace_obj));
  pdict_put(d, "remove_replacement", MP_OBJ_FROM_PTR(&pce_remove_obj));
  pdict_put(d, "clear_replacements", MP_OBJ_FROM_PTR(&pce_clear_obj));
  pdict_put(d, "draw", MP_OBJ_FROM_PTR(&pce_draw_obj));
  pdict_put(d, "sprite_bounds", MP_OBJ_FROM_PTR(&pce_bounds_obj));
  pdict_put(d, "geometry", MP_OBJ_FROM_PTR(&pce_geometry_obj));
  pdict_put(d, "WIDTH", mp_obj_new_int(256));
  pdict_put(d, "HEIGHT", mp_obj_new_int(224));
}

/* Free everything the profiles own. Call from the runtime's shutdown. */
void ab_profiles_py_shutdown(void) {
  ab_prof_nes_shutdown();
  ab_prof_gb_shutdown();
  ab_prof_md_shutdown();
  ab_prof_msx_shutdown();
  ab_prof_pce_shutdown();
  ab_prof_snes_shutdown();
}
