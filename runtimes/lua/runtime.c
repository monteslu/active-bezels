/*
 * runtime.c -- the prebuilt Lua Active Bezel runtime.
 *
 * This wasm IS the package entry point. A Lua bezel ships this file's build
 * as `main.wasm` plus its own `main.lua` in the archive; iterating on the
 * bezel is edit main.lua + repack, with no compiler in the loop.
 *
 * Script contract (all globals, all optional except tick):
 *   function init()              -- once, after the script loads
 *   function pre_render(frame)   -- BEFORE the core runs `frame`; write
 *                                   regions / ab.input_override to shape it
 *   function tick(frame)         -- once per emulated frame; draw the scene
 *   function event(kind)         -- host lifecycle events (AB_EVENT numbers)
 *
 * The whole ab_* import surface is exposed as the global `ab` table. Errors
 * never kill the session: they are logged, drawn on screen, and the script
 * is re-read on the next ASSETS_RELOADED event so a fix is one repack away.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "../../sdk/active_bezel.h"

#include "../common/ab_batteries.h"

/* --- link stubs ----------------------------------------------------------
 * lbaselib's dofile/loadfile still reference luaL_loadfilex even though the
 * definition is compiled out (AB_LUA_NOFILES): without this stub the symbol
 * becomes an `env` import the host refuses. There is no filesystem, so it
 * fails the Lua way instead.
 * emscripten_notify_memory_growth is emitted by ALLOW_MEMORY_GROWTH in
 * standalone mode; nobody outside needs the notification. */
int luaL_loadfilex(lua_State *S, const char *filename, const char *mode) {
  (void)filename; (void)mode;
  lua_pushliteral(S, "no filesystem in an active bezel");
  return LUA_ERRFILE;
}

/* ------------------------------------------------------------------ log -- */

void ab_runtime_write(const char *data, size_t length) {
  ab_log_raw(data, (int32_t)(length ? length : strlen(data)));
}
void ab_runtime_writeline(void) { /* per-call log lines already break */ }

/* ---------------------------------------------------------------- state -- */

static lua_State *L = NULL;
static char g_error[512];
static int g_has_tick = 0, g_has_event = 0, g_has_pre_render = 0;

/* The platform redraw profiles (`nes`/`gb`/`md`/`snes`/`msx`/`pce`).
 * All logic lives in runtimes/common/ab_profiles.c, shared with the other
 * three runtimes; ab_profiles_lua.c is marshaling only. */
void ab_profiles_lua_open(lua_State *S);
void ab_profiles_lua_shutdown(void);

static void set_error(const char *message) {
  size_t n = strlen(message);
  if (n >= sizeof(g_error)) n = sizeof(g_error) - 1;
  memcpy(g_error, message, n);
  g_error[n] = 0;
  ab_log_raw(g_error, (int32_t)n);
}

/* ------------------------------------------------------- Lua ab bindings -- */

static uint32_t arg_rgba(lua_State *S, int idx) {
  return (uint32_t)(lua_Unsigned)luaL_checknumber(S, idx);
}

static int l_clear(lua_State *S) { ab_clear(arg_rgba(S, 1)); return 0; }

static int l_draw_game(lua_State *S) {
  ab_draw_game(luaL_checknumber(S, 1), luaL_checknumber(S, 2),
               luaL_checknumber(S, 3), luaL_checknumber(S, 4),
               (int32_t)luaL_optinteger(S, 5, 0));
  return 0;
}

static int l_draw_game_fit(lua_State *S) {
  ab_draw_game_fit((int32_t)luaL_optinteger(S, 1, 0),
                   luaL_optnumber(S, 2, 0.5), luaL_optnumber(S, 3, 0.5),
                   (int32_t)luaL_optinteger(S, 4, 0));
  return 0;
}

static int l_fill_rect(lua_State *S) {
  ab_fill_rect(luaL_checknumber(S, 1), luaL_checknumber(S, 2),
               luaL_checknumber(S, 3), luaL_checknumber(S, 4), arg_rgba(S, 5));
  return 0;
}

static int l_triangle(lua_State *S) {
  ab_triangle(luaL_checknumber(S, 1), luaL_checknumber(S, 2),
              luaL_checknumber(S, 3), luaL_checknumber(S, 4),
              luaL_checknumber(S, 5), luaL_checknumber(S, 6), arg_rgba(S, 7));
  return 0;
}

static int l_text(lua_State *S) {
  size_t n = 0;
  const char *s = luaL_checklstring(S, 1, &n);
  ab_text_raw(s, (int32_t)n, luaL_checknumber(S, 2), luaL_checknumber(S, 3),
              luaL_checknumber(S, 4), arg_rgba(S, 5));
  return 0;
}

static int l_scissor(lua_State *S) {
  ab_scissor(luaL_checknumber(S, 1), luaL_checknumber(S, 2),
             luaL_checknumber(S, 3), luaL_checknumber(S, 4));
  return 0;
}

static int l_scissor_reset(lua_State *S) { (void)S; ab_scissor_reset(); return 0; }
static int l_push_transform(lua_State *S) { lua_pushinteger(S, ab_push_transform()); return 1; }
static int l_pop_transform(lua_State *S) { lua_pushinteger(S, ab_pop_transform()); return 1; }
static int l_reset_transform(lua_State *S) { (void)S; ab_reset_transform(); return 0; }
static int l_translate(lua_State *S) { ab_translate(luaL_checknumber(S, 1), luaL_checknumber(S, 2)); return 0; }

static int l_scale(lua_State *S) {
  double x = luaL_checknumber(S, 1);
  ab_scale(x, luaL_optnumber(S, 2, x));
  return 0;
}

static int l_rotate(lua_State *S) { ab_rotate(luaL_checknumber(S, 1)); return 0; }

/* skew(x[, y]) -- shear as tangents; skew(math.pi/6) leans 30 degrees. */
static int l_skew(lua_State *S) {
  ab_skew(luaL_checknumber(S, 1), luaL_optnumber(S, 2, 0));
  return 0;
}

static int l_transform2d(lua_State *S) {
  ab_transform2d(luaL_checknumber(S, 1), luaL_checknumber(S, 2), luaL_checknumber(S, 3),
                 luaL_checknumber(S, 4), luaL_checknumber(S, 5), luaL_checknumber(S, 6));
  return 0;
}

/* --- offscreen surfaces --------------------------------------------------
 * surface_create(w, h) -> handle
 * surface_target(handle) / surface_end()  -- draw into it
 * surface_filter(source, destination, glsl) -- run a shader between them
 * surface_preset(source, destination, "x.glslp") -- run a MULTI-PASS preset
 * The handle works anywhere a texture does. ab.GAME as a source means the
 * live frame. */
static int l_surface_create(lua_State *S) {
  lua_pushinteger(S, ab_surface_create((int32_t)luaL_checkinteger(S, 1),
                                       (int32_t)luaL_checkinteger(S, 2)));
  return 1;
}
static int l_surface_target(lua_State *S) {
  lua_pushinteger(S, ab_surface_target((int32_t)luaL_checkinteger(S, 1)));
  return 1;
}
static int l_surface_end(lua_State *S) { lua_pushinteger(S, ab_surface_end()); return 1; }
static int l_surface_filter(lua_State *S) {
  size_t n = 0;
  const char *shader = luaL_checklstring(S, 3, &n);
  lua_pushinteger(S, ab_surface_filter_raw((int32_t)luaL_checkinteger(S, 1),
    (int32_t)luaL_checkinteger(S, 2), shader, (int32_t)n));
  return 1;
}

static int l_surface_preset(lua_State *S) {
  size_t n = 0;
  const char *preset = luaL_checklstring(S, 3, &n);
  lua_pushinteger(S, ab_surface_preset_raw((int32_t)luaL_checkinteger(S, 1),
    (int32_t)luaL_checkinteger(S, 2), preset, (int32_t)n));
  return 1;
}

/* quad({{x=,y=}, ...4}, texture, rgba) -- perspective-correct textured quad,
 * corners clockwise from top-left. This is how a tilted surface reads as a
 * plane instead of a PS1 warp. */
static int l_quad(lua_State *S) {
  luaL_checktype(S, 1, LUA_TTABLE);
  ab_point pts[4];
  for (int i = 0; i < 4; i++) {
    lua_rawgeti(S, 1, i + 1);
    if (!lua_istable(S, -1)) return luaL_error(S, "quad: corner %d is not a table", i + 1);
    lua_getfield(S, -1, "x"); pts[i].x = lua_tonumber(S, -1); lua_pop(S, 1);
    lua_getfield(S, -1, "y"); pts[i].y = lua_tonumber(S, -1); lua_pop(S, 1);
    lua_pop(S, 1);
  }
  lua_pushinteger(S, ab_quad(pts, (int32_t)luaL_optinteger(S, 2, 0),
    (uint32_t)(lua_Unsigned)luaL_optnumber(S, 3, 4294967295.0)));
  return 1;
}

/* mesh{ {x=,y=,rgba=,u=,v=}, ... }[, texture] -> emitted triangle count */
static int l_mesh(lua_State *S) {
  luaL_checktype(S, 1, LUA_TTABLE);
  int32_t count = (int32_t)lua_rawlen(S, 1);
  int32_t texture = (int32_t)luaL_optinteger(S, 2, 0);
  if (count <= 0) { lua_pushinteger(S, 0); return 1; }
  ab_vertex *v = (ab_vertex *)malloc(sizeof(ab_vertex) * (size_t)count);
  if (!v) return luaL_error(S, "mesh: out of memory");
  for (int32_t i = 0; i < count; i++) {
    lua_rawgeti(S, 1, i + 1);
    if (!lua_istable(S, -1)) { free(v); return luaL_error(S, "mesh: vertex %d is not a table", i + 1); }
    lua_getfield(S, -1, "x");    v[i].x = lua_tonumber(S, -1); lua_pop(S, 1);
    lua_getfield(S, -1, "y");    v[i].y = lua_tonumber(S, -1); lua_pop(S, 1);
    lua_getfield(S, -1, "rgba"); v[i].rgba = (uint32_t)(lua_Unsigned)lua_tonumber(S, -1); lua_pop(S, 1);
    lua_getfield(S, -1, "u");    v[i].u = lua_tonumber(S, -1); lua_pop(S, 1);
    lua_getfield(S, -1, "v");    v[i].v = lua_tonumber(S, -1); lua_pop(S, 1);
    lua_pop(S, 1);
  }
  int32_t emitted = ab_mesh(v, count, texture);
  free(v);
  lua_pushinteger(S, emitted);
  return 1;
}

/* texture_filter(handle, mode): 0 nearest, 1 linear, 2 bicubic,
 * 3 palette-indexed bicubic; +16 = REPEAT wrap (GPU only) */
static int l_texture_filter(lua_State *S) {
  lua_pushinteger(S, ab_texture_filter((int32_t)luaL_checkinteger(S, 1),
                                       (int32_t)luaL_checkinteger(S, 2)));
  return 1;
}

/* texture_palette(handle, paletteHandle): bind a 256x1 RGBA palette texture
 * to an index texture (RED channel = i/255) for filter mode 3. */
static int l_texture_palette(lua_State *S) {
  lua_pushinteger(S, ab_texture_palette((int32_t)luaL_checkinteger(S, 1),
                                        (int32_t)luaL_checkinteger(S, 2)));
  return 1;
}

static int l_texture_create(lua_State *S) {
  size_t n = 0;
  const char *pixels = luaL_checklstring(S, 1, &n);
  int32_t w = (int32_t)luaL_checkinteger(S, 2);
  int32_t h = (int32_t)luaL_checkinteger(S, 3);
  if (w <= 0 || h <= 0 || n < (size_t)w * (size_t)h * 4)
    return luaL_error(S, "texture_create: need %d bytes of RGBA, got %d", w * h * 4, (int)n);
  lua_pushinteger(S, ab_texture_create_rgba(pixels, w, h));
  return 1;
}

static int l_texture_destroy(lua_State *S) {
  lua_pushinteger(S, ab_texture_destroy((int32_t)luaL_checkinteger(S, 1)));
  return 1;
}

static int l_draw_texture(lua_State *S) {
  lua_pushinteger(S, ab_draw_texture((int32_t)luaL_checkinteger(S, 1),
    luaL_checknumber(S, 2), luaL_checknumber(S, 3),
    luaL_checknumber(S, 4), luaL_checknumber(S, 5)));
  return 1;
}

static int l_draw_texture_rect(lua_State *S) {
  lua_pushinteger(S, ab_draw_texture_rect((int32_t)luaL_checkinteger(S, 1),
    luaL_checknumber(S, 2), luaL_checknumber(S, 3),
    luaL_checknumber(S, 4), luaL_checknumber(S, 5),
    (int32_t)luaL_checkinteger(S, 6), (int32_t)luaL_checkinteger(S, 7),
    (int32_t)luaL_checkinteger(S, 8), (int32_t)luaL_checkinteger(S, 9)));
  return 1;
}

static int l_effect_set(lua_State *S) {
  size_t n = 0;
  const char *src = luaL_checklstring(S, 1, &n);
  lua_pushboolean(S, ab_effect_set_raw(src, (int32_t)n) != 0);
  return 1;
}

static int l_effect_clear(lua_State *S) { lua_pushinteger(S, ab_effect_clear()); return 1; }

static int l_game_width(lua_State *S) { lua_pushinteger(S, ab_game_width()); return 1; }
static int l_game_height(lua_State *S) { lua_pushinteger(S, ab_game_height()); return 1; }

static int l_game_pixel(lua_State *S) {
  lua_pushinteger(S, (lua_Integer)ab_game_pixel(
    (int32_t)luaL_checkinteger(S, 1), (int32_t)luaL_checkinteger(S, 2)));
  return 1;
}

static int l_logical_width(lua_State *S) { lua_pushinteger(S, ab_logical_width()); return 1; }
static int l_logical_height(lua_State *S) { lua_pushinteger(S, ab_logical_height()); return 1; }
static int l_physical_width(lua_State *S) { lua_pushinteger(S, ab_physical_width()); return 1; }
static int l_physical_height(lua_State *S) { lua_pushinteger(S, ab_physical_height()); return 1; }
static int l_elapsed_ms(lua_State *S) { lua_pushnumber(S, ab_elapsed_ms()); return 1; }
static int l_delta_ms(lua_State *S) { lua_pushnumber(S, ab_delta_ms()); return 1; }

static int l_input(lua_State *S) {
  lua_pushinteger(S, ab_input_state(
    (int32_t)luaL_optinteger(S, 1, 0), (int32_t)luaL_optinteger(S, 2, 1),
    (int32_t)luaL_optinteger(S, 3, 0), (int32_t)luaL_checkinteger(S, 4)));
  return 1;
}

/* input_override(port, device, index, id, value) -> accepted?
 * Only honored inside pre_render; the host refuses (and logs once) anywhere
 * else. Same argument shape as ab.input, plus the value the CORE should see. */
static int l_input_override(lua_State *S) {
  lua_pushboolean(S, ab_input_override(
    (int32_t)luaL_optinteger(S, 1, 0), (int32_t)luaL_optinteger(S, 2, 1),
    (int32_t)luaL_optinteger(S, 3, 0), (int32_t)luaL_checkinteger(S, 4),
    (int32_t)luaL_checkinteger(S, 5)) != 0);
  return 1;
}

static int l_log(lua_State *S) {
  size_t n = 0;
  const char *s = luaL_checklstring(S, 1, &n);
  ab_log_raw(s, (int32_t)n);
  return 0;
}

static int l_region(lua_State *S) {
  size_t n = 0;
  const char *name = luaL_checklstring(S, 1, &n);
  int32_t id = ab_region_find_raw(name, (int32_t)n);
  if (id < 0) lua_pushnil(S); else lua_pushinteger(S, id);
  return 1;
}

static int l_region_find_id(lua_State *S) {
  int32_t id = ab_region_find_id((int32_t)luaL_checkinteger(S, 1));
  if (id < 0) lua_pushnil(S); else lua_pushinteger(S, id);
  return 1;
}

static int l_region_size(lua_State *S) { lua_pushinteger(S, ab_region_size((int32_t)luaL_checkinteger(S, 1))); return 1; }
static int l_region_flags(lua_State *S) { lua_pushinteger(S, ab_region_flags((int32_t)luaL_checkinteger(S, 1))); return 1; }
static int l_region_offset(lua_State *S) { lua_pushinteger(S, ab_region_offset((int32_t)luaL_checkinteger(S, 1))); return 1; }
static int l_region_generation(lua_State *S) { lua_pushinteger(S, ab_region_generation()); return 1; }
static int l_region_count(lua_State *S) { lua_pushinteger(S, ab_region_count()); return 1; }

static int l_read_u8(lua_State *S) {
  lua_pushinteger(S, ab_region_read_u8(
    (int32_t)luaL_checkinteger(S, 1), (int32_t)luaL_checkinteger(S, 2)));
  return 1;
}

static int l_write_u8(lua_State *S) {
  lua_pushinteger(S, ab_region_write_u8(
    (int32_t)luaL_checkinteger(S, 1), (int32_t)luaL_checkinteger(S, 2),
    (int32_t)luaL_checkinteger(S, 3)));
  return 1;
}

/* read(id, offset, length) -> binary string; the per-byte import is fine for
 * a few reads, a string is what table-driven decoders want. */
static int l_read(lua_State *S) {
  int32_t id = (int32_t)luaL_checkinteger(S, 1);
  int32_t off = (int32_t)luaL_checkinteger(S, 2);
  int32_t len = (int32_t)luaL_checkinteger(S, 3);
  if (len <= 0 || len > 1 << 20) return luaL_error(S, "read: bad length");
  luaL_Buffer b;
  char *dst = luaL_buffinitsize(S, &b, (size_t)len);
  for (int32_t i = 0; i < len; i++) {
    int32_t v = ab_region_read_u8(id, off + i);
    dst[i] = (char)(v < 0 ? 0 : v);
  }
  luaL_pushresultsize(&b, (size_t)len);
  return 1;
}

static int l_asset(lua_State *S) {
  size_t n = 0;
  const char *name = luaL_checklstring(S, 1, &n);
  int32_t size = ab_asset_size_raw(name, (int32_t)n);
  if (size < 0) { lua_pushnil(S); return 1; }
  luaL_Buffer b;
  char *dst = luaL_buffinitsize(S, &b, (size_t)size);
  int32_t got = ab_asset_read_raw(name, (int32_t)n, dst, size);
  luaL_pushresultsize(&b, (size_t)(got < 0 ? 0 : got));
  return 1;
}

static int l_config_bool(lua_State *S) {
  size_t n = 0;
  const char *key = luaL_checklstring(S, 1, &n);
  lua_pushboolean(S, ab_config_bool_raw(key, (int32_t)n) != 0);
  return 1;
}

static int l_config_number(lua_State *S) {
  size_t n = 0;
  const char *key = luaL_checklstring(S, 1, &n);
  lua_pushnumber(S, ab_config_number_raw(key, (int32_t)n));
  return 1;
}

static int l_config_string(lua_State *S) {
  size_t n = 0;
  const char *key = luaL_checklstring(S, 1, &n);
  int32_t len = ab_config_string_length_raw(key, (int32_t)n);
  if (len < 0) { lua_pushnil(S); return 1; }
  luaL_Buffer b;
  char *dst = luaL_buffinitsize(S, &b, (size_t)len);
  int32_t got = ab_config_string_read_raw(key, (int32_t)n, dst, len);
  luaL_pushresultsize(&b, (size_t)(got < 0 ? 0 : got));
  return 1;
}

/* --- images --------------------------------------------------------------
 * ab.image('assets/logo.png') -> { texture=, width=, height= }
 * ab.image_data(bytes) does the same for a raw string. Both go through the
 * shared batteries so every runtime decodes identically.
 */
static int push_image(lua_State *S, int ok, int32_t tex, int w, int h, const char *err) {
  if (!ok) return luaL_error(S, "image: %s", err ? err : "decode failed");
  lua_createtable(S, 0, 3);
  lua_pushinteger(S, tex); lua_setfield(S, -2, "texture");
  lua_pushinteger(S, w); lua_setfield(S, -2, "width");
  lua_pushinteger(S, h); lua_setfield(S, -2, "height");
  return 1;
}

static int l_image_data(lua_State *S) {
  size_t n = 0;
  const char *bytes = luaL_checklstring(S, 1, &n);
  int32_t tex = 0; int w = 0, h = 0; const char *err = NULL;
  int ok = ab_bat_image_from_memory((const unsigned char *)bytes, (int)n, &tex, &w, &h, &err);
  return push_image(S, ok, tex, w, h, err);
}

static int l_image(lua_State *S) {
  size_t n = 0;
  const char *name = luaL_checklstring(S, 1, &n);
  int32_t tex = 0; int w = 0, h = 0; const char *err = NULL;
  int ok = ab_bat_image_from_asset(name, (int)n, &tex, &w, &h, &err);
  return push_image(S, ok, tex, w, h, err);
}

/* --- TrueType text -------------------------------------------------------
 * ab.font('assets/font.ttf') -> handle
 * ab.print(font, text, x, y, px, rgba) -> pen X after the last glyph
 * ab.measure(font, text, px) -> width
 * ab.font_metrics(font, px) -> ascent, descent, line_height
 * All shared with the other runtimes (see runtimes/common/ab_batteries.c).
 */
static int l_font(lua_State *S) {
  size_t n = 0;
  const char *name = luaL_checklstring(S, 1, &n);
  const char *err = NULL;
  int32_t handle = ab_bat_font_load(name, (int)n, &err);
  if (handle <= 0) return luaL_error(S, "font: %s", err ? err : "load failed");
  lua_pushinteger(S, handle);
  return 1;
}

static int l_print(lua_State *S) {
  size_t n = 0;
  int32_t font = (int32_t)luaL_checkinteger(S, 1);
  const char *text = luaL_checklstring(S, 2, &n);
  const char *err = NULL;
  double advance = ab_bat_font_print(font, text, (int)n,
    luaL_checknumber(S, 3), luaL_checknumber(S, 4),
    (int32_t)luaL_checkinteger(S, 5),
    (uint32_t)(lua_Unsigned)luaL_checknumber(S, 6), &err);
  if (err) return luaL_error(S, "print: %s", err);
  lua_pushnumber(S, advance);
  return 1;
}

static int l_measure(lua_State *S) {
  size_t n = 0;
  int32_t font = (int32_t)luaL_checkinteger(S, 1);
  const char *text = luaL_checklstring(S, 2, &n);
  const char *err = NULL;
  double w = ab_bat_font_measure(font, text, (int)n, (int32_t)luaL_checkinteger(S, 3), &err);
  if (err) return luaL_error(S, "measure: %s", err);
  lua_pushnumber(S, w);
  return 1;
}

static int l_font_metrics(lua_State *S) {
  int32_t font = (int32_t)luaL_checkinteger(S, 1);
  double a = 0, d = 0, lh = 0; const char *err = NULL;
  if (!ab_bat_font_metrics(font, (int32_t)luaL_checkinteger(S, 2), &a, &d, &lh, &err))
    return luaL_error(S, "font_metrics: %s", err ? err : "failed");
  lua_pushnumber(S, a); lua_pushnumber(S, d); lua_pushnumber(S, lh);
  return 3;
}

/* --- multi-byte region reads (shared) ------------------------------------ */
static int read_uint(lua_State *S, int bytes) {
  lua_pushinteger(S, (lua_Integer)ab_bat_read_uint(
    (int32_t)luaL_checkinteger(S, 1), (int32_t)luaL_checkinteger(S, 2),
    bytes, lua_toboolean(S, 3)));
  return 1;
}
static int l_read_u16(lua_State *S) { return read_uint(S, 2); }
static int l_read_u24(lua_State *S) { return read_uint(S, 3); }
static int l_read_u32(lua_State *S) { return read_uint(S, 4); }

/* loadasset('assets/data.lua') -> chunk function (or nil, error). Data and
 * modules without a filesystem: the chunk is compiled, not run. */
static int l_loadasset(lua_State *S) {
  size_t n = 0;
  const char *name = luaL_checklstring(S, 1, &n);
  int32_t size = ab_asset_size_raw(name, (int32_t)n);
  if (size < 0) { lua_pushnil(S); lua_pushfstring(S, "no such asset '%s'", name); return 2; }
  char *bytes = (char *)malloc((size_t)size + 1);
  if (!bytes) { lua_pushnil(S); lua_pushliteral(S, "out of memory"); return 2; }
  int32_t got = ab_asset_read_raw(name, (int32_t)n, bytes, size);
  int status = luaL_loadbufferx(S, bytes, (size_t)(got < 0 ? 0 : got), name, "t");
  free(bytes);
  if (status != LUA_OK) { lua_pushnil(S); lua_insert(S, -2); return 2; }
  return 1;
}

/* rgb(r, g, b[, a]) -> packed 0xRRGGBBAA, the format every command takes */
static int l_rgb(lua_State *S) {
  lua_pushinteger(S, (lua_Integer)ab_bat_rgba(
    (int)luaL_checkinteger(S, 1), (int)luaL_checkinteger(S, 2),
    (int)luaL_checkinteger(S, 3), (int)luaL_optinteger(S, 4, 255)));
  return 1;
}

static const luaL_Reg AB_FUNCS[] = {
  { "clear", l_clear },
  { "draw_game", l_draw_game },
  { "draw_game_fit", l_draw_game_fit },
  { "fill_rect", l_fill_rect },
  { "triangle", l_triangle },
  { "text", l_text },
  { "scissor", l_scissor },
  { "scissor_reset", l_scissor_reset },
  { "push_transform", l_push_transform },
  { "pop_transform", l_pop_transform },
  { "reset_transform", l_reset_transform },
  { "translate", l_translate },
  { "scale", l_scale },
  { "rotate", l_rotate },
  { "skew", l_skew },
  { "transform2d", l_transform2d },
  { "quad", l_quad },
  { "surface_create", l_surface_create },
  { "surface_target", l_surface_target },
  { "surface_end", l_surface_end },
  { "surface_filter", l_surface_filter },
  { "surface_preset", l_surface_preset },
  { "mesh", l_mesh },
  { "texture_filter", l_texture_filter },
  { "texture_palette", l_texture_palette },
  { "texture_create", l_texture_create },
  { "texture_destroy", l_texture_destroy },
  { "draw_texture", l_draw_texture },
  { "draw_texture_rect", l_draw_texture_rect },
  { "effect_set", l_effect_set },
  { "effect_clear", l_effect_clear },
  { "game_width", l_game_width },
  { "game_height", l_game_height },
  { "game_pixel", l_game_pixel },
  { "logical_width", l_logical_width },
  { "logical_height", l_logical_height },
  { "physical_width", l_physical_width },
  { "physical_height", l_physical_height },
  { "elapsed_ms", l_elapsed_ms },
  { "delta_ms", l_delta_ms },
  { "input", l_input },
  { "input_override", l_input_override },
  { "log", l_log },
  { "region", l_region },
  { "region_find_id", l_region_find_id },
  { "region_size", l_region_size },
  { "region_flags", l_region_flags },
  { "region_offset", l_region_offset },
  { "region_generation", l_region_generation },
  { "region_count", l_region_count },
  { "read_u8", l_read_u8 },
  { "write_u8", l_write_u8 },
  { "read", l_read },
  { "asset", l_asset },
  { "image", l_image },
  { "image_data", l_image_data },
  { "font", l_font },
  /* draw_text, not print: the same spelling in all four runtimes. Python
   * and Ruby cannot safely bind `print` (shadows a builtin / silently
   * reaches Kernel#print), so the portable name is the only name. */
  { "draw_text", l_print },
  { "measure", l_measure },
  { "font_metrics", l_font_metrics },
  { "read_u16", l_read_u16 },
  { "read_u24", l_read_u24 },
  { "read_u32", l_read_u32 },
  { "loadasset", l_loadasset },
  { "config_bool", l_config_bool },
  { "config_number", l_config_number },
  { "config_string", l_config_string },
  { "rgb", l_rgb },
  { NULL, NULL },
};

/* -------------------------------------------------------- script loading -- */

static int load_script(void) {
  static const char *CANDIDATES[] = { "main.lua", "assets/main.lua" };
  const char *name = NULL;
  int32_t size = -1;
  for (unsigned i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++) {
    size = ab_asset_size(CANDIDATES[i]);
    if (size >= 0) { name = CANDIDATES[i]; break; }
  }
  if (!name) { set_error("lua runtime: no main.lua (or assets/main.lua) in the package"); return 0; }

  char *source = (char *)malloc((size_t)size + 1);
  if (!source) { set_error("lua runtime: out of memory reading main.lua"); return 0; }
  int32_t got = ab_asset_read(name, source, size);
  if (got < 0) { free(source); set_error("lua runtime: asset_read failed for main.lua"); return 0; }

  if (luaL_loadbufferx(L, source, (size_t)got, name, "t") != LUA_OK ||
      lua_pcall(L, 0, 0, 0) != LUA_OK) {
    set_error(lua_tostring(L, -1) ? lua_tostring(L, -1) : "lua runtime: unknown load error");
    lua_pop(L, 1);
    free(source);
    return 0;
  }
  free(source);

  lua_getglobal(L, "tick");
  g_has_tick = lua_isfunction(L, -1);
  lua_pop(L, 1);
  lua_getglobal(L, "event");
  g_has_event = lua_isfunction(L, -1);
  lua_pop(L, 1);
  lua_getglobal(L, "pre_render");
  g_has_pre_render = lua_isfunction(L, -1);
  lua_pop(L, 1);
  if (!g_has_tick) { set_error("lua runtime: main.lua must define a global function tick(frame)"); return 0; }

  lua_getglobal(L, "init");
  if (lua_isfunction(L, -1)) {
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
      set_error(lua_tostring(L, -1) ? lua_tostring(L, -1) : "lua runtime: init() failed");
      lua_pop(L, 1);
      return 0;
    }
  } else {
    lua_pop(L, 1);
  }
  g_error[0] = 0;
  return 1;
}

static void boot(void) {
  if (L) { lua_close(L); L = NULL; }
  g_has_tick = g_has_event = g_has_pre_render = 0;
  L = luaL_newstate();
  if (!L) { set_error("lua runtime: luaL_newstate failed"); return; }
  /* base, string, table, math, utf8, coroutine. No io/os/package: there is
   * no filesystem and no dynamic loading in a bezel, by design. */
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1); lua_pop(L, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L, 1);
  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L, 1);
  luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(L, 1);
  luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1); lua_pop(L, 1);

  /* Platform redraw profiles: GPU-native renderers with declarative
   * substitution. Registers the `nes`/`gb`/`md`/`snes`/`msx`/`pce` globals. */
  ab_profiles_lua_open(L);

  luaL_newlib(L, AB_FUNCS);
  /* Constant tables so scripts never hard-code ABI numbers. Mirrors the C
   * SDK's AB_* defines; button ids are the libretro joypad numbering the
   * input_state import speaks. */
  /* ab.GAME: pass as a texture handle to sample the LIVE GAME FRAME, e.g.
   * ab.quad(corners, ab.GAME) maps the running game onto a tilted plane. */
  lua_pushinteger(L, -1);
  lua_setfield(L, -2, "GAME");
  {
    static const struct { const char *name; lua_Integer value; } EVENT[] = {
      { "RESET", 1 }, { "STATE_LOADED", 2 }, { "REWIND_JUMP", 3 },
      { "CONFIG_CHANGED", 4 }, { "DISPLAY_CHANGED", 5 },
      { "ASSETS_RELOADED", 6 }, { "REGIONS_CHANGED", 7 },
    }, FIT[] = {
      { "CONTAIN", 0 }, { "COVER", 1 }, { "STRETCH", 2 }, { "INTEGER", 3 },
    }, SAMPLE[] = {
      { "NEAREST", 0 }, { "LINEAR", 1 },
    }, DEVICE[] = {
      { "JOYPAD", 1 }, { "ANALOG", 5 },
    }, BTN[] = {
      { "B", 0 }, { "Y", 1 }, { "SELECT", 2 }, { "START", 3 },
      { "UP", 4 }, { "DOWN", 5 }, { "LEFT", 6 }, { "RIGHT", 7 },
      { "A", 8 }, { "X", 9 }, { "L", 10 }, { "R", 11 },
      { "L2", 12 }, { "R2", 13 }, { "L3", 14 }, { "R3", 15 }, { "MASK", 256 },
    }, ANALOG[] = {
      /* ab.input(port, ab.DEVICE.ANALOG, index, id):
       * sticks: index LEFT/RIGHT, id X/Y -> -32768..32767
       * triggers: index BUTTON, id ab.BTN.L2 / ab.BTN.R2 -> 0..32767 */
      { "LEFT", 0 }, { "RIGHT", 1 }, { "BUTTON", 2 }, { "X", 0 }, { "Y", 1 },
    };
    static const struct { const char *name;
                          const void *rows; int count; } GROUPS[] = {
      { "EVENT", EVENT, (int)(sizeof(EVENT) / sizeof(EVENT[0])) },
      { "FIT", FIT, (int)(sizeof(FIT) / sizeof(FIT[0])) },
      { "SAMPLE", SAMPLE, (int)(sizeof(SAMPLE) / sizeof(SAMPLE[0])) },
      { "DEVICE", DEVICE, (int)(sizeof(DEVICE) / sizeof(DEVICE[0])) },
      { "BTN", BTN, (int)(sizeof(BTN) / sizeof(BTN[0])) },
      { "ANALOG", ANALOG, (int)(sizeof(ANALOG) / sizeof(ANALOG[0])) },
    };
    for (unsigned g = 0; g < sizeof(GROUPS) / sizeof(GROUPS[0]); g++) {
      const struct { const char *name; lua_Integer value; } *rows = GROUPS[g].rows;
      lua_createtable(L, 0, GROUPS[g].count);
      for (int i = 0; i < GROUPS[g].count; i++) {
        lua_pushinteger(L, rows[i].value);
        lua_setfield(L, -2, rows[i].name);
      }
      lua_setfield(L, -2, GROUPS[g].name);
    }
  }
  lua_setglobal(L, "ab");

  load_script();
}

/* ------------------------------------------------------------ entrypoints -- */

/* 2, not 1: this runtime imports input_override, so it cannot instantiate on
 * a pre-ABI-2 host anyway -- reporting 2 turns that LinkError into the
 * versioned refusal the spec promises. */
AB_EXPORT("ab_abi_version")
int32_t ab_abi_version(void) { return AB_ABI_VERSION; }

AB_EXPORT("ab_init")
int32_t ab_init(uint32_t descriptor) {
  /* The ABI passes a descriptor word (sdk/abi.json: ab_init(i32)->i32).
   * Nothing needs it yet, but the signature is the contract: a JS host
   * calling ab_init(0) against a zero-arg export only works because
   * wasm silently drops surplus arguments, and a stricter host would not. */
  (void)descriptor;
  boot();
  return 0; /* 0 = success. A script error is NOT an init failure: the error
             * state still wants ticks so it can display itself. */
}

/* The host reads this after ab_init AND after every ASSETS_RELOADED reboot,
 * and skips the per-frame ab_pre_render call entirely when the script defines
 * no hook -- watch/breakpoint bursts step thousands of frames, so "not
 * defined" has to cost a cached property check, not an interpreter call. */
AB_EXPORT("ab_pre_render_defined")
int32_t ab_pre_render_defined(void) { return g_has_pre_render; }

AB_EXPORT("ab_pre_render")
int32_t ab_pre_render(uint64_t frame) {
  /* No error panel here: pre_render runs before the frame, tick() owns the
   * screen. A broken script already shows its panel there. */
  if (g_error[0] || !L || !g_has_pre_render) return 0;
  lua_getglobal(L, "pre_render");
  lua_pushinteger(L, (lua_Integer)frame);
  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    set_error(lua_tostring(L, -1) ? lua_tostring(L, -1) : "pre_render() failed");
    lua_pop(L, 1);
  }
  return 0;
}

AB_EXPORT("ab_tick")
void ab_tick(uint64_t frame) {
  if (g_error[0] || !L || !g_has_tick) {
    ab_clear(0x201018ffu);
    ab_draw_game_fit(0, 0.5, 0.35, 0);
    ab_text("lua bezel error:", 40, 40, 30, 0xff8080ffu);
    ab_text(g_error[0] ? g_error : "script not loaded", 40, 84, 24, 0xffd0d0ffu);
    ab_text("fix main.lua and repack -- the runtime reloads it", 40, 124, 22, 0x9098b0ffu);
    return;
  }
  lua_getglobal(L, "tick");
  lua_pushinteger(L, (lua_Integer)frame);
  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    set_error(lua_tostring(L, -1) ? lua_tostring(L, -1) : "tick() failed");
    lua_pop(L, 1);
  }
}

AB_EXPORT("ab_event")
void ab_event(int32_t kind, uint32_t data) {
  (void)data;                          /* reserved by the ABI, unused today */
  /* ASSETS_RELOADED (6): the package archive changed under us -- re-read
   * main.lua. This is the whole iteration story: edit, repack, replay. */
  if (kind == 6) { boot(); return; }
  if (!L || !g_has_event || g_error[0]) return;
  lua_getglobal(L, "event");
  lua_pushinteger(L, kind);
  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    set_error(lua_tostring(L, -1) ? lua_tostring(L, -1) : "event() failed");
    lua_pop(L, 1);
  }
}

AB_EXPORT("ab_shutdown")
void ab_shutdown(void) {
  if (L) { lua_close(L); L = NULL; }
  ab_bat_shutdown();
  ab_profiles_lua_shutdown();
}
