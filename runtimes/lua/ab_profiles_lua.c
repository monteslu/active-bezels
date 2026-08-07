/*
 * ab_profiles_lua.c -- Lua marshaling for the platform redraw profiles.
 *
 * Exposes the `nes`, `gb`, `md`, `snes`, `msx`, `pce` globals. ALL logic
 * lives in runtimes/common/ab_profiles.c (shared with the Python, JS and
 * Ruby runtimes); this file only converts Lua tables to the core's structs
 * and back. Behaviour -- return shapes, error strings, which failures are
 * hard errors versus nil+reason -- matches the original per-platform
 * bindings exactly, because bezels and the certification harness assert on
 * those shapes.
 */
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "../../sdk/active_bezel.h"
#include "ab_render.h"
#include "ab_profiles.h"
#include "ab_nes.h"
#include "ab_gb.h"
#include "ab_md.h"
#include "ab_msx.h"

/* ------------------------------------------------------------- helpers -- */

/* Read an integer array field into `out`, returning the count. */
static int table_ints(lua_State *S, int idx, const char *field,
                      int *out, int max, int mask) {
  lua_getfield(S, idx, field);
  if (!lua_istable(S, -1)) { lua_pop(S, 1); return 0; }
  int n = 0;
  const lua_Integer len = (lua_Integer)lua_rawlen(S, -1);
  for (lua_Integer i = 1; i <= len && n < max; i++) {
    lua_rawgeti(S, -1, i);
    if (lua_isinteger(S, -1) || lua_isnumber(S, -1))
      out[n++] = (int)lua_tointeger(S, -1) & mask;
    lua_pop(S, 1);
  }
  lua_pop(S, 1);
  return n;
}

static int field_int(lua_State *S, int idx, const char *field, int def) {
  lua_getfield(S, idx, field);
  int v = def;
  if (lua_isnumber(S, -1)) v = (int)lua_tointeger(S, -1);
  lua_pop(S, 1);
  return v;
}

static void field_num(lua_State *S, int idx, const char *field, double *v) {
  lua_getfield(S, idx, field);
  if (lua_isnumber(S, -1)) *v = lua_tonumber(S, -1);
  lua_pop(S, 1);
}

/* Tri-state field: absent = -1 (follow the registers), else 0/1. */
static int field_tri(lua_State *S, int idx, const char *field) {
  lua_getfield(S, idx, field);
  int v = -1;
  if (lua_isboolean(S, -1)) v = lua_toboolean(S, -1) ? 1 : 0;
  else if (lua_isnumber(S, -1)) v = (int)lua_tointeger(S, -1) ? 1 : 0;
  lua_pop(S, 1);
  return v;
}

/* Reads x/y/scale plus the optional layer-split surfaces. Returns 0 and
 * leaves *err set when the caller asked for a split this profile cannot do;
 * the draw shim then fails loudly rather than quietly drawing the whole
 * picture into both surfaces (which looks like it worked). */
static int read_view(lua_State *S, ab_prof_view *v, double def_scale,
                     ab_prof_id prof, const char **err) {
  v->x = 0; v->y = 0; v->scale = def_scale;
  v->bg_surface = 0; v->spr_surface = 0; v->solid_surface = 0;
  *err = NULL;
  if (lua_istable(S, 1)) {
    field_num(S, 1, "x", &v->x);
    field_num(S, 1, "y", &v->y);
    field_num(S, 1, "scale", &v->scale);
    v->bg_surface  = field_int(S, 1, "bg_surface", 0);
    v->spr_surface = field_int(S, 1, "spr_surface", 0);
    v->solid_surface = field_int(S, 1, "solid_surface", 0);
    if ((v->bg_surface || v->spr_surface || v->solid_surface) &&
        !ab_prof_layers_supported(prof)) {
      *err = "draw: bg_surface/spr_surface not supported on this profile "
             "(its layers are resolved per pixel by the core, so there is "
             "no separate sprite batch to route)";
      return 0;
    }
  }
  return 1;
}

static int push_nil_msg(lua_State *S, const char *msg) {
  lua_pushnil(S);
  lua_pushstring(S, msg);
  return 2;
}

/* Per-profile marshaling identity: name, id, and the substitution key's
 * spelling for error messages ("tile ids" vs "sprite pattern ids"). */
typedef struct {
  const char *name;
  ab_prof_id id;
  const char *key_desc;
  int tile_mask;
} prof_desc;

static const prof_desc PROFS[AB_PROF_COUNT] = {
  [AB_PROF_NES] = { "nes", AB_PROF_NES, "tile ids", ~0 },
  [AB_PROF_GB]  = { "gb",  AB_PROF_GB,  "tile ids", ~0 },
  /* v1 LIMITATION, on purpose: the kit registry keys tiles 0..255, Genesis
   * sprite tiles are 0..2047, so rules match on (tile & 0xFF). Fine while a
   * bezel targets one game whose replaced sprites it knows; widen the
   * registry before building a general library on this. */
  [AB_PROF_MD]  = { "md",  AB_PROF_MD,  "tile ids", 0xFF },
  /* `tiles` are SPRITE PATTERN ids. In 16x16 mode the hardware ignores the
   * low TWO bits, so register the aligned id (0x10, not 0x11) or the match
   * silently never fires. */
  [AB_PROF_MSX] = { "msx", AB_PROF_MSX, "sprite pattern ids", ~0 },
  [AB_PROF_PCE] = { "pce", AB_PROF_PCE, "sprite pattern numbers", ~0 },
};

static void ensure_bound(lua_State *S, const prof_desc *d) {
  if (!ab_prof_bound(d->id))
    luaL_error(S, "%s: call %s.bind() in init() first", d->name, d->name);
}

/* Shared replace_sprite{ tiles=..., image=..., anchor_exclude=..., ... }.
 * `image` is the table ab.image() returns ({texture, width, height}).
 * Returns a rule id, or nil + reason. PCE accepts `patterns` first with
 * `tiles` as the ported-bezel alias. */
static int prof_replace_sprite(lua_State *S, const prof_desc *d,
                               const char *primary_field) {
  ensure_bound(S, d);
  luaL_checktype(S, 1, LUA_TTABLE);

  ab_sub_rule rule;
  memset(&rule, 0, sizeof(rule));

  rule.tile_count = table_ints(S, 1, primary_field, rule.tiles,
                               AB_SUB_MAX_TILES, d->tile_mask);
  if (rule.tile_count <= 0 && strcmp(primary_field, "tiles") != 0)
    rule.tile_count = table_ints(S, 1, "tiles", rule.tiles,
                                 AB_SUB_MAX_TILES, d->tile_mask);
  if (rule.tile_count <= 0) {
    lua_pushnil(S);
    lua_pushfstring(S, "%s.replace_sprite: `%s` must be a non-empty array "
                       "of %s -- that is the substitution key",
                    d->name, primary_field, d->key_desc);
    return 2;
  }
  rule.exclude_count = table_ints(S, 1, "anchor_exclude", rule.anchor_exclude,
                                  AB_SUB_MAX_TILES, d->tile_mask);

  lua_getfield(S, 1, "image");
  if (!lua_istable(S, -1)) {
    lua_pop(S, 1);
    lua_pushnil(S);
    lua_pushfstring(S, "%s.replace_sprite: `image` must be the table "
                       "returned by ab.image()", d->name);
    return 2;
  }
  lua_getfield(S, -1, "texture"); rule.texture = (int32_t)lua_tointeger(S, -1); lua_pop(S, 1);
  lua_getfield(S, -1, "width");   rule.tex_w   = (int)lua_tointeger(S, -1);     lua_pop(S, 1);
  lua_getfield(S, -1, "height");  rule.tex_h   = (int)lua_tointeger(S, -1);     lua_pop(S, 1);
  lua_pop(S, 1);

  rule.base_w = field_int(S, 1, "base_w", 0);
  rule.base_h = field_int(S, 1, "base_h", 0);
  rule.ring   = (double)field_int(S, 1, "ring", 0);

  const int id = ab_prof_add_rule(d->id, &rule);
  if (!id) {
    lua_pushnil(S);
    lua_pushfstring(S, "%s.replace_sprite: registry full or invalid rule",
                    d->name);
    return 2;
  }
  lua_pushinteger(S, id);
  return 1;
}

static int prof_remove_replacement(lua_State *S, const prof_desc *d) {
  ensure_bound(S, d);
  lua_pushboolean(S, ab_prof_remove_rule(d->id, (int)luaL_checkinteger(S, 1)));
  return 1;
}

static int prof_clear_replacements(lua_State *S, const prof_desc *d) {
  ensure_bound(S, d);
  ab_prof_clear_rules(d->id);
  return 0;
}

static int push_bounds(lua_State *S, int ok, const int b[4]) {
  if (!ok) { lua_pushnil(S); return 1; }
  lua_pushinteger(S, b[0]); lua_pushinteger(S, b[1]);
  lua_pushinteger(S, b[2]); lua_pushinteger(S, b[3]);
  return 4;
}

/* ---------------------------------------------------------------- NES -- */

static int l_nes_bind(lua_State *S) {
  const char *err = NULL;
  if (!ab_prof_nes_bind(&err)) return push_nil_msg(S, err);
  lua_pushboolean(S, 1);
  return 1;
}

static int l_nes_replace_sprite(lua_State *S) {
  return prof_replace_sprite(S, &PROFS[AB_PROF_NES], "tiles");
}
static int l_nes_remove_replacement(lua_State *S) {
  return prof_remove_replacement(S, &PROFS[AB_PROF_NES]);
}
static int l_nes_clear_replacements(lua_State *S) {
  return prof_clear_replacements(S, &PROFS[AB_PROF_NES]);
}

/* nes.draw{ x=, y=, scale= } -> { bg_quads=, spr_quads=, hd_drawn= } */
static int l_nes_draw(lua_State *S) {
  ensure_bound(S, &PROFS[AB_PROF_NES]);
  ab_prof_view v;
  const char *err = NULL;
  if (!read_view(S, &v, 4.0, AB_PROF_NES, &err)) return push_nil_msg(S, err);
  ab_prof_nes_result r;
  if (!ab_prof_nes_draw(&v, &r, &err)) return push_nil_msg(S, err);
  lua_createtable(S, 0, 4);
  lua_pushinteger(S, r.bg_quads);         lua_setfield(S, -2, "bg_quads");
  lua_pushinteger(S, r.spr_quads);        lua_setfield(S, -2, "spr_quads");
  lua_pushinteger(S, r.hd_drawn);         lua_setfield(S, -2, "hd_drawn");
  lua_pushinteger(S, r.sprites_replaced); lua_setfield(S, -2, "sprites_replaced");
  return 1;
}

/* nes.hide_cell(cx, cy) -- mark one 8x8 BACKGROUND cell as not-to-be-drawn
 * for the next nes.draw. The redraw simply never emits those pixels, so the
 * guest can own that class of tiles outright: no erase, no paint-over, and
 * the tiles never receive whatever treatment the layer gets. Cleared by
 * every draw, so a guest re-marks each frame. */
/* nes.hide_sprite(slot) -- take one OAM slot out of the sprite pass. The
 * guest then draws that entity itself, from the clean frame, at whatever
 * point in the composite it wants. */
static int l_nes_hide_sprite(lua_State *S) {
  ensure_bound(S, &PROFS[AB_PROF_NES]);
  ab_prof_nes_hide_sprite((int)luaL_checkinteger(S, 1));
  return 0;
}

/* nes.isolate_sprite(slot) -- emit ONLY these slots this draw. Renders the
 * entity through the normal path onto whatever surface the draw targets,
 * rather than cutting its pixels out of the finished frame. */
static int l_nes_isolate_sprite(lua_State *S) {
  ensure_bound(S, &PROFS[AB_PROF_NES]);
  ab_prof_nes_isolate_sprite((int)luaL_checkinteger(S, 1));
  return 0;
}

static int l_nes_hide_cell(lua_State *S) {
  ensure_bound(S, &PROFS[AB_PROF_NES]);
  ab_prof_nes_hide_cell((int)luaL_checkinteger(S, 1),
                        (int)luaL_checkinteger(S, 2));
  return 0;
}

/* nes.sprite_bounds() -> x0,y0,x1,y1 of the currently matched metasprite, or
 * nil. Useful for bezels that want to draw their own overlay near a monster. */
static int l_nes_sprite_bounds(lua_State *S) {
  ensure_bound(S, &PROFS[AB_PROF_NES]);
  int b[4];
  return push_bounds(S, ab_prof_nes_sprite_bounds(b), b);
}

static const luaL_Reg NES_FUNCS[] = {
  { "bind",                l_nes_bind },
  { "replace_sprite",      l_nes_replace_sprite },
  { "remove_replacement",  l_nes_remove_replacement },
  { "clear_replacements",  l_nes_clear_replacements },
  { "draw",                l_nes_draw },
  { "sprite_bounds",       l_nes_sprite_bounds },
  { "hide_cell",           l_nes_hide_cell },
  { "hide_sprite",         l_nes_hide_sprite },
  { "isolate_sprite",      l_nes_isolate_sprite },
  { NULL, NULL }
};

/* ----------------------------------------------------------------- GB -- */

static int l_gb_bind(lua_State *S) {
  const char *err = NULL;
  if (!ab_prof_gb_bind(&err)) return push_nil_msg(S, err);
  lua_pushboolean(S, 1);
  return 1;
}

static int l_gb_replace_sprite(lua_State *S) {
  return prof_replace_sprite(S, &PROFS[AB_PROF_GB], "tiles");
}
static int l_gb_remove_replacement(lua_State *S) {
  return prof_remove_replacement(S, &PROFS[AB_PROF_GB]);
}
static int l_gb_clear_replacements(lua_State *S) {
  return prof_clear_replacements(S, &PROFS[AB_PROF_GB]);
}

static int l_gb_draw(lua_State *S) {
  ensure_bound(S, &PROFS[AB_PROF_GB]);
  ab_prof_view v;
  const char *err = NULL;
  if (!read_view(S, &v, 7.0, AB_PROF_GB, &err)) return push_nil_msg(S, err);
  ab_prof_gb_result r;
  if (!ab_prof_gb_draw(&v, &r, &err)) return push_nil_msg(S, err);
  lua_createtable(S, 0, 4);
  lua_pushinteger(S, r.bg_quads);         lua_setfield(S, -2, "bg_quads");
  lua_pushinteger(S, r.spr_quads);        lua_setfield(S, -2, "spr_quads");
  lua_pushinteger(S, r.hd_drawn);         lua_setfield(S, -2, "hd_drawn");
  lua_pushinteger(S, r.sprites_replaced); lua_setfield(S, -2, "sprites_replaced");
  return 1;
}

static int l_gb_sprite_bounds(lua_State *S) {
  ensure_bound(S, &PROFS[AB_PROF_GB]);
  int b[4];
  return push_bounds(S, ab_prof_gb_sprite_bounds(b), b);
}

static const luaL_Reg GB_FUNCS[] = {
  { "bind",                l_gb_bind },
  { "replace_sprite",      l_gb_replace_sprite },
  { "remove_replacement",  l_gb_remove_replacement },
  { "clear_replacements",  l_gb_clear_replacements },
  { "draw",                l_gb_draw },
  { "sprite_bounds",       l_gb_sprite_bounds },
  { NULL, NULL }
};

/* ----------------------------------------------------------------- MD -- */

static int l_md_bind(lua_State *S) {
  const char *err = NULL;
  if (!ab_prof_md_bind(&err)) return push_nil_msg(S, err);
  lua_pushboolean(S, 1);
  return 1;
}

static int l_md_replace_sprite(lua_State *S) {
  return prof_replace_sprite(S, &PROFS[AB_PROF_MD], "tiles");
}
static int l_md_remove_replacement(lua_State *S) {
  return prof_remove_replacement(S, &PROFS[AB_PROF_MD]);
}
static int l_md_clear_replacements(lua_State *S) {
  return prof_clear_replacements(S, &PROFS[AB_PROF_MD]);
}

static int l_md_draw(lua_State *S) {
  ensure_bound(S, &PROFS[AB_PROF_MD]);
  ab_prof_view v;
  const char *err = NULL;
  if (!read_view(S, &v, 4.0, AB_PROF_MD, &err)) return push_nil_msg(S, err);
  ab_prof_md_result r;
  if (!ab_prof_md_draw(&v, &r, &err)) return push_nil_msg(S, err);
  lua_createtable(S, 0, 3);
  lua_pushinteger(S, r.quads);            lua_setfield(S, -2, "quads");
  lua_pushinteger(S, r.hd_drawn);         lua_setfield(S, -2, "hd_drawn");
  lua_pushinteger(S, r.sprites_replaced); lua_setfield(S, -2, "sprites_replaced");
  return 1;
}

static int l_md_sprite_bounds(lua_State *S) {
  ensure_bound(S, &PROFS[AB_PROF_MD]);
  int b[4];
  return push_bounds(S, ab_prof_md_sprite_bounds(b), b);
}

static const luaL_Reg MD_FUNCS[] = {
  { "bind",               l_md_bind },
  { "replace_sprite",     l_md_replace_sprite },
  { "remove_replacement", l_md_remove_replacement },
  { "clear_replacements", l_md_clear_replacements },
  { "draw",               l_md_draw },
  { "sprite_bounds",      l_md_sprite_bounds },
  { NULL, NULL }
};

/* ---------------------------------------------------------------- MSX -- */

static int l_msx_bind(lua_State *S) {
  const char *err = NULL;
  if (!ab_prof_msx_bind(&err)) return push_nil_msg(S, err);
  lua_pushboolean(S, 1);
  return 1;
}

static int l_msx_replace_sprite(lua_State *S) {
  return prof_replace_sprite(S, &PROFS[AB_PROF_MSX], "tiles");
}
static int l_msx_remove_replacement(lua_State *S) {
  return prof_remove_replacement(S, &PROFS[AB_PROF_MSX]);
}
static int l_msx_clear_replacements(lua_State *S) {
  return prof_clear_replacements(S, &PROFS[AB_PROF_MSX]);
}

/* msx.mode() -> screen_mode_number|nil, description
 * Lets a bezel wait for a mode it can decorate rather than drawing over the
 * BIOS boot screen. */
static int l_msx_mode(lua_State *S) {
  ensure_bound(S, &PROFS[AB_PROF_MSX]);
  int mode = 0;
  const char *desc = NULL;
  switch (ab_prof_msx_mode(&mode, &desc)) {
    case AB_PROF_MSX_MODE_OK:
      lua_pushinteger(S, mode);
      lua_pushstring(S, desc);
      return 2;
    case AB_PROF_MSX_MODE_UNSUPPORTED:
      return push_nil_msg(S, desc);
    default:
      lua_pushnil(S);
      return 1;
  }
}

/* msx.draw{ x=, y=, scale= } -> { quads=, hd_drawn=, sprites_replaced=,
 *                                 supported=, mode= } */
static int l_msx_draw(lua_State *S) {
  ensure_bound(S, &PROFS[AB_PROF_MSX]);
  ab_prof_msx_view v;
  const char *verr = NULL;
  if (!read_view(S, &v.v, 3.0, AB_PROF_MSX, &verr)) return push_nil_msg(S, verr);
  v.fit_width = 0;
  if (lua_istable(S, 1)) {
    lua_getfield(S, 1, "fit_width");
    if (lua_isboolean(S, -1)) v.fit_width = lua_toboolean(S, -1);
    lua_pop(S, 1);
  }
  ab_prof_msx_result r;
  const char *err = NULL;
  if (!ab_prof_msx_draw(&v, &r, &err)) return push_nil_msg(S, err);

  if (!r.supported) {
    lua_createtable(S, 0, 5);
    lua_pushinteger(S, 0); lua_setfield(S, -2, "quads");
    lua_pushinteger(S, 0); lua_setfield(S, -2, "hd_drawn");
    lua_pushinteger(S, 0); lua_setfield(S, -2, "sprites_replaced");
    lua_pushboolean(S, 0); lua_setfield(S, -2, "supported");
    lua_pushnil(S);        lua_setfield(S, -2, "mode");
    return 1;
  }

  lua_createtable(S, 0, 9);
  lua_pushinteger(S, r.quads);            lua_setfield(S, -2, "quads");
  lua_pushinteger(S, r.hd_drawn);         lua_setfield(S, -2, "hd_drawn");
  lua_pushinteger(S, r.sprites_replaced); lua_setfield(S, -2, "sprites_replaced");
  lua_pushboolean(S, 1);                  lua_setfield(S, -2, "supported");
  lua_pushinteger(S, r.mode);             lua_setfield(S, -2, "mode");
  lua_pushinteger(S, r.width);            lua_setfield(S, -2, "width");
  lua_pushboolean(S, r.per_line);         lua_setfield(S, -2, "per_line");
  lua_pushboolean(S, r.vram_replay);      lua_setfield(S, -2, "vram_replay");
  lua_pushboolean(S, r.retained);         lua_setfield(S, -2, "retained");
  return 1;
}

/* msx.sprite_bounds() -> x0,y0,x1,y1 of the currently matched metasprite,
 * in DISPLAY coordinates (0,0 = top-left of the 256x192 active area). */
static int l_msx_sprite_bounds(lua_State *S) {
  ensure_bound(S, &PROFS[AB_PROF_MSX]);
  int b[4];
  return push_bounds(S, ab_prof_msx_sprite_bounds(b), b);
}

/* msx.sprites() -> array of {index, x, y, pattern, colour} for the sprites the
 * attribute table actually lists. Stops at the y==208 terminator, like the
 * hardware -- entries after it are not "hidden sprites", they do not exist. */
static int l_msx_sprites(lua_State *S) {
  ensure_bound(S, &PROFS[AB_PROF_MSX]);
  ab_prof_msx_sprite spr[AB_MSX_SPRITES];
  const int n = ab_prof_msx_sprites(spr, AB_MSX_SPRITES);
  if (n < 0) { lua_pushnil(S); return 1; }
  lua_newtable(S);
  for (int i = 0; i < n; i++) {
    lua_createtable(S, 0, 5);
    lua_pushinteger(S, spr[i].index);   lua_setfield(S, -2, "index");
    lua_pushinteger(S, spr[i].x);       lua_setfield(S, -2, "x");
    lua_pushinteger(S, spr[i].y);       lua_setfield(S, -2, "y");
    lua_pushinteger(S, spr[i].pattern); lua_setfield(S, -2, "pattern");
    lua_pushinteger(S, spr[i].colour);  lua_setfield(S, -2, "colour");
    lua_rawseti(S, -2, i + 1);
  }
  return 1;
}

static const luaL_Reg MSX_FUNCS[] = {
  { "bind",                l_msx_bind },
  { "replace_sprite",      l_msx_replace_sprite },
  { "remove_replacement",  l_msx_remove_replacement },
  { "clear_replacements",  l_msx_clear_replacements },
  { "draw",                l_msx_draw },
  { "mode",                l_msx_mode },
  { "sprites",             l_msx_sprites },
  { "sprite_bounds",       l_msx_sprite_bounds },
  { NULL, NULL }
};

/* ---------------------------------------------------------------- PCE -- */

static int l_pce_bind(lua_State *S) {
  const char *err = NULL;
  if (!ab_prof_pce_bind(&err)) return push_nil_msg(S, err);
  lua_pushboolean(S, 1);
  return 1;
}

/* The substitution key is the PCE sprite PATTERN number ((SATB word 2) >> 1),
 * which is this platform's equivalent of a tile id. `tiles` is accepted as an
 * alias so a bezel ported from another console keeps working. */
static int l_pce_replace_sprite(lua_State *S) {
  return prof_replace_sprite(S, &PROFS[AB_PROF_PCE], "patterns");
}
static int l_pce_remove_replacement(lua_State *S) {
  return prof_remove_replacement(S, &PROFS[AB_PROF_PCE]);
}
static int l_pce_clear_replacements(lua_State *S) {
  return prof_clear_replacements(S, &PROFS[AB_PROF_PCE]);
}

/* pce.draw{ x=, y=, scale=, height=, bg=, sprites=, fb_width= }
 *   -> { quads=, hd_drawn=, sprites_replaced=, width=, height= } */
static int l_pce_draw(lua_State *S) {
  ensure_bound(S, &PROFS[AB_PROF_PCE]);
  ab_prof_pce_view v;
  ab_prof_pce_view_init(&v);
  if (lua_istable(S, 1)) {
    field_num(S, 1, "x", &v.v.x);
    field_num(S, 1, "y", &v.v.y);
    field_num(S, 1, "scale", &v.v.scale);
    v.height = field_int(S, 1, "height", 224);
    v.force_bg = field_tri(S, 1, "bg");
    v.force_sprites = field_tri(S, 1, "sprites");
    v.fb_width = field_int(S, 1, "fb_width", 0);
    /* Diagnostic/control: force the reconstruction path instead of the
     * core's resolved line buffer. */
    lua_getfield(S, 1, "pal_delta_row");
    if (lua_isnumber(S, -1)) v.pal_delta_row = (int)lua_tointeger(S, -1);
    lua_pop(S, 1);
    lua_getfield(S, 1, "pal_delta");
    if (lua_isnumber(S, -1)) v.pal_delta = (int)lua_tointeger(S, -1);
    lua_pop(S, 1);
    lua_getfield(S, 1, "no_linepix");
    if (lua_isboolean(S, -1)) v.no_linepix = lua_toboolean(S, -1);
    lua_pop(S, 1);
    lua_getfield(S, 1, "no_paldeltas");
    if (lua_isboolean(S, -1)) v.no_paldeltas = lua_toboolean(S, -1);
    lua_pop(S, 1);
  }
  ab_prof_pce_result r;
  const char *err = NULL;
  if (!ab_prof_pce_draw(&v, &r, &err)) return push_nil_msg(S, err);
  lua_createtable(S, 0, 5);
  lua_pushinteger(S, r.quads);            lua_setfield(S, -2, "quads");
  lua_pushinteger(S, r.hd_drawn);         lua_setfield(S, -2, "hd_drawn");
  lua_pushinteger(S, r.sprites_replaced); lua_setfield(S, -2, "sprites_replaced");
  lua_pushinteger(S, r.width);            lua_setfield(S, -2, "width");
  lua_pushinteger(S, r.height);           lua_setfield(S, -2, "height");
  return 1;
}

/* pce.sprite_bounds([height[, fb_width]]) -> x0,y0,x1,y1. Bounds must be
 * computed against the width the emitter walks, or the reported box is
 * offset from where the art will actually be drawn. */
static int l_pce_sprite_bounds(lua_State *S) {
  ensure_bound(S, &PROFS[AB_PROF_PCE]);
  const int height = (int)luaL_optinteger(S, 1, 224);
  const int fbw = (int)luaL_optinteger(S, 2, 0);
  int b[4];
  return push_bounds(S, ab_prof_pce_sprite_bounds(height, fbw, b), b);
}

/* pce.geometry() -> { width=, bg=, sprites=, bat_w=, bat_h= }
 * Lets a bezel see what the VDC is configured for without drawing. */
static int l_pce_geometry(lua_State *S) {
  ensure_bound(S, &PROFS[AB_PROF_PCE]);
  ab_prof_pce_geometry g;
  if (!ab_prof_pce_get_geometry(&g)) { lua_pushnil(S); return 1; }
  lua_createtable(S, 0, 7);
  lua_pushinteger(S, g.width);    lua_setfield(S, -2, "width");
  lua_pushboolean(S, g.bg);       lua_setfield(S, -2, "bg");
  lua_pushboolean(S, g.sprites);  lua_setfield(S, -2, "sprites");
  lua_pushinteger(S, g.bat_w);    lua_setfield(S, -2, "bat_w");
  lua_pushinteger(S, g.bat_h);    lua_setfield(S, -2, "bat_h");
  lua_pushinteger(S, g.scroll_x); lua_setfield(S, -2, "scroll_x");
  lua_pushinteger(S, g.scroll_y); lua_setfield(S, -2, "scroll_y");
  return 1;
}

static const luaL_Reg PCE_FUNCS[] = {
  { "bind",                l_pce_bind },
  { "replace_sprite",      l_pce_replace_sprite },
  { "remove_replacement",  l_pce_remove_replacement },
  { "clear_replacements",  l_pce_clear_replacements },
  { "draw",                l_pce_draw },
  { "sprite_bounds",       l_pce_sprite_bounds },
  { "geometry",            l_pce_geometry },
  { NULL, NULL }
};

/* --------------------------------------------------------------- SNES -- */

static int l_snes_bind(lua_State *S) {
  const char *err = NULL;
  if (!ab_prof_snes_bind(&err)) {
    /* Missing regions degrade (nil + reason); the plane allocation failing
     * is a hard error, matching the original binding. */
    if (strncmp(err, "snes: capture regions missing", 29) == 0)
      return push_nil_msg(S, err);
    return luaL_error(S, "%s", err);
  }
  lua_pushboolean(S, 1);
  return 1;
}

static void snes_ensure_bound(lua_State *S) {
  if (!ab_prof_snes_bound())
    luaL_error(S, "snes: call snes.bind() in init() first");
}

static int l_snes_set_hd_tiles(lua_State *S) {
  size_t n = 0;
  const char *blob = luaL_checklstring(S, 1, &n);
  int idxn = 0, rgban = 0;
  const char *err = NULL;
  if (!ab_prof_snes_set_hd_tiles(blob, n, &idxn, &rgban, &err))
    return luaL_error(S, "%s", err);
  lua_pushinteger(S, idxn);
  lua_pushinteger(S, rgban);
  return 2;
}

static int l_snes_tick(lua_State *S) {
  snes_ensure_bound(S);
  int compare = 1;
  if (lua_istable(S, 1)) {
    lua_getfield(S, 1, "compare");
    if (!lua_isnil(S, -1)) compare = lua_toboolean(S, -1);
    lua_pop(S, 1);
  }
  ab_prof_snes_tick_result r;
  const char *err = NULL;
  switch (ab_prof_snes_tick(compare, &r, &err)) {
    case AB_PROF_SNES_NOT_READY:
      lua_pushnil(S);
      return 1;                              /* hi-res / not ready */
    case AB_PROF_SNES_PLAIN:
      lua_newtable(S);
      return 1;
    case AB_PROF_SNES_M7:
      lua_newtable(S);
      lua_pushinteger(S, r.w);             lua_setfield(S, -2, "w");
      lua_pushinteger(S, r.h);             lua_setfield(S, -2, "h");
      lua_pushinteger(S, r.m7start);       lua_setfield(S, -2, "m7start");
      lua_pushinteger(S, r.m7stop);        lua_setfield(S, -2, "m7stop");
      lua_pushboolean(S, r.plane_rebuilt); lua_setfield(S, -2, "plane_rebuilt");
      return 1;
    default:
      return luaL_error(S, "%s", err ? err : "snes: tick failed");
  }
}

/* snes.draw{ x=, y=, scale= } -- FAITHFUL reconstruction from the capture,
 * no Mode 7 re-projection, no substitution. Returns { w=, h=, quads= }. */
static int l_snes_draw(lua_State *S) {
  snes_ensure_bound(S);
  double ox = 0, oy = 0, scale = 1;
  if (lua_istable(S, 1)) {
    lua_getfield(S, 1, "x"); ox = luaL_optnumber(S, -1, 0); lua_pop(S, 1);
    lua_getfield(S, 1, "y"); oy = luaL_optnumber(S, -1, 0); lua_pop(S, 1);
    lua_getfield(S, 1, "scale"); scale = luaL_optnumber(S, -1, 1); lua_pop(S, 1);
  }
  ab_prof_snes_draw_result r;
  const char *err = NULL;
  if (!ab_prof_snes_draw(ox, oy, scale, &r, &err)) {
    if (err) return luaL_error(S, "%s", err);
    lua_pushnil(S);
    return 1;
  }
  lua_newtable(S);
  lua_pushinteger(S, r.w);     lua_setfield(S, -2, "w");
  lua_pushinteger(S, r.h);     lua_setfield(S, -2, "h");
  lua_pushinteger(S, r.quads); lua_setfield(S, -2, "quads");
  return 1;
}

/* snes.frame_size() -> width, lines (or nil) -- geometry without drawing */
static int l_snes_frame_size(lua_State *S) {
  snes_ensure_bound(S);
  int w = 0, h = 0;
  if (!ab_prof_snes_frame_size(&w, &h)) { lua_pushnil(S); return 1; }
  lua_pushinteger(S, w);
  lua_pushinteger(S, h);
  return 2;
}

static const luaL_Reg SNES_FNS[] = {
  { "bind", l_snes_bind },
  { "draw", l_snes_draw },
  { "frame_size", l_snes_frame_size },
  { "set_hd_tiles", l_snes_set_hd_tiles },
  { "tick", l_snes_tick },
  { NULL, NULL },
};

/* -------------------------------------------------------------- wiring -- */

void ab_profiles_lua_open(lua_State *S) {
  luaL_newlib(S, NES_FUNCS);
  lua_pushinteger(S, AB_NES_W);            lua_setfield(S, -2, "WIDTH");
  lua_pushinteger(S, AB_NES_H);            lua_setfield(S, -2, "HEIGHT");
  lua_pushinteger(S, AB_NES_OVERSCAN_TOP); lua_setfield(S, -2, "OVERSCAN_TOP");
  lua_setglobal(S, "nes");

  luaL_newlib(S, GB_FUNCS);
  lua_pushinteger(S, AB_GB_W); lua_setfield(S, -2, "WIDTH");
  lua_pushinteger(S, AB_GB_H); lua_setfield(S, -2, "HEIGHT");
  lua_setglobal(S, "gb");

  luaL_newlib(S, MD_FUNCS);
  lua_pushinteger(S, AB_MD_MAX_W); lua_setfield(S, -2, "MAX_WIDTH");
  lua_pushinteger(S, AB_MD_MAX_H); lua_setfield(S, -2, "MAX_HEIGHT");
  lua_setglobal(S, "md");

  luaL_newlib(S, SNES_FNS);
  lua_setglobal(S, "snes");

  luaL_newlib(S, MSX_FUNCS);
  lua_pushinteger(S, AB_MSX_W);       lua_setfield(S, -2, "WIDTH");
  /* SCREEN 6/7 frames are MAX_WIDTH wide; msx.draw reports the actual width
   * of the frame it just drew so a bezel does not have to guess. */
  lua_pushinteger(S, AB_MSX_MAXW);    lua_setfield(S, -2, "MAX_WIDTH");
  lua_pushinteger(S, AB_MSX_H);       lua_setfield(S, -2, "HEIGHT");
  lua_pushinteger(S, AB_MSX_BORDER);  lua_setfield(S, -2, "BORDER");
  lua_pushinteger(S, AB_MSX_DISPLAY); lua_setfield(S, -2, "DISPLAY_WIDTH");
  lua_setglobal(S, "msx");

  luaL_newlib(S, PCE_FUNCS);
  /* WIDTH is the common case, not a constant: the VDC's display width is
   * programmable and pce.geometry().width is the live value. */
  lua_pushinteger(S, 256); lua_setfield(S, -2, "WIDTH");
  lua_pushinteger(S, 224); lua_setfield(S, -2, "HEIGHT");
  lua_setglobal(S, "pce");
}

/* Free everything the profiles own. Call from the runtime's shutdown. */
void ab_profiles_lua_shutdown(void) {
  ab_prof_nes_shutdown();
  ab_prof_gb_shutdown();
  ab_prof_md_shutdown();
  ab_prof_msx_shutdown();
  ab_prof_pce_shutdown();
  ab_prof_snes_shutdown();
}
