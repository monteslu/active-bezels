/*
 * ab_profiles_rb.c -- mruby marshaling for the platform redraw profiles.
 * Exposes the `NES`, `GB`, `MD`, `SNES`, `MSX`, `PCE` modules (Ruby
 * constants must be uppercase, so the globals are spelled like `AB` is).
 * ALL logic lives in runtimes/common/ab_profiles.c, shared with the Lua,
 * Python and JS runtimes; this file only converts Hashes to the core's
 * structs and back.
 *
 * Language-shaped differences from the Lua binding, all deliberate:
 *   - configuration failures RAISE (RuntimeError for a missing binding or
 *     region set, ArgumentError for malformed arguments) instead of
 *     returning nil + reason -- that is how every other AB call reports;
 *   - transient conditions return nil (draw on a frame-read failure,
 *     SNES.tick/draw when the core has no frame yet);
 *   - multi-value returns are Arrays (sprite_bounds, MSX.mode, frame_size);
 *   - option Hashes accept symbol keys ({x: 240}) and string keys alike.
 */
#include <stdint.h>
#include <string.h>

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby/variable.h>

#include "../../sdk/active_bezel.h"
#include "../common/ab_render.h"
#include "../common/ab_profiles.h"
#include "../common/ab_nes.h"
#include "../common/ab_gb.h"
#include "../common/ab_md.h"
#include "../common/ab_msx.h"

/* ------------------------------------------------------------- helpers -- */

/* exc_runtime(m) / exc_argument(m) expand against an identifier literally
 * named `mrb` (runtime.c gets away with it via its global); these helpers
 * work with any state name. */
static struct RClass *exc_runtime(mrb_state *mrb) {
  return mrb_exc_get_id(mrb, MRB_ERROR_SYM(RuntimeError));
}
static struct RClass *exc_argument(mrb_state *mrb) {
  return mrb_exc_get_id(mrb, MRB_ERROR_SYM(ArgumentError));
}

static mrb_value h_get(mrb_state *m, mrb_value h, const char *key) {
  if (!mrb_hash_p(h)) return mrb_nil_value();
  mrb_value v = mrb_hash_get(m, h, mrb_symbol_value(mrb_intern_cstr(m, key)));
  if (mrb_nil_p(v)) v = mrb_hash_get(m, h, mrb_str_new_cstr(m, key));
  return v;
}

static void ropt_num(mrb_state *m, mrb_value h, const char *key, double *out) {
  mrb_value v = h_get(m, h, key);
  if (mrb_float_p(v)) *out = (double)mrb_float(v);
  else if (mrb_integer_p(v)) *out = (double)mrb_integer(v);
}

static int ropt_int(mrb_state *m, mrb_value h, const char *key, int def) {
  mrb_value v = h_get(m, h, key);
  if (mrb_integer_p(v)) return (int)mrb_integer(v);
  if (mrb_float_p(v)) return (int)mrb_float(v);
  return def;
}

static int ropt_bool(mrb_state *m, mrb_value h, const char *key, int def) {
  mrb_value v = h_get(m, h, key);
  if (mrb_nil_p(v)) return def;
  return mrb_test(v) ? 1 : 0;
}

/* Tri-state field: absent = -1 (follow the registers), else 0/1. */
static int ropt_tri(mrb_state *m, mrb_value h, const char *key) {
  mrb_value v = h_get(m, h, key);
  if (mrb_nil_p(v)) return -1;
  if (mrb_integer_p(v)) return mrb_integer(v) ? 1 : 0;
  return mrb_test(v) ? 1 : 0;
}

static int ropt_present(mrb_state *m, mrb_value h, const char *key) {
  return !mrb_nil_p(h_get(m, h, key));
}

static int ropt_ints(mrb_state *m, mrb_value h, const char *key,
                     int *out, int max, int mask) {
  mrb_value v = h_get(m, h, key);
  if (!mrb_array_p(v)) return 0;
  int n = 0;
  const mrb_int len = RARRAY_LEN(v);
  for (mrb_int i = 0; i < len && n < max; i++) {
    mrb_value item = mrb_ary_ref(m, v, i);
    if (mrb_integer_p(item)) out[n++] = (int)mrb_integer(item) & mask;
    else if (mrb_float_p(item)) out[n++] = (int)mrb_float(item) & mask;
  }
  return n;
}

static void rh_set(mrb_state *m, mrb_value h, const char *key, mrb_value v) {
  mrb_hash_set(m, h, mrb_symbol_value(mrb_intern_cstr(m, key)), v);
}

static mrb_value bounds_array(mrb_state *m, int ok, const int b[4]) {
  if (!ok) return mrb_nil_value();
  mrb_value a = mrb_ary_new_capa(m, 4);
  for (int i = 0; i < 4; i++) mrb_ary_push(m, a, mrb_int_value(m, b[i]));
  return a;
}

typedef struct {
  const char *name;          /* Ruby module name, "NES" */
  const char *lname;         /* error-message spelling, "nes" */
  ab_prof_id id;
  const char *key_field;
  const char *key_desc;
  int tile_mask;
} rbp_desc;

static const rbp_desc RB_PROFS[AB_PROF_COUNT] = {
  [AB_PROF_NES] = { "NES", "nes", AB_PROF_NES, "tiles", "tile ids", ~0 },
  [AB_PROF_GB]  = { "GB",  "gb",  AB_PROF_GB,  "tiles", "tile ids", ~0 },
  /* v1 LIMITATION, on purpose: the kit registry keys tiles 0..255 -- see
   * the md profile notes in runtimes/lua/README.md. */
  [AB_PROF_MD]  = { "MD",  "md",  AB_PROF_MD,  "tiles", "tile ids", 0xFF },
  [AB_PROF_MSX] = { "MSX", "msx", AB_PROF_MSX, "tiles", "sprite pattern ids", ~0 },
  [AB_PROF_PCE] = { "PCE", "pce", AB_PROF_PCE, "patterns", "sprite pattern numbers", ~0 },
};

static void ensure_bound(mrb_state *m, const rbp_desc *d) {
  if (!ab_prof_bound(d->id))
    mrb_raisef(m, exc_runtime(m), "%s: call %s.bind in init first",
               d->lname, d->name);
}

/* mruby's method table gives us no userdata pointer, so each profile's
 * methods resolve their descriptor from the receiver module's name. */
static const rbp_desc *desc_for(mrb_state *m, mrb_value self) {
  mrb_value name = mrb_funcall(m, self, "to_s", 0);
  const char *s = mrb_string_p(name) ? RSTRING_PTR(name) : "";
  for (int i = 0; i < AB_PROF_COUNT; i++)
    if (strcmp(s, RB_PROFS[i].name) == 0) return &RB_PROFS[i];
  mrb_raise(m, exc_runtime(m), "redraw profile: unknown receiver");
  return NULL;   /* unreachable */
}

static mrb_value prof_replace_sprite(mrb_state *m, mrb_value self) {
  const rbp_desc *d = desc_for(m, self);
  ensure_bound(m, d);
  mrb_value opts;
  mrb_get_args(m, "H", &opts);

  ab_sub_rule rule;
  memset(&rule, 0, sizeof(rule));
  rule.tile_count = ropt_ints(m, opts, d->key_field, rule.tiles,
                              AB_SUB_MAX_TILES, d->tile_mask);
  if (rule.tile_count <= 0 && strcmp(d->key_field, "tiles") != 0)
    rule.tile_count = ropt_ints(m, opts, "tiles", rule.tiles,
                                AB_SUB_MAX_TILES, d->tile_mask);
  if (rule.tile_count <= 0)
    mrb_raisef(m, exc_argument(m),
               "%s.replace_sprite: `%s` must be a non-empty Array of %s",
               d->name, d->key_field, d->key_desc);
  rule.exclude_count = ropt_ints(m, opts, "anchor_exclude", rule.anchor_exclude,
                                 AB_SUB_MAX_TILES, d->tile_mask);

  mrb_value img = h_get(m, opts, "image");
  if (!mrb_hash_p(img))
    mrb_raisef(m, exc_argument(m),
               "%s.replace_sprite: `image` must be the Hash returned "
               "by AB.image", d->name);
  rule.texture = (int32_t)ropt_int(m, img, "texture", 0);
  rule.tex_w   = ropt_int(m, img, "width", 0);
  rule.tex_h   = ropt_int(m, img, "height", 0);

  rule.base_w = ropt_int(m, opts, "base_w", 0);
  rule.base_h = ropt_int(m, opts, "base_h", 0);
  rule.ring   = (double)ropt_int(m, opts, "ring", 0);

  const int id = ab_prof_add_rule(d->id, &rule);
  if (!id)
    mrb_raisef(m, exc_runtime(m),
               "%s.replace_sprite: registry full or invalid rule", d->name);
  return mrb_int_value(m, id);
}

static mrb_value prof_remove(mrb_state *m, mrb_value self) {
  const rbp_desc *d = desc_for(m, self);
  ensure_bound(m, d);
  mrb_int id;
  mrb_get_args(m, "i", &id);
  return mrb_bool_value(ab_prof_remove_rule(d->id, (int)id) != 0);
}

static mrb_value prof_clear(mrb_state *m, mrb_value self) {
  const rbp_desc *d = desc_for(m, self);
  ensure_bound(m, d);
  ab_prof_clear_rules(d->id);
  return mrb_nil_value();
}

/* Reads x/y/scale plus the optional layer-split surfaces. Raises when the
 * caller asks for a split this profile cannot do, rather than quietly
 * drawing the whole picture into both surfaces (which looks like it
 * worked). */
static void read_view(mrb_state *m, mrb_value opts, ab_prof_view *v,
                      double def_scale, ab_prof_id prof) {
  v->x = 0; v->y = 0; v->scale = def_scale;
  v->bg_surface = 0; v->spr_surface = 0;
  ropt_num(m, opts, "x", &v->x);
  ropt_num(m, opts, "y", &v->y);
  ropt_num(m, opts, "scale", &v->scale);
  v->bg_surface  = ropt_int(m, opts, "bg_surface", 0);
  v->spr_surface = ropt_int(m, opts, "spr_surface", 0);
  if ((v->bg_surface || v->spr_surface) && !ab_prof_layers_supported(prof))
    mrb_raise(m, exc_runtime(m),
      "draw: bg_surface/spr_surface not supported on this profile "
      "(its layers are resolved per pixel by the core, so there is "
      "no separate sprite batch to route)");
}

static mrb_value opt_hash_arg(mrb_state *m) {
  mrb_value opts = mrb_nil_value();
  mrb_get_args(m, "|H", &opts);
  return opts;
}

/* ---------------------------------------------------------------- NES -- */

static mrb_value nes_bind(mrb_state *m, mrb_value self) {
  (void)self;
  const char *err = NULL;
  if (!ab_prof_nes_bind(&err)) mrb_raisef(m, exc_runtime(m), "%s", err);
  return mrb_true_value();
}

static mrb_value nes_draw(mrb_state *m, mrb_value self) {
  (void)self;
  ensure_bound(m, &RB_PROFS[AB_PROF_NES]);
  mrb_value opts = opt_hash_arg(m);
  ab_prof_view v;
  read_view(m, opts, &v, 4.0, AB_PROF_NES);
  ab_prof_nes_result r;
  const char *err = NULL;
  if (!ab_prof_nes_draw(&v, &r, &err)) return mrb_nil_value();
  mrb_value h = mrb_hash_new_capa(m, 4);
  rh_set(m, h, "bg_quads", mrb_int_value(m, r.bg_quads));
  rh_set(m, h, "spr_quads", mrb_int_value(m, r.spr_quads));
  rh_set(m, h, "hd_drawn", mrb_int_value(m, r.hd_drawn));
  rh_set(m, h, "sprites_replaced", mrb_int_value(m, r.sprites_replaced));
  return h;
}

static mrb_value nes_bounds(mrb_state *m, mrb_value self) {
  (void)self;
  ensure_bound(m, &RB_PROFS[AB_PROF_NES]);
  int b[4];
  return bounds_array(m, ab_prof_nes_sprite_bounds(b), b);
}

/* ----------------------------------------------------------------- GB -- */

static mrb_value gb_bind(mrb_state *m, mrb_value self) {
  (void)self;
  const char *err = NULL;
  if (!ab_prof_gb_bind(&err)) mrb_raisef(m, exc_runtime(m), "%s", err);
  return mrb_true_value();
}

static mrb_value gb_draw(mrb_state *m, mrb_value self) {
  (void)self;
  ensure_bound(m, &RB_PROFS[AB_PROF_GB]);
  mrb_value opts = opt_hash_arg(m);
  ab_prof_view v;
  read_view(m, opts, &v, 7.0, AB_PROF_GB);
  ab_prof_gb_result r;
  const char *err = NULL;
  if (!ab_prof_gb_draw(&v, &r, &err)) return mrb_nil_value();
  mrb_value h = mrb_hash_new_capa(m, 4);
  rh_set(m, h, "bg_quads", mrb_int_value(m, r.bg_quads));
  rh_set(m, h, "spr_quads", mrb_int_value(m, r.spr_quads));
  rh_set(m, h, "hd_drawn", mrb_int_value(m, r.hd_drawn));
  rh_set(m, h, "sprites_replaced", mrb_int_value(m, r.sprites_replaced));
  return h;
}

static mrb_value gb_bounds(mrb_state *m, mrb_value self) {
  (void)self;
  ensure_bound(m, &RB_PROFS[AB_PROF_GB]);
  int b[4];
  return bounds_array(m, ab_prof_gb_sprite_bounds(b), b);
}

/* ----------------------------------------------------------------- MD -- */

static mrb_value md_bind(mrb_state *m, mrb_value self) {
  (void)self;
  const char *err = NULL;
  if (!ab_prof_md_bind(&err)) mrb_raisef(m, exc_runtime(m), "%s", err);
  return mrb_true_value();
}

static mrb_value md_draw(mrb_state *m, mrb_value self) {
  (void)self;
  ensure_bound(m, &RB_PROFS[AB_PROF_MD]);
  mrb_value opts = opt_hash_arg(m);
  ab_prof_view v;
  read_view(m, opts, &v, 4.0, AB_PROF_MD);
  ab_prof_md_result r;
  const char *err = NULL;
  if (!ab_prof_md_draw(&v, &r, &err)) return mrb_nil_value();
  mrb_value h = mrb_hash_new_capa(m, 3);
  rh_set(m, h, "quads", mrb_int_value(m, r.quads));
  rh_set(m, h, "hd_drawn", mrb_int_value(m, r.hd_drawn));
  rh_set(m, h, "sprites_replaced", mrb_int_value(m, r.sprites_replaced));
  return h;
}

static mrb_value md_bounds(mrb_state *m, mrb_value self) {
  (void)self;
  ensure_bound(m, &RB_PROFS[AB_PROF_MD]);
  int b[4];
  return bounds_array(m, ab_prof_md_sprite_bounds(b), b);
}

/* ---------------------------------------------------------------- MSX -- */

static mrb_value msx_bind(mrb_state *m, mrb_value self) {
  (void)self;
  const char *err = NULL;
  if (!ab_prof_msx_bind(&err)) mrb_raisef(m, exc_runtime(m), "%s", err);
  return mrb_true_value();
}

/* MSX.mode -> [screen_mode_number, description] | [nil, reason] | nil */
static mrb_value msx_mode(mrb_state *m, mrb_value self) {
  (void)self;
  ensure_bound(m, &RB_PROFS[AB_PROF_MSX]);
  int mode = 0;
  const char *desc = NULL;
  mrb_value a;
  switch (ab_prof_msx_mode(&mode, &desc)) {
    case AB_PROF_MSX_MODE_OK:
      a = mrb_ary_new_capa(m, 2);
      mrb_ary_push(m, a, mrb_int_value(m, mode));
      mrb_ary_push(m, a, mrb_str_new_cstr(m, desc));
      return a;
    case AB_PROF_MSX_MODE_UNSUPPORTED:
      a = mrb_ary_new_capa(m, 2);
      mrb_ary_push(m, a, mrb_nil_value());
      mrb_ary_push(m, a, mrb_str_new_cstr(m, desc));
      return a;
    default:
      return mrb_nil_value();
  }
}

static mrb_value msx_draw(mrb_state *m, mrb_value self) {
  (void)self;
  ensure_bound(m, &RB_PROFS[AB_PROF_MSX]);
  mrb_value opts = opt_hash_arg(m);
  ab_prof_msx_view v;
  read_view(m, opts, &v.v, 3.0, AB_PROF_MSX);
  v.fit_width = ropt_bool(m, opts, "fit_width", 0);
  ab_prof_msx_result r;
  const char *err = NULL;
  if (!ab_prof_msx_draw(&v, &r, &err)) return mrb_nil_value();

  if (!r.supported) {
    mrb_value h = mrb_hash_new_capa(m, 5);
    rh_set(m, h, "quads", mrb_int_value(m, 0));
    rh_set(m, h, "hd_drawn", mrb_int_value(m, 0));
    rh_set(m, h, "sprites_replaced", mrb_int_value(m, 0));
    rh_set(m, h, "supported", mrb_false_value());
    rh_set(m, h, "mode", mrb_nil_value());
    return h;
  }

  mrb_value h = mrb_hash_new_capa(m, 9);
  rh_set(m, h, "quads", mrb_int_value(m, r.quads));
  rh_set(m, h, "hd_drawn", mrb_int_value(m, r.hd_drawn));
  rh_set(m, h, "sprites_replaced", mrb_int_value(m, r.sprites_replaced));
  rh_set(m, h, "supported", mrb_true_value());
  rh_set(m, h, "mode", mrb_int_value(m, r.mode));
  rh_set(m, h, "width", mrb_int_value(m, r.width));
  rh_set(m, h, "per_line", mrb_bool_value(r.per_line != 0));
  rh_set(m, h, "vram_replay", mrb_bool_value(r.vram_replay != 0));
  rh_set(m, h, "retained", mrb_bool_value(r.retained != 0));
  return h;
}

static mrb_value msx_sprites(mrb_state *m, mrb_value self) {
  (void)self;
  ensure_bound(m, &RB_PROFS[AB_PROF_MSX]);
  ab_prof_msx_sprite spr[AB_MSX_SPRITES];
  const int n = ab_prof_msx_sprites(spr, AB_MSX_SPRITES);
  if (n < 0) return mrb_nil_value();
  mrb_value list = mrb_ary_new_capa(m, n);
  for (int i = 0; i < n; i++) {
    mrb_value h = mrb_hash_new_capa(m, 5);
    rh_set(m, h, "index", mrb_int_value(m, spr[i].index));
    rh_set(m, h, "x", mrb_int_value(m, spr[i].x));
    rh_set(m, h, "y", mrb_int_value(m, spr[i].y));
    rh_set(m, h, "pattern", mrb_int_value(m, spr[i].pattern));
    rh_set(m, h, "colour", mrb_int_value(m, spr[i].colour));
    mrb_ary_push(m, list, h);
  }
  return list;
}

static mrb_value msx_bounds(mrb_state *m, mrb_value self) {
  (void)self;
  ensure_bound(m, &RB_PROFS[AB_PROF_MSX]);
  int b[4];
  return bounds_array(m, ab_prof_msx_sprite_bounds(b), b);
}

/* ---------------------------------------------------------------- PCE -- */

static mrb_value pce_bind(mrb_state *m, mrb_value self) {
  (void)self;
  const char *err = NULL;
  if (!ab_prof_pce_bind(&err)) mrb_raisef(m, exc_runtime(m), "%s", err);
  return mrb_true_value();
}

static mrb_value pce_draw(mrb_state *m, mrb_value self) {
  (void)self;
  ensure_bound(m, &RB_PROFS[AB_PROF_PCE]);
  mrb_value opts = opt_hash_arg(m);
  ab_prof_pce_view v;
  ab_prof_pce_view_init(&v);
  if (mrb_hash_p(opts)) {
    ropt_num(m, opts, "x", &v.v.x);
    ropt_num(m, opts, "y", &v.v.y);
    ropt_num(m, opts, "scale", &v.v.scale);
    v.height = ropt_int(m, opts, "height", 224);
    v.force_bg = ropt_tri(m, opts, "bg");
    v.force_sprites = ropt_tri(m, opts, "sprites");
    v.fb_width = ropt_int(m, opts, "fb_width", 0);
    if (ropt_present(m, opts, "pal_delta_row"))
      v.pal_delta_row = ropt_int(m, opts, "pal_delta_row", 0);
    if (ropt_present(m, opts, "pal_delta"))
      v.pal_delta = ropt_int(m, opts, "pal_delta", 0);
    v.no_linepix = ropt_tri(m, opts, "no_linepix");
    v.no_paldeltas = ropt_tri(m, opts, "no_paldeltas");
  }
  ab_prof_pce_result r;
  const char *err = NULL;
  if (!ab_prof_pce_draw(&v, &r, &err)) return mrb_nil_value();
  mrb_value h = mrb_hash_new_capa(m, 5);
  rh_set(m, h, "quads", mrb_int_value(m, r.quads));
  rh_set(m, h, "hd_drawn", mrb_int_value(m, r.hd_drawn));
  rh_set(m, h, "sprites_replaced", mrb_int_value(m, r.sprites_replaced));
  rh_set(m, h, "width", mrb_int_value(m, r.width));
  rh_set(m, h, "height", mrb_int_value(m, r.height));
  return h;
}

/* PCE.sprite_bounds(height = 224, fb_width = 0) */
static mrb_value pce_bounds(mrb_state *m, mrb_value self) {
  (void)self;
  ensure_bound(m, &RB_PROFS[AB_PROF_PCE]);
  mrb_int height = 224, fbw = 0;
  mrb_get_args(m, "|ii", &height, &fbw);
  int b[4];
  return bounds_array(m, ab_prof_pce_sprite_bounds((int)height, (int)fbw, b), b);
}

static mrb_value pce_geometry(mrb_state *m, mrb_value self) {
  (void)self;
  ensure_bound(m, &RB_PROFS[AB_PROF_PCE]);
  ab_prof_pce_geometry g;
  if (!ab_prof_pce_get_geometry(&g)) return mrb_nil_value();
  mrb_value h = mrb_hash_new_capa(m, 7);
  rh_set(m, h, "width", mrb_int_value(m, g.width));
  rh_set(m, h, "bg", mrb_bool_value(g.bg != 0));
  rh_set(m, h, "sprites", mrb_bool_value(g.sprites != 0));
  rh_set(m, h, "bat_w", mrb_int_value(m, g.bat_w));
  rh_set(m, h, "bat_h", mrb_int_value(m, g.bat_h));
  rh_set(m, h, "scroll_x", mrb_int_value(m, g.scroll_x));
  rh_set(m, h, "scroll_y", mrb_int_value(m, g.scroll_y));
  return h;
}

/* --------------------------------------------------------------- SNES -- */

static void snes_ensure_bound(mrb_state *m) {
  if (!ab_prof_snes_bound())
    mrb_raise(m, exc_runtime(m), "snes: call SNES.bind in init first");
}

static mrb_value snes_bind(mrb_state *m, mrb_value self) {
  (void)self;
  const char *err = NULL;
  if (!ab_prof_snes_bind(&err)) mrb_raisef(m, exc_runtime(m), "%s", err);
  return mrb_true_value();
}

/* SNES.set_hd_tiles(blob) -> [indexed_count, rgba_count] */
static mrb_value snes_set_hd_tiles(mrb_state *m, mrb_value self) {
  (void)self;
  snes_ensure_bound(m);
  const char *blob;
  mrb_int n;
  mrb_get_args(m, "s", &blob, &n);
  int idxn = 0, rgban = 0;
  const char *err = NULL;
  if (!ab_prof_snes_set_hd_tiles(blob, (size_t)n, &idxn, &rgban, &err))
    mrb_raisef(m, exc_argument(m), "%s", err);
  mrb_value a = mrb_ary_new_capa(m, 2);
  mrb_ary_push(m, a, mrb_int_value(m, idxn));
  mrb_ary_push(m, a, mrb_int_value(m, rgban));
  return a;
}

static mrb_value snes_tick(mrb_state *m, mrb_value self) {
  (void)self;
  snes_ensure_bound(m);
  mrb_value opts = opt_hash_arg(m);
  int compare = 1;
  if (mrb_hash_p(opts) && ropt_present(m, opts, "compare"))
    compare = ropt_bool(m, opts, "compare", 1);
  ab_prof_snes_tick_result r;
  const char *err = NULL;
  switch (ab_prof_snes_tick(compare, &r, &err)) {
    case AB_PROF_SNES_NOT_READY:
      return mrb_nil_value();
    case AB_PROF_SNES_PLAIN:
      return mrb_hash_new(m);
    case AB_PROF_SNES_M7: {
      mrb_value h = mrb_hash_new_capa(m, 5);
      rh_set(m, h, "w", mrb_int_value(m, r.w));
      rh_set(m, h, "h", mrb_int_value(m, r.h));
      rh_set(m, h, "m7start", mrb_int_value(m, r.m7start));
      rh_set(m, h, "m7stop", mrb_int_value(m, r.m7stop));
      rh_set(m, h, "plane_rebuilt", mrb_bool_value(r.plane_rebuilt != 0));
      return h;
    }
    default:
      mrb_raisef(m, exc_runtime(m), "%s", err ? err : "snes: tick failed");
      return mrb_nil_value();   /* unreachable */
  }
}

static mrb_value snes_draw(mrb_state *m, mrb_value self) {
  (void)self;
  snes_ensure_bound(m);
  mrb_value opts = opt_hash_arg(m);
  double x = 0, y = 0, scale = 1;
  ropt_num(m, opts, "x", &x);
  ropt_num(m, opts, "y", &y);
  ropt_num(m, opts, "scale", &scale);
  ab_prof_snes_draw_result r;
  const char *err = NULL;
  if (!ab_prof_snes_draw(x, y, scale, &r, &err)) {
    if (err) mrb_raisef(m, exc_runtime(m), "%s", err);
    return mrb_nil_value();
  }
  mrb_value h = mrb_hash_new_capa(m, 3);
  rh_set(m, h, "w", mrb_int_value(m, r.w));
  rh_set(m, h, "h", mrb_int_value(m, r.h));
  rh_set(m, h, "quads", mrb_int_value(m, r.quads));
  return h;
}

/* SNES.frame_size -> [width, lines] | nil */
static mrb_value snes_frame_size(mrb_state *m, mrb_value self) {
  (void)self;
  snes_ensure_bound(m);
  int w = 0, h = 0;
  if (!ab_prof_snes_frame_size(&w, &h)) return mrb_nil_value();
  mrb_value a = mrb_ary_new_capa(m, 2);
  mrb_ary_push(m, a, mrb_int_value(m, w));
  mrb_ary_push(m, a, mrb_int_value(m, h));
  return a;
}

/* -------------------------------------------------------------- wiring -- */

typedef struct { const char *name; mrb_func_t fn; mrb_aspec spec; } prof_binding;

static struct RClass *def_module(mrb_state *m, const char *name,
                                 const prof_binding *fns, int count) {
  struct RClass *mod = mrb_define_module(m, name);
  for (int i = 0; i < count; i++)
    mrb_define_module_function(m, mod, fns[i].name, fns[i].fn, fns[i].spec);
  return mod;
}

static void def_const(mrb_state *m, struct RClass *mod, const char *name,
                      int value) {
  mrb_define_const(m, mod, name, mrb_int_value(m, value));
}

void ab_profiles_rb_define(mrb_state *m) {
  struct RClass *mod;

  static const prof_binding NES_FNS[] = {
    { "bind", nes_bind, MRB_ARGS_NONE() },
    { "replace_sprite", prof_replace_sprite, MRB_ARGS_REQ(1) },
    { "remove_replacement", prof_remove, MRB_ARGS_REQ(1) },
    { "clear_replacements", prof_clear, MRB_ARGS_NONE() },
    { "draw", nes_draw, MRB_ARGS_OPT(1) },
    { "sprite_bounds", nes_bounds, MRB_ARGS_NONE() },
  };
  mod = def_module(m, "NES", NES_FNS, (int)(sizeof(NES_FNS) / sizeof(NES_FNS[0])));
  def_const(m, mod, "WIDTH", AB_NES_W);
  def_const(m, mod, "HEIGHT", AB_NES_H);
  def_const(m, mod, "OVERSCAN_TOP", AB_NES_OVERSCAN_TOP);

  static const prof_binding GB_FNS[] = {
    { "bind", gb_bind, MRB_ARGS_NONE() },
    { "replace_sprite", prof_replace_sprite, MRB_ARGS_REQ(1) },
    { "remove_replacement", prof_remove, MRB_ARGS_REQ(1) },
    { "clear_replacements", prof_clear, MRB_ARGS_NONE() },
    { "draw", gb_draw, MRB_ARGS_OPT(1) },
    { "sprite_bounds", gb_bounds, MRB_ARGS_NONE() },
  };
  mod = def_module(m, "GB", GB_FNS, (int)(sizeof(GB_FNS) / sizeof(GB_FNS[0])));
  def_const(m, mod, "WIDTH", AB_GB_W);
  def_const(m, mod, "HEIGHT", AB_GB_H);

  static const prof_binding MD_FNS[] = {
    { "bind", md_bind, MRB_ARGS_NONE() },
    { "replace_sprite", prof_replace_sprite, MRB_ARGS_REQ(1) },
    { "remove_replacement", prof_remove, MRB_ARGS_REQ(1) },
    { "clear_replacements", prof_clear, MRB_ARGS_NONE() },
    { "draw", md_draw, MRB_ARGS_OPT(1) },
    { "sprite_bounds", md_bounds, MRB_ARGS_NONE() },
  };
  mod = def_module(m, "MD", MD_FNS, (int)(sizeof(MD_FNS) / sizeof(MD_FNS[0])));
  def_const(m, mod, "MAX_WIDTH", AB_MD_MAX_W);
  def_const(m, mod, "MAX_HEIGHT", AB_MD_MAX_H);

  static const prof_binding SNES_FNS[] = {
    { "bind", snes_bind, MRB_ARGS_NONE() },
    { "draw", snes_draw, MRB_ARGS_OPT(1) },
    { "frame_size", snes_frame_size, MRB_ARGS_NONE() },
    { "set_hd_tiles", snes_set_hd_tiles, MRB_ARGS_REQ(1) },
    { "tick", snes_tick, MRB_ARGS_OPT(1) },
  };
  def_module(m, "SNES", SNES_FNS, (int)(sizeof(SNES_FNS) / sizeof(SNES_FNS[0])));

  static const prof_binding MSX_FNS[] = {
    { "bind", msx_bind, MRB_ARGS_NONE() },
    { "replace_sprite", prof_replace_sprite, MRB_ARGS_REQ(1) },
    { "remove_replacement", prof_remove, MRB_ARGS_REQ(1) },
    { "clear_replacements", prof_clear, MRB_ARGS_NONE() },
    { "draw", msx_draw, MRB_ARGS_OPT(1) },
    { "mode", msx_mode, MRB_ARGS_NONE() },
    { "sprites", msx_sprites, MRB_ARGS_NONE() },
    { "sprite_bounds", msx_bounds, MRB_ARGS_NONE() },
  };
  mod = def_module(m, "MSX", MSX_FNS, (int)(sizeof(MSX_FNS) / sizeof(MSX_FNS[0])));
  def_const(m, mod, "WIDTH", AB_MSX_W);
  /* SCREEN 6/7 frames are MAX_WIDTH wide; MSX.draw reports the actual width
   * of the frame it just drew so a bezel does not have to guess. */
  def_const(m, mod, "MAX_WIDTH", AB_MSX_MAXW);
  def_const(m, mod, "HEIGHT", AB_MSX_H);
  def_const(m, mod, "BORDER", AB_MSX_BORDER);
  def_const(m, mod, "DISPLAY_WIDTH", AB_MSX_DISPLAY);

  static const prof_binding PCE_FNS[] = {
    { "bind", pce_bind, MRB_ARGS_NONE() },
    { "replace_sprite", prof_replace_sprite, MRB_ARGS_REQ(1) },
    { "remove_replacement", prof_remove, MRB_ARGS_REQ(1) },
    { "clear_replacements", prof_clear, MRB_ARGS_NONE() },
    { "draw", pce_draw, MRB_ARGS_OPT(1) },
    { "sprite_bounds", pce_bounds, MRB_ARGS_OPT(2) },
    { "geometry", pce_geometry, MRB_ARGS_NONE() },
  };
  mod = def_module(m, "PCE", PCE_FNS, (int)(sizeof(PCE_FNS) / sizeof(PCE_FNS[0])));
  /* WIDTH is the common case, not a constant: the VDC's display width is
   * programmable and PCE.geometry[:width] is the live value. */
  def_const(m, mod, "WIDTH", 256);
  def_const(m, mod, "HEIGHT", 224);
}

/* Free everything the profiles own. Call from the runtime's shutdown. */
void ab_profiles_rb_shutdown(void) {
  ab_prof_nes_shutdown();
  ab_prof_gb_shutdown();
  ab_prof_md_shutdown();
  ab_prof_msx_shutdown();
  ab_prof_pce_shutdown();
  ab_prof_snes_shutdown();
}
