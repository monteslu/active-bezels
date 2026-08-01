/*
 * runtime.c -- the prebuilt JavaScript Active Bezel runtime.
 *
 * This wasm IS the package entry point. A JS bezel ships this file's build as
 * `main.wasm` plus its own `main.js` in the archive; iterating on the bezel is
 * edit main.js + repack, with no compiler in the loop.
 *
 * Script contract (all globals, all optional except tick):
 *   function init()        -- once, after the script loads
 *   function tick(frame)   -- once per emulated frame; draw the whole scene
 *   function event(kind)   -- host lifecycle events (AB_EVENT numbers)
 *
 * The whole ab_* import surface is exposed as the global `ab` object, with the
 * same names and semantics as the Lua runtime. Errors never kill the session:
 * they are logged, drawn on screen, and the script is re-read on the next
 * ASSETS_RELOADED event so a fix is one repack away.
 *
 * Engine is quickjs-ng, embedded with the intrinsics a bezel can actually use
 * and nothing that needs a filesystem, a clock, or threads.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "quickjs.h"

#include "../../sdk/active_bezel.h"

#include "../common/ab_batteries.h"

/* ---------------------------------------------------------------- state -- */

static JSRuntime *g_rt = NULL;
static JSContext *g_ctx = NULL;
static char g_error[512];
static int g_has_tick = 0, g_has_event = 0;

static void set_error(const char *message) {
  size_t n = strlen(message);
  if (n >= sizeof(g_error)) n = sizeof(g_error) - 1;
  memcpy(g_error, message, n);
  g_error[n] = 0;
  ab_log_raw(g_error, (int32_t)n);
}

/* Turn a pending JS exception into the error panel's message. QuickJS keeps
 * the stack on the exception object, and the first stack line is what makes a
 * runtime error findable -- so it is appended when there is room. */
static void set_error_from_exception(JSContext *ctx, const char *what) {
  JSValue exc = JS_GetException(ctx);
  char buffer[512];
  const char *text = JS_ToCString(ctx, exc);
  int n = 0;

  if (text) {
    n = snprintf(buffer, sizeof(buffer), "%s: %s", what, text);
    JS_FreeCString(ctx, text);
  } else {
    n = snprintf(buffer, sizeof(buffer), "%s: unknown error", what);
  }
  if (n < 0) n = 0;
  if (n > (int)sizeof(buffer) - 1) n = (int)sizeof(buffer) - 1;

  if (JS_IsError(exc)) {
    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    const char *s = JS_IsUndefined(stack) ? NULL : JS_ToCString(ctx, stack);
    if (s) {
      /* Just the first frame: the panel has three lines, not a scrollback. */
      const char *nl = strchr(s, '\n');
      int len = nl ? (int)(nl - s) : (int)strlen(s);
      while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == ' ')) len--;
      if (len > 0 && n < (int)sizeof(buffer) - 4)
        snprintf(buffer + n, sizeof(buffer) - (size_t)n, " %.*s", len, s);
      JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, stack);
  }
  JS_FreeValue(ctx, exc);
  set_error(buffer);
}

/* --------------------------------------------------------- arg helpers -- */
/* Every binding below converts with these instead of JS_ToInt32/ToFloat64
 * directly: a conversion can itself throw (valueOf on an object), and the
 * bindings want a plain value, not an error path per argument. A throwing
 * argument degrades to 0 -- the call still reaches the host and the script
 * sees a wrong drawing, which is easier to debug than a silent no-op. */

static double arg_num(JSContext *ctx, JSValueConst v, double fallback) {
  double d;
  if (JS_IsUndefined(v)) return fallback;
  if (JS_ToFloat64(ctx, &d, v) < 0) { JS_FreeValue(ctx, JS_GetException(ctx)); return fallback; }
  return d;
}

static int32_t arg_int(JSContext *ctx, JSValueConst v, int32_t fallback) {
  double d = arg_num(ctx, v, (double)fallback);
  /* Truncate toward zero like a C cast; NaN/inf would be UB there. */
  if (!(d > -2147483649.0 && d < 2147483648.0)) return fallback;
  return (int32_t)d;
}

/* Colors are 0xRRGGBBAA. JS bitwise ops produce SIGNED int32, so 0xff0000ff
 * written as a literal is a positive double but `(r<<24)|...` is negative --
 * both must land on the same uint32. Going through double and masking covers
 * every way a script can spell a color. */
static uint32_t arg_rgba(JSContext *ctx, JSValueConst v) {
  double d = arg_num(ctx, v, 0);
  if (d < 0) d += 4294967296.0;          /* signed int32 from a bitwise op */
  if (!(d >= 0 && d < 4294967296.0)) return 0;
  return (uint32_t)d;
}

/* ---------------------------------------------------------- ab bindings -- */

#define AB_FN(name) static JSValue name(JSContext *ctx, JSValueConst this_val, \
                                        int argc, JSValueConst *argv)
/* argc/this_val are unused by most bindings; a single macro keeps the churn
 * out of every function body. */
#define AB_UNUSED() (void)this_val; (void)argc; (void)argv

AB_FN(js_clear) {
  AB_UNUSED();
  ab_clear(arg_rgba(ctx, argv[0]));
  return JS_UNDEFINED;
}

AB_FN(js_draw_game) {
  AB_UNUSED();
  ab_draw_game(arg_num(ctx, argv[0], 0), arg_num(ctx, argv[1], 0),
               arg_num(ctx, argv[2], 0), arg_num(ctx, argv[3], 0),
               arg_int(ctx, argv[4], 0));
  return JS_UNDEFINED;
}

AB_FN(js_draw_game_fit) {
  AB_UNUSED();
  ab_draw_game_fit(arg_int(ctx, argv[0], 0),
                   arg_num(ctx, argv[1], 0.5), arg_num(ctx, argv[2], 0.5),
                   arg_int(ctx, argv[3], 0));
  return JS_UNDEFINED;
}

AB_FN(js_fill_rect) {
  AB_UNUSED();
  ab_fill_rect(arg_num(ctx, argv[0], 0), arg_num(ctx, argv[1], 0),
               arg_num(ctx, argv[2], 0), arg_num(ctx, argv[3], 0),
               arg_rgba(ctx, argv[4]));
  return JS_UNDEFINED;
}

AB_FN(js_triangle) {
  AB_UNUSED();
  ab_triangle(arg_num(ctx, argv[0], 0), arg_num(ctx, argv[1], 0),
              arg_num(ctx, argv[2], 0), arg_num(ctx, argv[3], 0),
              arg_num(ctx, argv[4], 0), arg_num(ctx, argv[5], 0),
              arg_rgba(ctx, argv[6]));
  return JS_UNDEFINED;
}

AB_FN(js_text) {
  size_t n = 0;
  const char *s;
  AB_UNUSED();
  s = JS_ToCStringLen(ctx, &n, argv[0]);
  if (!s) return JS_EXCEPTION;
  ab_text_raw(s, (int32_t)n, arg_num(ctx, argv[1], 0), arg_num(ctx, argv[2], 0),
              arg_num(ctx, argv[3], 0), arg_rgba(ctx, argv[4]));
  JS_FreeCString(ctx, s);
  return JS_UNDEFINED;
}

AB_FN(js_scissor) {
  AB_UNUSED();
  ab_scissor(arg_num(ctx, argv[0], 0), arg_num(ctx, argv[1], 0),
             arg_num(ctx, argv[2], 0), arg_num(ctx, argv[3], 0));
  return JS_UNDEFINED;
}

AB_FN(js_scissor_reset) { AB_UNUSED(); (void)ctx; ab_scissor_reset(); return JS_UNDEFINED; }
AB_FN(js_push_transform) { AB_UNUSED(); return JS_NewInt32(ctx, ab_push_transform()); }
AB_FN(js_pop_transform) { AB_UNUSED(); return JS_NewInt32(ctx, ab_pop_transform()); }
AB_FN(js_reset_transform) { AB_UNUSED(); (void)ctx; ab_reset_transform(); return JS_UNDEFINED; }

AB_FN(js_translate) {
  AB_UNUSED();
  ab_translate(arg_num(ctx, argv[0], 0), arg_num(ctx, argv[1], 0));
  return JS_UNDEFINED;
}

AB_FN(js_scale) {
  double x;
  AB_UNUSED();
  x = arg_num(ctx, argv[0], 1);
  /* scale(2) is uniform, the common case; scale(2, 3) is explicit. */
  ab_scale(x, arg_num(ctx, argv[1], x));
  return JS_UNDEFINED;
}

AB_FN(js_rotate) { AB_UNUSED(); ab_rotate(arg_num(ctx, argv[0], 0)); return JS_UNDEFINED; }

/* ab.skew(x, y = 0) -- shear as tangents; skew(Math.PI/6) leans 30 degrees. */
AB_FN(js_skew) {
  AB_UNUSED();
  ab_skew(arg_num(ctx, argv[0], 0), argc > 1 ? arg_num(ctx, argv[1], 0) : 0);
  return JS_UNDEFINED;
}

AB_FN(js_transform2d) {
  AB_UNUSED();
  ab_transform2d(arg_num(ctx, argv[0], 0), arg_num(ctx, argv[1], 0),
                 arg_num(ctx, argv[2], 0), arg_num(ctx, argv[3], 0),
                 arg_num(ctx, argv[4], 0), arg_num(ctx, argv[5], 0));
  return JS_UNDEFINED;
}

/* ab.quad([{x,y} x4][, texture][, rgba]) -- perspective-correct textured
 * quad, corners clockwise from top-left. A tilt drawn this way reads as a
 * receding plane; the same corners through mesh() warp like a PS1 polygon. */
/* --- offscreen surfaces -------------------------------------------------- */
AB_FN(js_surface_create) {
  AB_UNUSED();
  return JS_NewInt32(ctx, ab_surface_create((int32_t)arg_num(ctx, argv[0], 0),
                                            (int32_t)arg_num(ctx, argv[1], 0)));
}
AB_FN(js_surface_target) {
  AB_UNUSED();
  return JS_NewInt32(ctx, ab_surface_target((int32_t)arg_num(ctx, argv[0], 0)));
}
AB_FN(js_surface_end) { AB_UNUSED(); return JS_NewInt32(ctx, ab_surface_end()); }
AB_FN(js_surface_filter) {
  size_t len = 0;
  const char *shader;
  int32_t result;
  AB_UNUSED();
  shader = JS_ToCStringLen(ctx, &len, argv[2]);
  if (!shader) return JS_EXCEPTION;
  result = ab_surface_filter_raw((int32_t)arg_num(ctx, argv[0], 0),
                                 (int32_t)arg_num(ctx, argv[1], 0), shader, (int32_t)len);
  JS_FreeCString(ctx, shader);
  return JS_NewInt32(ctx, result);
}

AB_FN(js_surface_preset) {
  size_t len = 0;
  const char *preset;
  int32_t result;
  AB_UNUSED();
  preset = JS_ToCStringLen(ctx, &len, argv[2]);
  if (!preset) return JS_EXCEPTION;
  result = ab_surface_preset_raw((int32_t)arg_num(ctx, argv[0], 0),
                                 (int32_t)arg_num(ctx, argv[1], 0), preset, (int32_t)len);
  JS_FreeCString(ctx, preset);
  return JS_NewInt32(ctx, result);
}

AB_FN(js_quad) {
  ab_point pts[4];
  int64_t i;
  int32_t texture;
  uint32_t rgba;
  AB_UNUSED();

  if (!JS_IsArray(argv[0]))
    return JS_ThrowTypeError(ctx, "quad: first argument must be an array of 4 corners");
  for (i = 0; i < 4; i++) {
    JSValue corner = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
    JSValue f;
    if (JS_IsException(corner)) return JS_EXCEPTION;
    f = JS_GetPropertyStr(ctx, corner, "x"); pts[i].x = arg_num(ctx, f, 0); JS_FreeValue(ctx, f);
    f = JS_GetPropertyStr(ctx, corner, "y"); pts[i].y = arg_num(ctx, f, 0); JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, corner);
  }
  texture = argc > 1 ? (int32_t)arg_num(ctx, argv[1], 0) : 0;
  rgba = argc > 2 ? (uint32_t)arg_num(ctx, argv[2], 0xffffffffu) : 0xffffffffu;
  return JS_NewInt32(ctx, ab_quad(pts, texture, rgba));
}

/* ab.mesh([{x, y, rgba, u, v}, ...][, texture]) -> emitted triangle count */
AB_FN(js_mesh) {
  int64_t count = 0;
  int32_t texture, emitted;
  ab_vertex *verts;
  int64_t i;
  AB_UNUSED();

  if (!JS_IsArray(argv[0]))
    return JS_ThrowTypeError(ctx, "mesh: first argument must be an array of vertices");
  if (JS_GetLength(ctx, argv[0], &count) < 0) return JS_EXCEPTION;
  texture = arg_int(ctx, argv[1], 0);
  if (count <= 0) return JS_NewInt32(ctx, 0);
  /* The host caps a frame at 16384 commands; a single mesh larger than this
   * is a script bug, and malloc'ing it would be the wrong failure. */
  if (count > 1 << 20) return JS_ThrowRangeError(ctx, "mesh: too many vertices");

  verts = (ab_vertex *)calloc((size_t)count, sizeof(ab_vertex));
  if (!verts) return JS_ThrowOutOfMemory(ctx);
  for (i = 0; i < count; i++) {
    JSValue v = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
    JSValue f;
    if (JS_IsException(v)) { free(verts); return JS_EXCEPTION; }
    if (!JS_IsObject(v)) {
      JS_FreeValue(ctx, v);
      free(verts);
      return JS_ThrowTypeError(ctx, "mesh: vertex %d is not an object", (int)i);
    }
    f = JS_GetPropertyStr(ctx, v, "x");    verts[i].x = (float)arg_num(ctx, f, 0); JS_FreeValue(ctx, f);
    f = JS_GetPropertyStr(ctx, v, "y");    verts[i].y = (float)arg_num(ctx, f, 0); JS_FreeValue(ctx, f);
    f = JS_GetPropertyStr(ctx, v, "rgba"); verts[i].rgba = arg_rgba(ctx, f);       JS_FreeValue(ctx, f);
    f = JS_GetPropertyStr(ctx, v, "u");    verts[i].u = (float)arg_num(ctx, f, 0); JS_FreeValue(ctx, f);
    f = JS_GetPropertyStr(ctx, v, "v");    verts[i].v = (float)arg_num(ctx, f, 0); JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, v);
  }
  emitted = ab_mesh(verts, (int32_t)count, texture);
  free(verts);
  return JS_NewInt32(ctx, emitted);
}

/* ab.texture_create(pixels, w, h) -- pixels is any typed array / ArrayBuffer
 * of RGBA bytes. Accepting a Uint8Array is the whole point of the JS runtime:
 * the Lua binding has to take a binary string. */
static uint8_t *bytes_from_value(JSContext *ctx, JSValueConst v, size_t *out_len) {
  size_t len = 0;
  uint8_t *p = JS_GetUint8Array(ctx, &len, v);
  if (p) { *out_len = len; return p; }
  JS_FreeValue(ctx, JS_GetException(ctx));
  /* Not a Uint8Array: try a view of any type, then a bare ArrayBuffer. */
  {
    size_t offset = 0, size = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &offset, &size, &bpe);
    if (!JS_IsException(buf)) {
      size_t total = 0;
      uint8_t *base = JS_GetArrayBuffer(ctx, &total, buf);
      JS_FreeValue(ctx, buf);
      if (base && offset + size <= total) { *out_len = size; return base + offset; }
      JS_FreeValue(ctx, JS_GetException(ctx));
    } else {
      JS_FreeValue(ctx, JS_GetException(ctx));
    }
  }
  p = JS_GetArrayBuffer(ctx, &len, v);
  if (p) { *out_len = len; return p; }
  JS_FreeValue(ctx, JS_GetException(ctx));
  return NULL;
}

AB_FN(js_texture_create) {
  size_t n = 0;
  uint8_t *pixels;
  int32_t w, h;
  AB_UNUSED();
  pixels = bytes_from_value(ctx, argv[0], &n);
  if (!pixels)
    return JS_ThrowTypeError(ctx, "texture_create: pixels must be a typed array or ArrayBuffer");
  w = arg_int(ctx, argv[1], 0);
  h = arg_int(ctx, argv[2], 0);
  if (w <= 0 || h <= 0 || n < (size_t)w * (size_t)h * 4)
    return JS_ThrowRangeError(ctx, "texture_create: need %d bytes of RGBA, got %d",
                              w * h * 4, (int)n);
  return JS_NewInt32(ctx, ab_texture_create_rgba(pixels, w, h));
}

AB_FN(js_texture_destroy) {
  AB_UNUSED();
  return JS_NewInt32(ctx, ab_texture_destroy(arg_int(ctx, argv[0], 0)));
}

AB_FN(js_draw_texture) {
  AB_UNUSED();
  return JS_NewInt32(ctx, ab_draw_texture(arg_int(ctx, argv[0], 0),
    arg_num(ctx, argv[1], 0), arg_num(ctx, argv[2], 0),
    arg_num(ctx, argv[3], 0), arg_num(ctx, argv[4], 0)));
}

AB_FN(js_draw_texture_rect) {
  AB_UNUSED();
  return JS_NewInt32(ctx, ab_draw_texture_rect(arg_int(ctx, argv[0], 0),
    arg_num(ctx, argv[1], 0), arg_num(ctx, argv[2], 0),
    arg_num(ctx, argv[3], 0), arg_num(ctx, argv[4], 0),
    arg_int(ctx, argv[5], 0), arg_int(ctx, argv[6], 0),
    arg_int(ctx, argv[7], 0), arg_int(ctx, argv[8], 0)));
}

AB_FN(js_effect_set) {
  size_t n = 0;
  const char *src;
  int32_t ok;
  AB_UNUSED();
  src = JS_ToCStringLen(ctx, &n, argv[0]);
  if (!src) return JS_EXCEPTION;
  ok = ab_effect_set_raw(src, (int32_t)n);
  JS_FreeCString(ctx, src);
  return JS_NewBool(ctx, ok != 0);
}

AB_FN(js_effect_clear) { AB_UNUSED(); return JS_NewInt32(ctx, ab_effect_clear()); }

AB_FN(js_game_width) { AB_UNUSED(); return JS_NewInt32(ctx, ab_game_width()); }
AB_FN(js_game_height) { AB_UNUSED(); return JS_NewInt32(ctx, ab_game_height()); }

AB_FN(js_game_pixel) {
  AB_UNUSED();
  /* 0xRRGGBBAA does not fit a signed int32; NewUint32 keeps it positive so
   * comparing against a 0xff...  literal in a script works. */
  return JS_NewUint32(ctx, ab_game_pixel(arg_int(ctx, argv[0], 0), arg_int(ctx, argv[1], 0)));
}

AB_FN(js_logical_width) { AB_UNUSED(); return JS_NewInt32(ctx, ab_logical_width()); }
AB_FN(js_logical_height) { AB_UNUSED(); return JS_NewInt32(ctx, ab_logical_height()); }
AB_FN(js_physical_width) { AB_UNUSED(); return JS_NewInt32(ctx, ab_physical_width()); }
AB_FN(js_physical_height) { AB_UNUSED(); return JS_NewInt32(ctx, ab_physical_height()); }
AB_FN(js_elapsed_ms) { AB_UNUSED(); return JS_NewFloat64(ctx, ab_elapsed_ms()); }
AB_FN(js_delta_ms) { AB_UNUSED(); return JS_NewFloat64(ctx, ab_delta_ms()); }

/* ab.input(port, device, index, id) -- same argument order and defaults as
 * the Lua binding, so a bezel ports between the two by changing syntax only. */
AB_FN(js_input) {
  AB_UNUSED();
  return JS_NewInt32(ctx, ab_input_state(
    arg_int(ctx, argv[0], 0), arg_int(ctx, argv[1], 1),
    arg_int(ctx, argv[2], 0), arg_int(ctx, argv[3], 0)));
}

AB_FN(js_log) {
  size_t n = 0;
  const char *s;
  AB_UNUSED();
  s = JS_ToCStringLen(ctx, &n, argv[0]);
  if (!s) return JS_EXCEPTION;
  ab_log_raw(s, (int32_t)n);
  JS_FreeCString(ctx, s);
  return JS_UNDEFINED;
}

/* Missing regions return null, not -1: a script tests `if (ram === null)`
 * rather than remembering a sentinel. Same shape as the Lua nil. */
AB_FN(js_region) {
  size_t n = 0;
  const char *name;
  int32_t id;
  AB_UNUSED();
  name = JS_ToCStringLen(ctx, &n, argv[0]);
  if (!name) return JS_EXCEPTION;
  id = ab_region_find_raw(name, (int32_t)n);
  JS_FreeCString(ctx, name);
  return id < 0 ? JS_NULL : JS_NewInt32(ctx, id);
}

AB_FN(js_region_find_id) {
  int32_t id;
  AB_UNUSED();
  id = ab_region_find_id(arg_int(ctx, argv[0], 0));
  return id < 0 ? JS_NULL : JS_NewInt32(ctx, id);
}

AB_FN(js_region_size) { AB_UNUSED(); return JS_NewInt32(ctx, ab_region_size(arg_int(ctx, argv[0], 0))); }
AB_FN(js_region_flags) { AB_UNUSED(); return JS_NewInt32(ctx, ab_region_flags(arg_int(ctx, argv[0], 0))); }
AB_FN(js_region_offset) { AB_UNUSED(); return JS_NewInt32(ctx, ab_region_offset(arg_int(ctx, argv[0], 0))); }
AB_FN(js_region_generation) { AB_UNUSED(); return JS_NewInt32(ctx, ab_region_generation()); }
AB_FN(js_region_count) { AB_UNUSED(); return JS_NewInt32(ctx, ab_region_count()); }

AB_FN(js_read_u8) {
  AB_UNUSED();
  return JS_NewInt32(ctx, ab_region_read_u8(arg_int(ctx, argv[0], 0), arg_int(ctx, argv[1], 0)));
}

AB_FN(js_write_u8) {
  AB_UNUSED();
  return JS_NewInt32(ctx, ab_region_write_u8(arg_int(ctx, argv[0], 0),
                                             arg_int(ctx, argv[1], 0),
                                             arg_int(ctx, argv[2], 0)));
}

/* ab.read(id, offset, length) -> Uint8Array. The per-byte import is fine for
 * a few reads; a typed array is what a table-driven decoder wants, and it is
 * the natural JS shape (the Lua runtime has to hand back a binary string). */
AB_FN(js_read) {
  int32_t id, off, len, i;
  uint8_t *buf;
  JSValue out;
  AB_UNUSED();
  id = arg_int(ctx, argv[0], 0);
  off = arg_int(ctx, argv[1], 0);
  len = arg_int(ctx, argv[2], 0);
  if (len <= 0 || len > 1 << 20) return JS_ThrowRangeError(ctx, "read: bad length");
  buf = (uint8_t *)malloc((size_t)len);
  if (!buf) return JS_ThrowOutOfMemory(ctx);
  for (i = 0; i < len; i++) {
    int32_t v = ab_region_read_u8(id, off + i);
    buf[i] = (uint8_t)(v < 0 ? 0 : v);   /* out of range reads as 0, like Lua's */
  }
  out = JS_NewUint8ArrayCopy(ctx, buf, (size_t)len);
  free(buf);
  return out;
}

/* ab.asset(name) -> Uint8Array, or null when the package has no such entry. */
AB_FN(js_asset) {
  size_t n = 0;
  const char *name;
  int32_t size, got;
  uint8_t *buf;
  JSValue out;
  AB_UNUSED();
  name = JS_ToCStringLen(ctx, &n, argv[0]);
  if (!name) return JS_EXCEPTION;
  size = ab_asset_size_raw(name, (int32_t)n);
  if (size < 0) { JS_FreeCString(ctx, name); return JS_NULL; }
  buf = (uint8_t *)malloc((size_t)size + 1);   /* +1: never malloc(0) */
  if (!buf) { JS_FreeCString(ctx, name); return JS_ThrowOutOfMemory(ctx); }
  got = ab_asset_read_raw(name, (int32_t)n, buf, size);
  JS_FreeCString(ctx, name);
  out = JS_NewUint8ArrayCopy(ctx, buf, (size_t)(got < 0 ? 0 : got));
  free(buf);
  return out;
}

/* ab.asset_text(name) -> string, or null. Reading a JSON/text asset through
 * ab.asset() means hand-decoding UTF-8 in script; this is the shortcut.
 * The buffer is NUL-terminated before it becomes a string, because a JSON
 * parse over a non-terminated buffer over-reads (learned in wasmcart-jsgame). */
AB_FN(js_asset_text) {
  size_t n = 0;
  const char *name;
  int32_t size, got;
  char *buf;
  JSValue out;
  AB_UNUSED();
  name = JS_ToCStringLen(ctx, &n, argv[0]);
  if (!name) return JS_EXCEPTION;
  size = ab_asset_size_raw(name, (int32_t)n);
  if (size < 0) { JS_FreeCString(ctx, name); return JS_NULL; }
  buf = (char *)malloc((size_t)size + 1);
  if (!buf) { JS_FreeCString(ctx, name); return JS_ThrowOutOfMemory(ctx); }
  got = ab_asset_read_raw(name, (int32_t)n, buf, size);
  JS_FreeCString(ctx, name);
  if (got < 0) got = 0;
  buf[got] = 0;
  out = JS_NewStringLen(ctx, buf, (size_t)got);
  free(buf);
  return out;
}

AB_FN(js_config_bool) {
  size_t n = 0;
  const char *key;
  int32_t v;
  AB_UNUSED();
  key = JS_ToCStringLen(ctx, &n, argv[0]);
  if (!key) return JS_EXCEPTION;
  v = ab_config_bool_raw(key, (int32_t)n);
  JS_FreeCString(ctx, key);
  return JS_NewBool(ctx, v != 0);
}

AB_FN(js_config_number) {
  size_t n = 0;
  const char *key;
  double v;
  AB_UNUSED();
  key = JS_ToCStringLen(ctx, &n, argv[0]);
  if (!key) return JS_EXCEPTION;
  v = ab_config_number_raw(key, (int32_t)n);
  JS_FreeCString(ctx, key);
  return JS_NewFloat64(ctx, v);
}

AB_FN(js_config_string) {
  size_t n = 0;
  const char *key;
  int32_t len, got;
  char *buf;
  JSValue out;
  AB_UNUSED();
  key = JS_ToCStringLen(ctx, &n, argv[0]);
  if (!key) return JS_EXCEPTION;
  len = ab_config_string_length_raw(key, (int32_t)n);
  if (len < 0) { JS_FreeCString(ctx, key); return JS_NULL; }
  buf = (char *)malloc((size_t)len + 1);
  if (!buf) { JS_FreeCString(ctx, key); return JS_ThrowOutOfMemory(ctx); }
  got = ab_config_string_read_raw(key, (int32_t)n, buf, len);
  JS_FreeCString(ctx, key);
  if (got < 0) got = 0;
  buf[got] = 0;
  out = JS_NewStringLen(ctx, buf, (size_t)got);
  free(buf);
  return out;
}

/* --- images --------------------------------------------------------------
 * ab.image('assets/logo.png') -> { texture, width, height }
 * ab.image_data(bytes) does the same for a Uint8Array. Both go through the
 * shared batteries so every runtime decodes identically.
 */
static JSValue push_image(JSContext *ctx, int ok, int32_t tex, int w, int h, const char *err) {
  JSValue obj;
  if (!ok) return JS_ThrowInternalError(ctx, "image: %s", err ? err : "decode failed");
  obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) return obj;
  JS_SetPropertyStr(ctx, obj, "texture", JS_NewInt32(ctx, tex));
  JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, w));
  JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, h));
  return obj;
}

AB_FN(js_image_data) {
  size_t n = 0;
  uint8_t *bytes;
  int32_t tex = 0;
  int w = 0, h = 0, ok;
  const char *err = NULL;
  AB_UNUSED();
  bytes = bytes_from_value(ctx, argv[0], &n);
  if (!bytes)
    return JS_ThrowTypeError(ctx, "image_data: expected a typed array or ArrayBuffer");
  ok = ab_bat_image_from_memory(bytes, (int)n, &tex, &w, &h, &err);
  return push_image(ctx, ok, tex, w, h, err);
}

AB_FN(js_image) {
  size_t n = 0;
  const char *name;
  int32_t tex = 0;
  int w = 0, h = 0, ok;
  const char *err = NULL;
  AB_UNUSED();
  name = JS_ToCStringLen(ctx, &n, argv[0]);
  if (!name) return JS_EXCEPTION;
  ok = ab_bat_image_from_asset(name, (int)n, &tex, &w, &h, &err);
  JS_FreeCString(ctx, name);
  return push_image(ctx, ok, tex, w, h, err);
}

/* --- TrueType text -------------------------------------------------------
 * ab.font('assets/font.ttf') -> handle
 * ab.print(font, text, x, y, px, rgba) -> pen X after the last glyph
 * ab.measure(font, text, px) -> width
 * ab.font_metrics(font, px) -> { ascent, descent, lineHeight }
 * All shared with the other runtimes (see runtimes/common/ab_batteries.c).
 */
AB_FN(js_font) {
  size_t n = 0;
  const char *name;
  const char *err = NULL;
  int32_t handle;
  AB_UNUSED();
  name = JS_ToCStringLen(ctx, &n, argv[0]);
  if (!name) return JS_EXCEPTION;
  handle = ab_bat_font_load(name, (int)n, &err);
  JS_FreeCString(ctx, name);
  if (handle <= 0) return JS_ThrowInternalError(ctx, "font: %s", err ? err : "load failed");
  return JS_NewInt32(ctx, handle);
}

AB_FN(js_print) {
  size_t n = 0;
  const char *text;
  const char *err = NULL;
  double advance;
  AB_UNUSED();
  text = JS_ToCStringLen(ctx, &n, argv[1]);
  if (!text) return JS_EXCEPTION;
  advance = ab_bat_font_print(arg_int(ctx, argv[0], 0), text, (int)n,
    arg_num(ctx, argv[2], 0), arg_num(ctx, argv[3], 0),
    arg_int(ctx, argv[4], 0), arg_rgba(ctx, argv[5]), &err);
  JS_FreeCString(ctx, text);
  if (err) return JS_ThrowInternalError(ctx, "print: %s", err);
  return JS_NewFloat64(ctx, advance);
}

AB_FN(js_measure) {
  size_t n = 0;
  const char *text;
  const char *err = NULL;
  double w;
  AB_UNUSED();
  text = JS_ToCStringLen(ctx, &n, argv[1]);
  if (!text) return JS_EXCEPTION;
  w = ab_bat_font_measure(arg_int(ctx, argv[0], 0), text, (int)n, arg_int(ctx, argv[2], 0), &err);
  JS_FreeCString(ctx, text);
  if (err) return JS_ThrowInternalError(ctx, "measure: %s", err);
  return JS_NewFloat64(ctx, w);
}

/* Returns an object, not three values: JS has no multiple return, and
 * {ascent, descent, lineHeight} destructures at the call site. */
AB_FN(js_font_metrics) {
  double a = 0, d = 0, lh = 0;
  const char *err = NULL;
  JSValue obj;
  AB_UNUSED();
  if (!ab_bat_font_metrics(arg_int(ctx, argv[0], 0), arg_int(ctx, argv[1], 0),
                           &a, &d, &lh, &err))
    return JS_ThrowInternalError(ctx, "font_metrics: %s", err ? err : "failed");
  obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) return obj;
  JS_SetPropertyStr(ctx, obj, "ascent", JS_NewFloat64(ctx, a));
  JS_SetPropertyStr(ctx, obj, "descent", JS_NewFloat64(ctx, d));
  JS_SetPropertyStr(ctx, obj, "lineHeight", JS_NewFloat64(ctx, lh));
  return obj;
}

/* --- multi-byte region reads (shared) ------------------------------------
 * read_u16/24/32(region, offset[, bigEndian]). NewUint32 rather than Int32:
 * a 32-bit read of 0x41414141 is fine either way, but 0xffffffff must not
 * come back as -1. */
static JSValue read_uint(JSContext *ctx, JSValueConst *argv, int bytes) {
  return JS_NewUint32(ctx, ab_bat_read_uint(arg_int(ctx, argv[0], 0),
                                            arg_int(ctx, argv[1], 0),
                                            bytes, JS_ToBool(ctx, argv[2])));
}
AB_FN(js_read_u16) { AB_UNUSED(); return read_uint(ctx, argv, 2); }
AB_FN(js_read_u24) { AB_UNUSED(); return read_uint(ctx, argv, 3); }
AB_FN(js_read_u32) { AB_UNUSED(); return read_uint(ctx, argv, 4); }

/* rgb(r, g, b[, a]) -> packed 0xRRGGBBAA, the format every command takes */
AB_FN(js_rgb) {
  AB_UNUSED();
  return JS_NewUint32(ctx, ab_bat_rgba(arg_int(ctx, argv[0], 0), arg_int(ctx, argv[1], 0),
                                       arg_int(ctx, argv[2], 0), arg_int(ctx, argv[3], 255)));
}

static const JSCFunctionListEntry AB_FUNCS[] = {
  JS_CFUNC_DEF("clear", 1, js_clear),
  JS_CFUNC_DEF("draw_game", 5, js_draw_game),
  JS_CFUNC_DEF("draw_game_fit", 4, js_draw_game_fit),
  JS_CFUNC_DEF("fill_rect", 5, js_fill_rect),
  JS_CFUNC_DEF("triangle", 7, js_triangle),
  JS_CFUNC_DEF("text", 5, js_text),
  JS_CFUNC_DEF("scissor", 4, js_scissor),
  JS_CFUNC_DEF("scissor_reset", 0, js_scissor_reset),
  JS_CFUNC_DEF("push_transform", 0, js_push_transform),
  JS_CFUNC_DEF("pop_transform", 0, js_pop_transform),
  JS_CFUNC_DEF("reset_transform", 0, js_reset_transform),
  JS_CFUNC_DEF("translate", 2, js_translate),
  JS_CFUNC_DEF("scale", 2, js_scale),
  JS_CFUNC_DEF("rotate", 1, js_rotate),
  JS_CFUNC_DEF("skew", 2, js_skew),
  JS_CFUNC_DEF("transform2d", 6, js_transform2d),
  JS_CFUNC_DEF("quad", 3, js_quad),
  JS_CFUNC_DEF("surface_create", 2, js_surface_create),
  JS_CFUNC_DEF("surface_target", 1, js_surface_target),
  JS_CFUNC_DEF("surface_end", 0, js_surface_end),
  JS_CFUNC_DEF("surface_filter", 3, js_surface_filter),
  JS_CFUNC_DEF("surface_preset", 3, js_surface_preset),
  JS_CFUNC_DEF("mesh", 2, js_mesh),
  JS_CFUNC_DEF("texture_create", 3, js_texture_create),
  JS_CFUNC_DEF("texture_destroy", 1, js_texture_destroy),
  JS_CFUNC_DEF("draw_texture", 5, js_draw_texture),
  JS_CFUNC_DEF("draw_texture_rect", 9, js_draw_texture_rect),
  JS_CFUNC_DEF("effect_set", 1, js_effect_set),
  JS_CFUNC_DEF("effect_clear", 0, js_effect_clear),
  JS_CFUNC_DEF("game_width", 0, js_game_width),
  JS_CFUNC_DEF("game_height", 0, js_game_height),
  JS_CFUNC_DEF("game_pixel", 2, js_game_pixel),
  JS_CFUNC_DEF("logical_width", 0, js_logical_width),
  JS_CFUNC_DEF("logical_height", 0, js_logical_height),
  JS_CFUNC_DEF("physical_width", 0, js_physical_width),
  JS_CFUNC_DEF("physical_height", 0, js_physical_height),
  JS_CFUNC_DEF("elapsed_ms", 0, js_elapsed_ms),
  JS_CFUNC_DEF("delta_ms", 0, js_delta_ms),
  JS_CFUNC_DEF("input", 4, js_input),
  JS_CFUNC_DEF("log", 1, js_log),
  JS_CFUNC_DEF("region", 1, js_region),
  JS_CFUNC_DEF("region_find_id", 1, js_region_find_id),
  JS_CFUNC_DEF("region_size", 1, js_region_size),
  JS_CFUNC_DEF("region_flags", 1, js_region_flags),
  JS_CFUNC_DEF("region_offset", 1, js_region_offset),
  JS_CFUNC_DEF("region_generation", 0, js_region_generation),
  JS_CFUNC_DEF("region_count", 0, js_region_count),
  JS_CFUNC_DEF("read_u8", 2, js_read_u8),
  JS_CFUNC_DEF("write_u8", 3, js_write_u8),
  JS_CFUNC_DEF("read", 3, js_read),
  JS_CFUNC_DEF("asset", 1, js_asset),
  JS_CFUNC_DEF("asset_text", 1, js_asset_text),
  JS_CFUNC_DEF("image", 1, js_image),
  JS_CFUNC_DEF("image_data", 1, js_image_data),
  JS_CFUNC_DEF("font", 1, js_font),
  JS_CFUNC_DEF("print", 6, js_print),
  JS_CFUNC_DEF("measure", 3, js_measure),
  JS_CFUNC_DEF("font_metrics", 2, js_font_metrics),
  JS_CFUNC_DEF("read_u16", 3, js_read_u16),
  JS_CFUNC_DEF("read_u24", 3, js_read_u24),
  JS_CFUNC_DEF("read_u32", 3, js_read_u32),
  JS_CFUNC_DEF("config_bool", 1, js_config_bool),
  JS_CFUNC_DEF("config_number", 1, js_config_number),
  JS_CFUNC_DEF("config_string", 1, js_config_string),
  JS_CFUNC_DEF("rgb", 4, js_rgb),
};

/* console.log(...) -> ab_log. Scripts written anywhere else reach for console
 * before they reach for ab.log, and a console-shaped hole is a confusing first
 * five minutes. Arguments are joined with spaces, like every other console. */
AB_FN(js_console_log) {
  int i;
  AB_UNUSED();
  for (i = 0; i < argc; i++) {
    size_t n = 0;
    const char *s = JS_ToCStringLen(ctx, &n, argv[i]);
    if (!s) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
    if (i > 0) ab_log_raw(" ", 1);
    ab_log_raw(s, (int32_t)n);
    JS_FreeCString(ctx, s);
  }
  return JS_UNDEFINED;
}

/* ------------------------------------------------------------ constants -- */

typedef struct { const char *name; int32_t value; } ab_const;

static void define_consts(JSContext *ctx, JSValue target, const char *group,
                          const ab_const *rows, int count) {
  JSValue obj = JS_NewObject(ctx);
  int i;
  for (i = 0; i < count; i++)
    JS_SetPropertyStr(ctx, obj, rows[i].name, JS_NewInt32(ctx, rows[i].value));
  JS_SetPropertyStr(ctx, target, group, obj);
}

/* -------------------------------------------------------- script loading -- */

static int load_script(void) {
  static const char *CANDIDATES[] = { "main.js", "assets/main.js" };
  const char *name = NULL;
  int32_t size = -1, got;
  char *source;
  JSValue global, result, fn;
  unsigned i;

  for (i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++) {
    size = ab_asset_size(CANDIDATES[i]);
    if (size >= 0) { name = CANDIDATES[i]; break; }
  }
  if (!name) { set_error("js runtime: no main.js (or assets/main.js) in the package"); return 0; }

  /* +1 and an explicit NUL: JS_Eval takes a length but the engine still
   * expects a terminated buffer, and an unterminated one over-reads. */
  source = (char *)malloc((size_t)size + 1);
  if (!source) { set_error("js runtime: out of memory reading main.js"); return 0; }
  got = ab_asset_read(name, source, size);
  if (got < 0) { free(source); set_error("js runtime: asset_read failed for main.js"); return 0; }
  source[got] = 0;

  /* JS_EVAL_TYPE_GLOBAL, not MODULE: the contract is global init/tick/event,
   * and a module's top level would put them in module scope where the runtime
   * cannot see them. There is no filesystem, so `import` has nowhere to go. */
  result = JS_Eval(g_ctx, source, (size_t)got, name, JS_EVAL_TYPE_GLOBAL);
  free(source);
  if (JS_IsException(result)) {
    JS_FreeValue(g_ctx, result);
    set_error_from_exception(g_ctx, "js runtime");
    return 0;
  }
  JS_FreeValue(g_ctx, result);

  global = JS_GetGlobalObject(g_ctx);
  fn = JS_GetPropertyStr(g_ctx, global, "tick");
  g_has_tick = JS_IsFunction(g_ctx, fn);
  JS_FreeValue(g_ctx, fn);
  fn = JS_GetPropertyStr(g_ctx, global, "event");
  g_has_event = JS_IsFunction(g_ctx, fn);
  JS_FreeValue(g_ctx, fn);
  if (!g_has_tick) {
    JS_FreeValue(g_ctx, global);
    set_error("js runtime: main.js must define a global function tick(frame)");
    return 0;
  }

  fn = JS_GetPropertyStr(g_ctx, global, "init");
  if (JS_IsFunction(g_ctx, fn)) {
    result = JS_Call(g_ctx, fn, global, 0, NULL);
    if (JS_IsException(result)) {
      JS_FreeValue(g_ctx, result);
      JS_FreeValue(g_ctx, fn);
      JS_FreeValue(g_ctx, global);
      set_error_from_exception(g_ctx, "js runtime: init()");
      return 0;
    }
    JS_FreeValue(g_ctx, result);
  }
  JS_FreeValue(g_ctx, fn);
  JS_FreeValue(g_ctx, global);

  g_error[0] = 0;
  return 1;
}

static void teardown(void) {
  if (g_ctx) { JS_FreeContext(g_ctx); g_ctx = NULL; }
  if (g_rt) { JS_FreeRuntime(g_rt); g_rt = NULL; }
  g_has_tick = g_has_event = 0;
}

static void boot(void) {
  JSValue global, ab, console;

  teardown();
  g_rt = JS_NewRuntime();
  if (!g_rt) { set_error("js runtime: JS_NewRuntime failed"); return; }
  /* A runaway script must not wedge the emulator. The host ticks a bezel
   * inside its own frame budget, so a hard memory ceiling plus the stack
   * limit below turn an infinite allocation into a catchable exception and
   * the error panel, instead of a hung session. */
  JS_SetMemoryLimit(g_rt, 64u * 1024u * 1024u);
  JS_SetMaxStackSize(g_rt, 512u * 1024u);

  /* NewContextRaw + explicit intrinsics, not JS_NewContext: this is where the
   * size budget is won or lost. Excluded on purpose --
   *   Date        -- would need a wall clock; time comes from ab.elapsed_ms
   *   Proxy/WeakRef/Promise -- no event loop here, nothing pumps job queues
   *   BigInt      -- region values are all <= 32 bits
   * Kept: the objects a drawing script actually uses. */
  g_ctx = JS_NewContextRaw(g_rt);
  if (!g_ctx) { set_error("js runtime: JS_NewContextRaw failed"); teardown(); return; }
  JS_AddIntrinsicBaseObjects(g_ctx);
  /* Eval is not optional here even though scripts never call eval(): without
   * it JS_Eval itself throws "eval is not supported", so main.js cannot load
   * at all. Found by the load path landing on the error panel. */
  JS_AddIntrinsicEval(g_ctx);
  JS_AddIntrinsicRegExpCompiler(g_ctx);
  JS_AddIntrinsicRegExp(g_ctx);
  JS_AddIntrinsicJSON(g_ctx);
  JS_AddIntrinsicMapSet(g_ctx);
  JS_AddIntrinsicTypedArrays(g_ctx);

  global = JS_GetGlobalObject(g_ctx);
  ab = JS_NewObject(g_ctx);
  JS_SetPropertyFunctionList(g_ctx, ab, AB_FUNCS,
                             (int)(sizeof(AB_FUNCS) / sizeof(AB_FUNCS[0])));

  /* Constant tables so scripts never hard-code ABI numbers. Mirrors the C
   * SDK's AB_* defines and the Lua runtime's tables exactly; button ids are
   * the libretro joypad numbering the input_state import speaks. */
  {
    static const ab_const EVENT[] = {
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
    /* ab.GAME: pass as a texture handle to sample the LIVE GAME FRAME, e.g.
     * ab.quad(corners, ab.GAME) maps the running game onto a tilted plane. */
    JS_SetPropertyStr(g_ctx, ab, "GAME", JS_NewInt32(g_ctx, -1));
    define_consts(g_ctx, ab, "EVENT", EVENT, (int)(sizeof(EVENT) / sizeof(EVENT[0])));
    define_consts(g_ctx, ab, "FIT", FIT, (int)(sizeof(FIT) / sizeof(FIT[0])));
    define_consts(g_ctx, ab, "SAMPLE", SAMPLE, (int)(sizeof(SAMPLE) / sizeof(SAMPLE[0])));
    define_consts(g_ctx, ab, "DEVICE", DEVICE, (int)(sizeof(DEVICE) / sizeof(DEVICE[0])));
    define_consts(g_ctx, ab, "BTN", BTN, (int)(sizeof(BTN) / sizeof(BTN[0])));
  }
  JS_SetPropertyStr(g_ctx, global, "ab", ab);

  console = JS_NewObject(g_ctx);
  JS_SetPropertyStr(g_ctx, console, "log", JS_NewCFunction(g_ctx, js_console_log, "log", 1));
  /* warn/error/info alias log: there is one sink (ab_log) and separate
   * levels would only be a lie about where the output went. */
  JS_SetPropertyStr(g_ctx, console, "warn", JS_NewCFunction(g_ctx, js_console_log, "warn", 1));
  JS_SetPropertyStr(g_ctx, console, "error", JS_NewCFunction(g_ctx, js_console_log, "error", 1));
  JS_SetPropertyStr(g_ctx, console, "info", JS_NewCFunction(g_ctx, js_console_log, "info", 1));
  JS_SetPropertyStr(g_ctx, global, "console", console);
  JS_FreeValue(g_ctx, global);

  load_script();
}

/* Call a global script function with one number argument. Any exception ends
 * up on the error panel rather than propagating into the host. */
static void call_hook(const char *name, double arg) {
  JSValue global = JS_GetGlobalObject(g_ctx);
  JSValue fn = JS_GetPropertyStr(g_ctx, global, name);
  JSValue argument = JS_NewFloat64(g_ctx, arg);
  JSValue result = JS_Call(g_ctx, fn, global, 1, (JSValueConst *)&argument);
  char what[64];

  JS_FreeValue(g_ctx, argument);
  JS_FreeValue(g_ctx, fn);
  JS_FreeValue(g_ctx, global);
  if (JS_IsException(result)) {
    snprintf(what, sizeof(what), "%s()", name);
    set_error_from_exception(g_ctx, what);
  }
  JS_FreeValue(g_ctx, result);
}

/* ------------------------------------------------------------ entrypoints -- */

AB_EXPORT("ab_abi_version")
int32_t ab_abi_version(void) { return 1; }

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

AB_EXPORT("ab_tick")
void ab_tick(uint64_t frame) {
  if (g_error[0] || !g_ctx || !g_has_tick) {
    ab_clear(0x201018ffu);
    ab_draw_game_fit(0, 0.5, 0.35, 0);
    ab_text("js bezel error:", 40, 40, 30, 0xff8080ffu);
    ab_text(g_error[0] ? g_error : "script not loaded", 40, 84, 24, 0xffd0d0ffu);
    ab_text("fix main.js and repack -- the runtime reloads it", 40, 124, 22, 0x9098b0ffu);
    return;
  }
  /* double, not int64: a JS number is a double anyway, and the emulator's
   * frame counter is nowhere near 2^53. */
  call_hook("tick", (double)frame);
}

AB_EXPORT("ab_event")
void ab_event(int32_t kind, uint32_t data) {
  (void)data;                          /* reserved by the ABI, unused today */
  /* ASSETS_RELOADED (6): the package archive changed under us -- re-read
   * main.js. This is the whole iteration story: edit, repack, replay. */
  if (kind == 6) { boot(); return; }
  if (!g_ctx || !g_has_event || g_error[0]) return;
  call_hook("event", (double)kind);
}

AB_EXPORT("ab_shutdown")
void ab_shutdown(void) {
  teardown();
  ab_bat_shutdown();
}
