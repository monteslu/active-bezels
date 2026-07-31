/*
 * runtime.c -- the prebuilt Lua Active Bezel runtime.
 *
 * This wasm IS the package entry point. A Lua bezel ships this file's build
 * as `main.wasm` plus its own `main.lua` in the archive; iterating on the
 * bezel is edit main.lua + repack, with no compiler in the loop.
 *
 * Script contract (all globals, all optional except tick):
 *   function init()        -- once, after the script loads
 *   function tick(frame)   -- once per emulated frame; draw the whole scene
 *   function event(kind)   -- host lifecycle events (AB_EVENT numbers)
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

/* Batteries: PNG/JPG/GIF/BMP decoding and real TrueType text. Both are
 * public-domain stb single-headers; emscripten's libc supplies everything
 * they need. Interesting bezels want images and readable type, and "ship a
 * C toolchain" must never be the price of either. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
/* assert() in emscripten writes to stderr, which drags fd_write/fd_seek/
 * fd_close WASI imports into the wasm. These are release builds of
 * public-domain decoders; a failed internal assert may as well trap. */
#define STBI_ASSERT(x) ((void)0)
#include "stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((void)0)
#include "stb_truetype.h"

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
void emscripten_notify_memory_growth(int idx) { (void)idx; }

/* libc's __wasi_clock_time_get is a forwarding stub around the actual WASI
 * import; defining it here strongly removes the import. A bezel has no wall
 * clock -- time comes from the host via time_elapsed_ms -- so this returns a
 * counter that advances a millisecond per call: monotonic, deterministic,
 * and honest about not being real time. */
unsigned short __wasi_clock_time_get(unsigned int id, unsigned long long precision,
                                     unsigned long long *out) {
  static unsigned long long fake_ns = 0;
  (void)id; (void)precision;
  fake_ns += 1000000ull;
  if (out) *out = fake_ns;
  return 0;
}

/* ------------------------------------------------------------------ log -- */

void ab_runtime_write(const char *data, size_t length) {
  ab_log_raw(data, (int32_t)(length ? length : strlen(data)));
}
void ab_runtime_writeline(void) { /* per-call log lines already break */ }

/* ---------------------------------------------------------------- state -- */

static lua_State *L = NULL;
static char g_error[512];
static int g_has_tick = 0, g_has_event = 0;

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
 * Decodes from the package's asset bundle, uploads once, returns the handle
 * plus dimensions. ab.image_data(bytes) does the same for a raw string.
 */
static int push_image_from_memory(lua_State *S, const unsigned char *bytes, int len) {
  int w = 0, h = 0, comp = 0;
  unsigned char *pixels = stbi_load_from_memory(bytes, len, &w, &h, &comp, 4);
  if (!pixels) return luaL_error(S, "image: decode failed: %s", stbi_failure_reason());
  int32_t texture = ab_texture_create_rgba(pixels, w, h);
  stbi_image_free(pixels);
  if (texture <= 0) return luaL_error(S, "image: texture_create failed");
  lua_createtable(S, 0, 3);
  lua_pushinteger(S, texture); lua_setfield(S, -2, "texture");
  lua_pushinteger(S, w); lua_setfield(S, -2, "width");
  lua_pushinteger(S, h); lua_setfield(S, -2, "height");
  return 1;
}

static int l_image_data(lua_State *S) {
  size_t n = 0;
  const char *bytes = luaL_checklstring(S, 1, &n);
  return push_image_from_memory(S, (const unsigned char *)bytes, (int)n);
}

static int l_image(lua_State *S) {
  size_t n = 0;
  const char *name = luaL_checklstring(S, 1, &n);
  int32_t size = ab_asset_size_raw(name, (int32_t)n);
  if (size <= 0) return luaL_error(S, "image: no such asset '%s'", name);
  unsigned char *bytes = (unsigned char *)malloc((size_t)size);
  if (!bytes) return luaL_error(S, "image: out of memory");
  int32_t got = ab_asset_read_raw(name, (int32_t)n, bytes, size);
  if (got <= 0) { free(bytes); return luaL_error(S, "image: asset_read failed"); }
  int result = push_image_from_memory(S, bytes, (int)got);
  free(bytes);
  return result;
}

/* --- TrueType text -------------------------------------------------------
 * ab.font('assets/font.ttf') -> font handle
 * ab.print(font, text, x, y, px, rgba) -> x advance (draws)
 * ab.measure(font, text, px) -> width in logical pixels
 *
 * Each (font, integer px) gets an ASCII 32..126 atlas baked once into a
 * single WHITE texture; print emits one textured mesh whose vertex colour
 * carries the tint, so any colour costs nothing and a frame of text is a
 * couple of draw calls, not hundreds.
 */
#define FONT_MAX 8
#define FONT_SIZE_CACHE 16
#define ATLAS_PX 1024
typedef struct {
  int32_t px;
  int32_t texture;
  stbtt_bakedchar baked[96];
} FontAtlas;
typedef struct {
  unsigned char *bytes;
  stbtt_fontinfo info;
  FontAtlas sizes[FONT_SIZE_CACHE];
  int32_t used;
} Font;
static Font g_fonts[FONT_MAX];
static int32_t g_font_count = 0;

static FontAtlas *font_atlas(Font *font, int32_t px) {
  if (px < 6) px = 6;
  if (px > 256) px = 256;
  for (int32_t i = 0; i < font->used; i++)
    if (font->sizes[i].px == px) return &font->sizes[i];
  FontAtlas *slot;
  if (font->used < FONT_SIZE_CACHE) {
    slot = &font->sizes[font->used++];
  } else {
    /* Recycle the oldest slot -- but do NOT destroy its texture here: glyph
     * meshes emitted EARLIER THIS FRAME may still reference it, and the
     * compositor resolves handles at compose time. Destroying mid-frame
     * turned every already-drawn string into solid boxes (dead handle ==
     * color-only quads). The stale texture leaks until shutdown; a bezel
     * cycling >16 live sizes per font is the pathological case and pays
     * with memory, not with corrupted text. */
    slot = &font->sizes[0];
  }
  unsigned char *alpha = (unsigned char *)malloc(ATLAS_PX * ATLAS_PX);
  unsigned char *rgba = (unsigned char *)malloc(ATLAS_PX * ATLAS_PX * 4);
  if (!alpha || !rgba) { free(alpha); free(rgba); return NULL; }
  stbtt_BakeFontBitmap(font->bytes, 0, (float)px, alpha, ATLAS_PX, ATLAS_PX, 32, 96, slot->baked);
  for (int32_t i = 0; i < ATLAS_PX * ATLAS_PX; i++) {
    rgba[i * 4 + 0] = 255; rgba[i * 4 + 1] = 255;
    rgba[i * 4 + 2] = 255; rgba[i * 4 + 3] = alpha[i];
  }
  slot->px = px;
  slot->texture = ab_texture_create_rgba(rgba, ATLAS_PX, ATLAS_PX);
  free(alpha); free(rgba);
  return slot->texture > 0 ? slot : NULL;
}

static int l_font(lua_State *S) {
  size_t n = 0;
  const char *name = luaL_checklstring(S, 1, &n);
  if (g_font_count >= FONT_MAX) return luaL_error(S, "font: too many fonts (max %d)", FONT_MAX);
  int32_t size = ab_asset_size_raw(name, (int32_t)n);
  if (size <= 0) return luaL_error(S, "font: no such asset '%s'", name);
  Font *font = &g_fonts[g_font_count];
  font->bytes = (unsigned char *)malloc((size_t)size);
  if (!font->bytes) return luaL_error(S, "font: out of memory");
  if (ab_asset_read_raw(name, (int32_t)n, font->bytes, size) <= 0) {
    free(font->bytes); font->bytes = NULL;
    return luaL_error(S, "font: asset_read failed");
  }
  if (!stbtt_InitFont(&font->info, font->bytes, 0)) {
    free(font->bytes); font->bytes = NULL;
    return luaL_error(S, "font: '%s' is not a TrueType font", name);
  }
  font->used = 0;
  lua_pushinteger(S, ++g_font_count);   /* handles are 1-based */
  return 1;
}

static Font *arg_font(lua_State *S, int idx) {
  lua_Integer handle = luaL_checkinteger(S, idx);
  if (handle < 1 || handle > g_font_count || !g_fonts[handle - 1].bytes)
    luaL_error(S, "bad font handle %d", (int)handle);
  return &g_fonts[handle - 1];
}

static int l_print(lua_State *S) {
  Font *font = arg_font(S, 1);
  size_t n = 0;
  const char *text = luaL_checklstring(S, 2, &n);
  double x = luaL_checknumber(S, 3), y = luaL_checknumber(S, 4);
  int32_t px = (int32_t)luaL_checkinteger(S, 5);
  uint32_t color = (uint32_t)(lua_Unsigned)luaL_checknumber(S, 6);
  FontAtlas *atlas = font_atlas(font, px);
  if (!atlas) return luaL_error(S, "print: atlas bake failed");
  if (n == 0) { lua_pushnumber(S, x); return 1; }
  ab_vertex *verts = (ab_vertex *)malloc(sizeof(ab_vertex) * 6 * n);
  if (!verts) return luaL_error(S, "print: out of memory");
  float fx = (float)x, fy = (float)y;
  int32_t count = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)text[i];
    if (c < 32 || c > 126) c = '?';
    stbtt_aligned_quad q;
    stbtt_GetBakedQuad(atlas->baked, ATLAS_PX, ATLAS_PX, c - 32, &fx, &fy, &q, 1);
    /* ab_vertex is {x, y, u, v, rgba} -- positions, then UVs, then colour. */
    ab_vertex tl = { q.x0, q.y0, q.s0, q.t0, color };
    ab_vertex tr = { q.x1, q.y0, q.s1, q.t0, color };
    ab_vertex bl = { q.x0, q.y1, q.s0, q.t1, color };
    ab_vertex br = { q.x1, q.y1, q.s1, q.t1, color };
    verts[count * 6 + 0] = tl; verts[count * 6 + 1] = tr; verts[count * 6 + 2] = bl;
    verts[count * 6 + 3] = bl; verts[count * 6 + 4] = tr; verts[count * 6 + 5] = br;
    count++;
  }
  ab_mesh(verts, count * 6, atlas->texture);
  free(verts);
  lua_pushnumber(S, fx);
  return 1;
}

static int l_measure(lua_State *S) {
  Font *font = arg_font(S, 1);
  size_t n = 0;
  const char *text = luaL_checklstring(S, 2, &n);
  int32_t px = (int32_t)luaL_checkinteger(S, 3);
  FontAtlas *atlas = font_atlas(font, px);
  if (!atlas) return luaL_error(S, "measure: atlas bake failed");
  float fx = 0, fy = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)text[i];
    if (c < 32 || c > 126) c = '?';
    stbtt_aligned_quad q;
    stbtt_GetBakedQuad(atlas->baked, ATLAS_PX, ATLAS_PX, c - 32, &fx, &fy, &q, 1);
  }
  lua_pushnumber(S, fx);
  return 1;
}

/* --- little-endian / big-endian integer reads over a region ------------- */
static int read_uint(lua_State *S, int bytes) {
  int32_t id = (int32_t)luaL_checkinteger(S, 1);
  int32_t off = (int32_t)luaL_checkinteger(S, 2);
  int big = lua_toboolean(S, 3);
  lua_Unsigned value = 0;
  for (int i = 0; i < bytes; i++) {
    int32_t v = ab_region_read_u8(id, off + i);
    if (v < 0) v = 0;
    value |= (lua_Unsigned)(v & 0xff) << (8 * (big ? bytes - 1 - i : i));
  }
  lua_pushinteger(S, (lua_Integer)value);
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
  uint32_t r = (uint32_t)luaL_checkinteger(S, 1) & 0xff;
  uint32_t g = (uint32_t)luaL_checkinteger(S, 2) & 0xff;
  uint32_t b = (uint32_t)luaL_checkinteger(S, 3) & 0xff;
  uint32_t a = (uint32_t)luaL_optinteger(S, 4, 255) & 0xff;
  lua_pushinteger(S, (lua_Integer)((r << 24) | (g << 16) | (b << 8) | a));
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
  { "mesh", l_mesh },
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
  { "print", l_print },
  { "measure", l_measure },
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
  g_has_tick = g_has_event = 0;
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

  luaL_newlib(L, AB_FUNCS);
  /* Constant tables so scripts never hard-code ABI numbers. Mirrors the C
   * SDK's AB_* defines; button ids are the libretro joypad numbering the
   * input_state import speaks. */
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
      { "A", 8 }, { "X", 9 }, { "L", 10 }, { "R", 11 }, { "MASK", 256 },
    };
    static const struct { const char *name;
                          const void *rows; int count; } GROUPS[] = {
      { "EVENT", EVENT, (int)(sizeof(EVENT) / sizeof(EVENT[0])) },
      { "FIT", FIT, (int)(sizeof(FIT) / sizeof(FIT[0])) },
      { "SAMPLE", SAMPLE, (int)(sizeof(SAMPLE) / sizeof(SAMPLE[0])) },
      { "DEVICE", DEVICE, (int)(sizeof(DEVICE) / sizeof(DEVICE[0])) },
      { "BTN", BTN, (int)(sizeof(BTN) / sizeof(BTN[0])) },
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

AB_EXPORT("ab_abi_version")
int32_t ab_abi_version(void) { return 1; }

AB_EXPORT("ab_init")
int32_t ab_init(void) {
  boot();
  return 0; /* 0 = success. A script error is NOT an init failure: the error
             * state still wants ticks so it can display itself. */
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
void ab_event(int32_t kind) {
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
  for (int32_t i = 0; i < g_font_count; i++) {
    for (int32_t s = 0; s < g_fonts[i].used; s++)
      if (g_fonts[i].sizes[s].texture > 0) ab_texture_destroy(g_fonts[i].sizes[s].texture);
    free(g_fonts[i].bytes);
    g_fonts[i].bytes = NULL;
    g_fonts[i].used = 0;
  }
  g_font_count = 0;
}
