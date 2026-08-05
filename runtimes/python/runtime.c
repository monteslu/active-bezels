/*
 * runtime.c -- the prebuilt MicroPython Active Bezel runtime.
 *
 * This wasm IS the package entry point. A Python bezel ships this file's
 * build as `main.wasm` plus its own `main.py` in the archive; iterating on
 * the bezel is edit main.py + reload, with no compiler in the loop.
 *
 * Script contract (module-level functions, all optional except tick):
 *   def init():        -- once, after the script loads
 *   def tick(frame):   -- once per emulated frame; draw the whole scene
 *   def event(kind):   -- host lifecycle events (ab.EVENT.* values)
 *
 * The import surface arrives as the `ab` module, in a pygame-flavoured
 * shape: familiar, not compatible. That phrasing is deliberate and matches
 * the pycretro family (gbapy/mdpy/gtpy) -- a pygame user should be able to
 * guess the API, without us pretending to be pygame.
 *
 * Errors never kill the session: they are logged, drawn on screen, and the
 * script is re-read on the next ASSETS_RELOADED event so a fix is one
 * reload away.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "py/builtin.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/stackctrl.h"
#include "py/objmodule.h"
#include "py/objstr.h"
#include "py/objtuple.h"
#include "py/objtype.h"

#include "../common/ab_batteries.h"
#include "../../sdk/active_bezel.h"

/* The platform redraw profiles (`nes`/`gb`/`md`/`snes`/`msx`/`pce`).
 * All logic lives in runtimes/common/ab_profiles.c, shared with the other
 * three runtimes; ab_profiles_py.c is marshaling only. */
void ab_profiles_py_register(void);
void ab_profiles_py_shutdown(void);

/* ---------------------------------------------------------------- state -- */

/* The GC heap.
 *
 * 2MB, not 512KB: a bezel's assets land in it too. The first scaffold run
 * with a real font died on "MemoryError: allocating 511593 bytes" -- that is
 * ab.asset() returning a 500KB TTF as a bytes object, which alone did not
 * fit beside the script's own objects. Fonts, PNGs and ROM slices are all
 * routine here, so size the heap for data rather than for code.
 *
 * This is a static array so it costs nothing until touched (wasm zero-pages
 * on demand), and ALLOW_MEMORY_GROWTH still covers the rest. */
#define GC_HEAP_BYTES (2 * 1024 * 1024)
static char g_heap[GC_HEAP_BYTES];

static char g_error[512];
static int g_booted = 0;               /* MicroPython initialised */
static int g_has_tick = 0, g_has_event = 0;

static void set_error(const char *message) {
  size_t n = strlen(message);
  if (n >= sizeof(g_error)) n = sizeof(g_error) - 1;
  memcpy(g_error, message, n);
  g_error[n] = 0;
  ab_log_raw(g_error, (int32_t)n);
}

/* MicroPython reports failures by longjmp'ing to a nlr buffer; the exception
 * object carries the message. Render it into g_error so the panel can say
 * something useful instead of "it broke". */
static void set_error_from_exception(mp_obj_t exc) {
  mp_print_t print;
  vstr_t vstr;
  vstr_init_print(&vstr, 128, &print);
  mp_obj_print_exception(&print, exc);
  /* The panel is one line, and the LAST traceback line is the one that says
   * what actually went wrong ("ValueError: boom"); the frames above it are
   * context that does not fit. Take the last non-empty line, then append the
   * innermost frame's location if there is room, so the message reads like
   * "ValueError: boom  (main.py, in tick)". */
  const char *buf = vstr_str(&vstr);
  size_t total = vstr_len(&vstr);
  while (total > 0 && (buf[total - 1] == '\n' || buf[total - 1] == '\r')) total--;
  size_t start = total;
  while (start > 0 && buf[start - 1] != '\n') start--;
  size_t n = total - start;
  if (n >= sizeof(g_error)) n = sizeof(g_error) - 1;
  memcpy(g_error, buf + start, n);
  g_error[n] = 0;

  /* find the innermost "File ..." frame for the location suffix */
  if (start > 0) {
    size_t fend = start - 1, fstart;
    while (fend > 0 && (buf[fend - 1] == '\n' || buf[fend - 1] == '\r')) fend--;
    fstart = fend;
    while (fstart > 0 && buf[fstart - 1] != '\n') fstart--;
    while (fstart < fend && (buf[fstart] == ' ' || buf[fstart] == '\t')) fstart++;
    size_t flen = fend - fstart;
    if (flen > 5 && memcmp(buf + fstart, "File ", 5) == 0
        && n + flen + 4 < sizeof(g_error)) {
      g_error[n++] = ' '; g_error[n++] = ' '; g_error[n++] = '(';
      memcpy(g_error + n, buf + fstart + 5, flen - 5);
      n += flen - 5;
      g_error[n++] = ')';
      g_error[n] = 0;
    }
  }
  ab_log_raw(g_error, (int32_t)n);
  vstr_clear(&vstr);
}

/* ------------------------------------------------------ argument helpers -- */

static double arg_f(mp_obj_t o) { return (double)mp_obj_get_float(o); }
static int32_t arg_i(mp_obj_t o) { return (int32_t)mp_obj_get_int(o); }

/* Colours are 0xRRGGBBAA and routinely exceed INT31_MAX, which is a small
 * int in MicroPython -- go through the float/int union so 0xff0000ff does
 * not overflow into a negative. */
static uint32_t arg_rgba(mp_obj_t o) {
  if (mp_obj_is_float(o)) return (uint32_t)(int64_t)mp_obj_get_float(o);
  return (uint32_t)(uint64_t)mp_obj_get_int_truncated(o);
}

static const char *arg_str(mp_obj_t o, size_t *len) {
  return mp_obj_str_get_data(o, len);
}

/* Keys come from C, not from the script source, so the embed port's qstr
 * generator never sees them -- intern at runtime instead of MP_QSTR_*. */
static qstr q(const char *name) { return qstr_from_str(name); }

static mp_map_elem_t *dict_get(mp_map_t *m, const char *key) {
  return mp_map_lookup(m, MP_OBJ_NEW_QSTR(q(key)), MP_MAP_LOOKUP);
}

static void dict_put(mp_obj_t d, const char *key, mp_obj_t value) {
  mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(q(key)), value);
}

/* ------------------------------------------------------------ ab module -- */

/* Wrappers are py_-prefixed: an unprefixed ab_<name> would collide with the
 * SDK's import declaration of the very function being wrapped. */
#define AB_FN_0(name, body) \
  static mp_obj_t py_##name(void) { body } \
  static MP_DEFINE_CONST_FUN_OBJ_0(ab_##name##_obj, py_##name);

AB_FN_0(logical_width,  return mp_obj_new_int(ab_logical_width());)
AB_FN_0(logical_height, return mp_obj_new_int(ab_logical_height());)
AB_FN_0(physical_width, return mp_obj_new_int(ab_physical_width());)
AB_FN_0(physical_height,return mp_obj_new_int(ab_physical_height());)
AB_FN_0(game_width,     return mp_obj_new_int(ab_game_width());)
AB_FN_0(game_height,    return mp_obj_new_int(ab_game_height());)
AB_FN_0(elapsed_ms,     return mp_obj_new_float((mp_float_t)ab_elapsed_ms());)
AB_FN_0(delta_ms,       return mp_obj_new_float((mp_float_t)ab_delta_ms());)
AB_FN_0(scissor_reset,  ab_scissor_reset(); return mp_const_none;)
AB_FN_0(push_transform, return mp_obj_new_int(ab_push_transform());)
AB_FN_0(pop_transform,  return mp_obj_new_int(ab_pop_transform());)
AB_FN_0(reset_transform,ab_reset_transform(); return mp_const_none;)
AB_FN_0(effect_clear,   return mp_obj_new_int(ab_effect_clear());)
AB_FN_0(region_generation, return mp_obj_new_int(ab_region_generation());)
AB_FN_0(region_count,   return mp_obj_new_int(ab_region_count());)

static mp_obj_t py_clear(mp_obj_t rgba) {
  ab_clear(arg_rgba(rgba));
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ab_clear_obj, py_clear);

static mp_obj_t ab_draw_game_fn(size_t n, const mp_obj_t *a) {
  ab_draw_game(arg_f(a[0]), arg_f(a[1]), arg_f(a[2]), arg_f(a[3]),
               n > 4 ? arg_i(a[4]) : 0);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_draw_game_obj, 4, 5, ab_draw_game_fn);

static mp_obj_t ab_draw_game_fit_fn(size_t n, const mp_obj_t *a) {
  ab_draw_game_fit(n > 0 ? arg_i(a[0]) : 0,
                   n > 1 ? arg_f(a[1]) : 0.5, n > 2 ? arg_f(a[2]) : 0.5,
                   n > 3 ? arg_i(a[3]) : 0);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_draw_game_fit_obj, 0, 4, ab_draw_game_fit_fn);

static mp_obj_t ab_fill_rect_fn(size_t n, const mp_obj_t *a) {
  (void)n;
  ab_fill_rect(arg_f(a[0]), arg_f(a[1]), arg_f(a[2]), arg_f(a[3]), arg_rgba(a[4]));
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_fill_rect_obj, 5, 5, ab_fill_rect_fn);

static mp_obj_t ab_triangle_fn(size_t n, const mp_obj_t *a) {
  (void)n;
  ab_triangle(arg_f(a[0]), arg_f(a[1]), arg_f(a[2]), arg_f(a[3]),
              arg_f(a[4]), arg_f(a[5]), arg_rgba(a[6]));
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_triangle_obj, 7, 7, ab_triangle_fn);

/* ab.text() is the built-in 3x5 bitmap font: fine for a debug readout, but
 * ab.draw_text() with a real TTF is what a shipping bezel wants. */
static mp_obj_t ab_text_fn(size_t n, const mp_obj_t *a) {
  (void)n;
  size_t len = 0;
  const char *s = arg_str(a[0], &len);
  ab_text_raw(s, (int32_t)len, arg_f(a[1]), arg_f(a[2]), arg_f(a[3]), arg_rgba(a[4]));
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_text_obj, 5, 5, ab_text_fn);

static mp_obj_t ab_scissor_fn(size_t n, const mp_obj_t *a) {
  (void)n;
  ab_scissor(arg_f(a[0]), arg_f(a[1]), arg_f(a[2]), arg_f(a[3]));
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_scissor_obj, 4, 4, ab_scissor_fn);

static mp_obj_t ab_translate_fn(mp_obj_t x, mp_obj_t y) {
  ab_translate(arg_f(x), arg_f(y));
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ab_translate_obj, ab_translate_fn);

static mp_obj_t ab_scale_fn(size_t n, const mp_obj_t *a) {
  double x = arg_f(a[0]);
  ab_scale(x, n > 1 ? arg_f(a[1]) : x);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_scale_obj, 1, 2, ab_scale_fn);

static mp_obj_t ab_rotate_fn(mp_obj_t r) { ab_rotate(arg_f(r)); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_1(ab_rotate_obj, ab_rotate_fn);

/* skew(x, y=0) -- shear as tangents; skew(math.pi/6) leans 30 degrees. */
static mp_obj_t ab_skew_fn(size_t n, const mp_obj_t *a) {
  ab_skew(arg_f(a[0]), n > 1 ? arg_f(a[1]) : 0);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_skew_obj, 1, 2, ab_skew_fn);

static mp_obj_t ab_transform2d_fn(size_t n, const mp_obj_t *a) {
  (void)n;
  ab_transform2d(arg_f(a[0]), arg_f(a[1]), arg_f(a[2]),
                 arg_f(a[3]), arg_f(a[4]), arg_f(a[5]));
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_transform2d_obj, 6, 6, ab_transform2d_fn);

/* quad([(x,y) or {'x':,'y':}] * 4, texture=0, rgba=white) -- perspective
 * correct textured quad, corners clockwise from top-left. */
/* --- offscreen surfaces -------------------------------------------------- */
static mp_obj_t ab_surface_create_fn(mp_obj_t w, mp_obj_t h) {
  return mp_obj_new_int(ab_surface_create(arg_i(w), arg_i(h)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(ab_surface_create_obj, ab_surface_create_fn);

static mp_obj_t ab_surface_target_fn(mp_obj_t handle) {
  return mp_obj_new_int(ab_surface_target(arg_i(handle)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ab_surface_target_obj, ab_surface_target_fn);

static mp_obj_t ab_surface_end_fn(void) { return mp_obj_new_int(ab_surface_end()); }
static MP_DEFINE_CONST_FUN_OBJ_0(ab_surface_end_obj, ab_surface_end_fn);

static mp_obj_t ab_surface_filter_fn(mp_obj_t src, mp_obj_t dst, mp_obj_t shader) {
  size_t n = 0;
  const char *s = arg_str(shader, &n);
  return mp_obj_new_int(ab_surface_filter_raw(arg_i(src), arg_i(dst), s, (int32_t)n));
}
static MP_DEFINE_CONST_FUN_OBJ_3(ab_surface_filter_obj, ab_surface_filter_fn);

static mp_obj_t ab_surface_preset_fn(mp_obj_t src, mp_obj_t dst, mp_obj_t preset) {
  size_t n = 0;
  const char *s = arg_str(preset, &n);
  return mp_obj_new_int(ab_surface_preset_raw(arg_i(src), arg_i(dst), s, (int32_t)n));
}
static MP_DEFINE_CONST_FUN_OBJ_3(ab_surface_preset_obj, ab_surface_preset_fn);

static mp_obj_t ab_quad_fn(size_t n, const mp_obj_t *a) {
  size_t count = 0;
  mp_obj_t *items = NULL;
  mp_obj_get_array(a[0], &count, &items);
  if (count != 4) mp_raise_ValueError(MP_ERROR_TEXT("quad: need exactly 4 corners"));
  ab_point pts[4];
  for (size_t i = 0; i < 4; i++) {
    mp_obj_t c = items[i];
    if (mp_obj_is_type(c, &mp_type_dict)) {
      mp_map_t *m = mp_obj_dict_get_map(c);
      mp_map_elem_t *e;
      pts[i].x = (e = dict_get(m, "x")) ? arg_f(e->value) : 0;
      pts[i].y = (e = dict_get(m, "y")) ? arg_f(e->value) : 0;
    } else {
      size_t fn = 0;
      mp_obj_t *f = NULL;
      mp_obj_get_array(c, &fn, &f);
      pts[i].x = fn > 0 ? arg_f(f[0]) : 0;
      pts[i].y = fn > 1 ? arg_f(f[1]) : 0;
    }
  }
  return mp_obj_new_int(ab_quad(pts, n > 1 ? arg_i(a[1]) : 0,
    n > 2 ? arg_rgba(a[2]) : 0xffffffffu));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_quad_obj, 1, 3, ab_quad_fn);

/* mesh(vertices, texture=0): vertices is a sequence of dicts or 5-tuples.
 * Dicts read like pygame-adjacent code; tuples are the fast path. */
static mp_obj_t ab_mesh_fn(size_t n, const mp_obj_t *a) {
  size_t count = 0;
  mp_obj_t *items = NULL;
  mp_obj_get_array(a[0], &count, &items);
  int32_t texture = n > 1 ? arg_i(a[1]) : 0;
  if (count == 0) return mp_obj_new_int(0);

  ab_vertex *verts = (ab_vertex *)m_malloc(sizeof(ab_vertex) * count);
  for (size_t i = 0; i < count; i++) {
    mp_obj_t v = items[i];
    double x = 0, y = 0, u = 0, vv = 0;
    uint32_t rgba = 0xffffffffu;
    if (mp_obj_is_type(v, &mp_type_dict)) {
      mp_map_t *m = mp_obj_dict_get_map(v);
      mp_map_elem_t *e;
      if ((e = dict_get(m, "x"))) x = arg_f(e->value);
      if ((e = dict_get(m, "y"))) y = arg_f(e->value);
      if ((e = dict_get(m, "u"))) u = arg_f(e->value);
      if ((e = dict_get(m, "v"))) vv = arg_f(e->value);
      if ((e = dict_get(m, "rgba"))) rgba = arg_rgba(e->value);
    } else {
      size_t fn = 0;
      mp_obj_t *f = NULL;
      mp_obj_get_array(v, &fn, &f);
      if (fn > 0) x = arg_f(f[0]);
      if (fn > 1) y = arg_f(f[1]);
      if (fn > 2) rgba = arg_rgba(f[2]);
      if (fn > 3) u = arg_f(f[3]);
      if (fn > 4) vv = arg_f(f[4]);
    }
    verts[i].x = (float)x; verts[i].y = (float)y;
    verts[i].u = (float)u; verts[i].v = (float)vv;
    verts[i].rgba = rgba;
  }
  int32_t emitted = ab_mesh(verts, (int32_t)count, texture);
  m_free(verts);
  return mp_obj_new_int(emitted);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_mesh_obj, 1, 2, ab_mesh_fn);

/* --- textures ------------------------------------------------------------ */

static mp_obj_t ab_texture_create_fn(mp_obj_t pixels, mp_obj_t w, mp_obj_t h) {
  size_t n = 0;
  const char *bytes = arg_str(pixels, &n);
  int32_t iw = arg_i(w), ih = arg_i(h);
  if (iw <= 0 || ih <= 0 || n < (size_t)iw * (size_t)ih * 4) {
    mp_raise_ValueError(MP_ERROR_TEXT("texture_create: need w*h*4 bytes of RGBA"));
  }
  return mp_obj_new_int(ab_texture_create_rgba(bytes, iw, ih));
}
static MP_DEFINE_CONST_FUN_OBJ_3(ab_texture_create_obj, ab_texture_create_fn);

static mp_obj_t ab_texture_destroy_fn(mp_obj_t handle) {
  return mp_obj_new_int(ab_texture_destroy(arg_i(handle)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ab_texture_destroy_obj, ab_texture_destroy_fn);

static mp_obj_t ab_draw_texture_fn(size_t n, const mp_obj_t *a) {
  (void)n;
  return mp_obj_new_int(ab_draw_texture(arg_i(a[0]),
    arg_f(a[1]), arg_f(a[2]), arg_f(a[3]), arg_f(a[4])));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_draw_texture_obj, 5, 5, ab_draw_texture_fn);

static mp_obj_t ab_draw_texture_rect_fn(size_t n, const mp_obj_t *a) {
  (void)n;
  return mp_obj_new_int(ab_draw_texture_rect(arg_i(a[0]),
    arg_f(a[1]), arg_f(a[2]), arg_f(a[3]), arg_f(a[4]),
    arg_i(a[5]), arg_i(a[6]), arg_i(a[7]), arg_i(a[8])));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_draw_texture_rect_obj, 9, 9, ab_draw_texture_rect_fn);

/* --- images (shared batteries) ------------------------------------------- */

static mp_obj_t make_image(int32_t tex, int w, int h) {
  mp_obj_t d = mp_obj_new_dict(3);
  dict_put(d, "texture", mp_obj_new_int(tex));
  dict_put(d, "width", mp_obj_new_int(w));
  dict_put(d, "height", mp_obj_new_int(h));
  return d;
}

static mp_obj_t ab_image_fn(mp_obj_t name) {
  size_t n = 0;
  const char *s = arg_str(name, &n);
  int32_t tex = 0; int w = 0, h = 0; const char *err = NULL;
  if (!ab_bat_image_from_asset(s, (int)n, &tex, &w, &h, &err)) {
    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("image: %s"), err ? err : "decode failed");
  }
  return make_image(tex, w, h);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ab_image_obj, ab_image_fn);

static mp_obj_t ab_image_data_fn(mp_obj_t data) {
  size_t n = 0;
  const char *s = arg_str(data, &n);
  int32_t tex = 0; int w = 0, h = 0; const char *err = NULL;
  if (!ab_bat_image_from_memory((const unsigned char *)s, (int)n, &tex, &w, &h, &err)) {
    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("image_data: %s"), err ? err : "decode failed");
  }
  return make_image(tex, w, h);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ab_image_data_obj, ab_image_data_fn);

/* --- TrueType text (shared batteries) ------------------------------------ */

static mp_obj_t ab_font_fn(mp_obj_t name) {
  size_t n = 0;
  const char *s = arg_str(name, &n);
  const char *err = NULL;
  int32_t handle = ab_bat_font_load(s, (int)n, &err);
  if (handle <= 0) {
    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("font: %s"), err ? err : "load failed");
  }
  return mp_obj_new_int(handle);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ab_font_obj, ab_font_fn);

static mp_obj_t ab_draw_text_fn(size_t n, const mp_obj_t *a) {
  (void)n;
  size_t len = 0;
  const char *s = arg_str(a[1], &len);
  const char *err = NULL;
  double advance = ab_bat_font_print(arg_i(a[0]), s, (int)len,
    arg_f(a[2]), arg_f(a[3]), arg_i(a[4]), arg_rgba(a[5]), &err);
  if (err) mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("draw_text: %s"), err);
  return mp_obj_new_float((mp_float_t)advance);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_draw_text_obj, 6, 6, ab_draw_text_fn);

static mp_obj_t ab_measure_fn(mp_obj_t font, mp_obj_t text, mp_obj_t px) {
  size_t len = 0;
  const char *s = arg_str(text, &len);
  const char *err = NULL;
  double w = ab_bat_font_measure(arg_i(font), s, (int)len, arg_i(px), &err);
  if (err) mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("measure: %s"), err);
  return mp_obj_new_float((mp_float_t)w);
}
static MP_DEFINE_CONST_FUN_OBJ_3(ab_measure_obj, ab_measure_fn);

static mp_obj_t ab_font_metrics_fn(mp_obj_t font, mp_obj_t px) {
  double asc = 0, desc = 0, lh = 0; const char *err = NULL;
  if (!ab_bat_font_metrics(arg_i(font), arg_i(px), &asc, &desc, &lh, &err)) {
    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("font_metrics: %s"), err ? err : "failed");
  }
  mp_obj_t d = mp_obj_new_dict(3);
  dict_put(d, "ascent", mp_obj_new_float((mp_float_t)asc));
  dict_put(d, "descent", mp_obj_new_float((mp_float_t)desc));
  dict_put(d, "line_height", mp_obj_new_float((mp_float_t)lh));
  return d;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ab_font_metrics_obj, ab_font_metrics_fn);

/* --- shader effects ------------------------------------------------------ */

static mp_obj_t ab_effect_set_fn(mp_obj_t src) {
  size_t n = 0;
  const char *s = arg_str(src, &n);
  return mp_obj_new_bool(ab_effect_set_raw(s, (int32_t)n) != 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ab_effect_set_obj, ab_effect_set_fn);

/* --- the machine --------------------------------------------------------- */

static mp_obj_t ab_game_pixel_fn(mp_obj_t x, mp_obj_t y) {
  return mp_obj_new_int_from_uint(ab_game_pixel(arg_i(x), arg_i(y)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(ab_game_pixel_obj, ab_game_pixel_fn);

static mp_obj_t ab_input_fn(size_t n, const mp_obj_t *a) {
  return mp_obj_new_int(ab_input_state(
    n > 0 ? arg_i(a[0]) : 0, n > 1 ? arg_i(a[1]) : 1,
    n > 2 ? arg_i(a[2]) : 0, n > 3 ? arg_i(a[3]) : 0));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_input_obj, 1, 4, ab_input_fn);

static mp_obj_t ab_log_fn(mp_obj_t msg) {
  size_t n = 0;
  const char *s = arg_str(msg, &n);
  ab_log_raw(s, (int32_t)n);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ab_log_obj, ab_log_fn);

static mp_obj_t ab_region_fn(mp_obj_t name) {
  size_t n = 0;
  const char *s = arg_str(name, &n);
  int32_t id = ab_region_find_raw(s, (int32_t)n);
  return id < 0 ? mp_const_none : mp_obj_new_int(id);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ab_region_obj, ab_region_fn);

static mp_obj_t ab_region_find_id_fn(mp_obj_t id) {
  int32_t r = ab_region_find_id(arg_i(id));
  return r < 0 ? mp_const_none : mp_obj_new_int(r);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ab_region_find_id_obj, ab_region_find_id_fn);

#define AB_FN_1_INT(name, call) \
  static mp_obj_t py_##name##_fn(mp_obj_t a) { return mp_obj_new_int(call(arg_i(a))); } \
  static MP_DEFINE_CONST_FUN_OBJ_1(ab_##name##_obj, py_##name##_fn);

AB_FN_1_INT(region_size, ab_region_size)
AB_FN_1_INT(region_flags, ab_region_flags)
AB_FN_1_INT(region_offset, ab_region_offset)

static mp_obj_t ab_read_u8_fn(mp_obj_t region, mp_obj_t offset) {
  return mp_obj_new_int(ab_region_read_u8(arg_i(region), arg_i(offset)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(ab_read_u8_obj, ab_read_u8_fn);

static mp_obj_t ab_write_u8_fn(mp_obj_t region, mp_obj_t offset, mp_obj_t value) {
  return mp_obj_new_int(ab_region_write_u8(arg_i(region), arg_i(offset), arg_i(value)));
}
static MP_DEFINE_CONST_FUN_OBJ_3(ab_write_u8_obj, ab_write_u8_fn);

/* read(region, offset, length) -> bytes. The per-byte import is fine for a
 * few reads; a bytes object is what table-driven decoders want. */
static mp_obj_t ab_read_fn(mp_obj_t region, mp_obj_t offset, mp_obj_t length) {
  int32_t id = arg_i(region), off = arg_i(offset), len = arg_i(length);
  if (len <= 0 || len > (1 << 20)) mp_raise_ValueError(MP_ERROR_TEXT("read: bad length"));
  vstr_t vstr;
  vstr_init_len(&vstr, (size_t)len);
  for (int32_t i = 0; i < len; i++) {
    int32_t v = ab_region_read_u8(id, off + i);
    vstr.buf[i] = (char)(v < 0 ? 0 : v);
  }
  return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_3(ab_read_obj, ab_read_fn);

static mp_obj_t read_uint_n(size_t n, const mp_obj_t *a, int bytes) {
  return mp_obj_new_int_from_uint(ab_bat_read_uint(
    arg_i(a[0]), arg_i(a[1]), bytes, n > 2 && mp_obj_is_true(a[2])));
}
static mp_obj_t ab_read_u16_fn(size_t n, const mp_obj_t *a) { return read_uint_n(n, a, 2); }
static mp_obj_t ab_read_u24_fn(size_t n, const mp_obj_t *a) { return read_uint_n(n, a, 3); }
static mp_obj_t ab_read_u32_fn(size_t n, const mp_obj_t *a) { return read_uint_n(n, a, 4); }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_read_u16_obj, 2, 3, ab_read_u16_fn);
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_read_u24_obj, 2, 3, ab_read_u24_fn);
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_read_u32_obj, 2, 3, ab_read_u32_fn);

static mp_obj_t ab_asset_fn(mp_obj_t name) {
  size_t n = 0;
  const char *s = arg_str(name, &n);
  const char *err = NULL;
  int len = 0;
  unsigned char *bytes = ab_bat_asset_slurp(s, (int)n, &len, &err);
  if (!bytes) return mp_const_none;
  mp_obj_t out = mp_obj_new_bytes(bytes, (size_t)len);
  free(bytes);
  return out;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ab_asset_obj, ab_asset_fn);

/* --- config -------------------------------------------------------------- */

static mp_obj_t ab_config_bool_fn(mp_obj_t key) {
  size_t n = 0;
  const char *s = arg_str(key, &n);
  return mp_obj_new_bool(ab_config_bool_raw(s, (int32_t)n) != 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ab_config_bool_obj, ab_config_bool_fn);

static mp_obj_t ab_config_number_fn(mp_obj_t key) {
  size_t n = 0;
  const char *s = arg_str(key, &n);
  return mp_obj_new_float((mp_float_t)ab_config_number_raw(s, (int32_t)n));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ab_config_number_obj, ab_config_number_fn);

static mp_obj_t ab_config_string_fn(mp_obj_t key) {
  size_t n = 0;
  const char *s = arg_str(key, &n);
  int32_t len = ab_config_string_length_raw(s, (int32_t)n);
  if (len < 0) return mp_const_none;
  vstr_t vstr;
  vstr_init_len(&vstr, (size_t)len);
  int32_t got = ab_config_string_read_raw(s, (int32_t)n, vstr.buf, len);
  if (got < 0) got = 0;
  vstr.len = (size_t)got;
  return mp_obj_new_str_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ab_config_string_obj, ab_config_string_fn);

static mp_obj_t ab_rgb_fn(size_t n, const mp_obj_t *a) {
  return mp_obj_new_int_from_uint(ab_bat_rgba(
    arg_i(a[0]), arg_i(a[1]), arg_i(a[2]), n > 3 ? arg_i(a[3]) : 255));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ab_rgb_obj, 3, 4, ab_rgb_fn);

/* --- constant tables -----------------------------------------------------
 * Scripts should never hard-code ABI numbers. These mirror the C SDK's
 * AB_* defines and the other runtimes' tables exactly.
 */
static mp_obj_t make_consts(const char *const *names, const int *values, size_t n) {
  mp_obj_t d = mp_obj_new_dict(n);
  for (size_t i = 0; i < n; i++) dict_put(d, names[i], mp_obj_new_int(values[i]));
  return d;
}

static const char *const EVENT_NAMES[] = { "RESET", "STATE_LOADED", "REWIND_JUMP",
  "CONFIG_CHANGED", "DISPLAY_CHANGED", "ASSETS_RELOADED", "REGIONS_CHANGED" };
static const int EVENT_VALUES[] = { 1, 2, 3, 4, 5, 6, 7 };
static const char *const FIT_NAMES[] = { "CONTAIN", "COVER", "STRETCH", "INTEGER" };
static const int FIT_VALUES[] = { 0, 1, 2, 3 };
static const char *const SAMPLE_NAMES[] = { "NEAREST", "LINEAR" };
static const int SAMPLE_VALUES[] = { 0, 1 };
static const char *const DEVICE_NAMES[] = { "JOYPAD", "ANALOG" };
static const int DEVICE_VALUES[] = { 1, 5 };
static const char *const BTN_NAMES[] = { "B", "Y", "SELECT", "START", "UP", "DOWN",
  "LEFT", "RIGHT", "A", "X", "L", "R", "MASK" };
static const int BTN_VALUES[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 256 };

/* The module dict is built at boot so the constant dicts can be real dicts
 * (a script may want ab.BTN['START'] or ab.BTN.get(name)). */
static mp_obj_t g_ab_module_dict = MP_OBJ_NULL;

static void ab_module_add(mp_obj_t d, const char *name, mp_obj_t value) {
  dict_put(d, name, value);
}

static mp_obj_t build_ab_module(void) {
  mp_obj_t d = mp_obj_new_dict(64);
#define ADD(n, o) ab_module_add(d, n, MP_OBJ_FROM_PTR(&o))
  ADD("clear", ab_clear_obj);
  ADD("draw_game", ab_draw_game_obj);
  ADD("draw_game_fit", ab_draw_game_fit_obj);
  ADD("fill_rect", ab_fill_rect_obj);
  ADD("triangle", ab_triangle_obj);
  ADD("text", ab_text_obj);
  ADD("scissor", ab_scissor_obj);
  ADD("scissor_reset", ab_scissor_reset_obj);
  ADD("push_transform", ab_push_transform_obj);
  ADD("pop_transform", ab_pop_transform_obj);
  ADD("reset_transform", ab_reset_transform_obj);
  ADD("translate", ab_translate_obj);
  ADD("scale", ab_scale_obj);
  ADD("rotate", ab_rotate_obj);
  ADD("skew", ab_skew_obj);
  ADD("transform2d", ab_transform2d_obj);
  ADD("quad", ab_quad_obj);
  ADD("surface_create", ab_surface_create_obj);
  ADD("surface_target", ab_surface_target_obj);
  ADD("surface_end", ab_surface_end_obj);
  ADD("surface_filter", ab_surface_filter_obj);
  ADD("surface_preset", ab_surface_preset_obj);
  ADD("mesh", ab_mesh_obj);
  ADD("texture_create", ab_texture_create_obj);
  ADD("texture_destroy", ab_texture_destroy_obj);
  ADD("draw_texture", ab_draw_texture_obj);
  ADD("draw_texture_rect", ab_draw_texture_rect_obj);
  ADD("image", ab_image_obj);
  ADD("image_data", ab_image_data_obj);
  ADD("font", ab_font_obj);
  ADD("draw_text", ab_draw_text_obj);
  ADD("measure", ab_measure_obj);
  ADD("font_metrics", ab_font_metrics_obj);
  ADD("effect_set", ab_effect_set_obj);
  ADD("effect_clear", ab_effect_clear_obj);
  ADD("game_width", ab_game_width_obj);
  ADD("game_height", ab_game_height_obj);
  ADD("game_pixel", ab_game_pixel_obj);
  ADD("logical_width", ab_logical_width_obj);
  ADD("logical_height", ab_logical_height_obj);
  ADD("physical_width", ab_physical_width_obj);
  ADD("physical_height", ab_physical_height_obj);
  ADD("elapsed_ms", ab_elapsed_ms_obj);
  ADD("delta_ms", ab_delta_ms_obj);
  ADD("input", ab_input_obj);
  ADD("log", ab_log_obj);
  ADD("region", ab_region_obj);
  ADD("region_find_id", ab_region_find_id_obj);
  ADD("region_size", ab_region_size_obj);
  ADD("region_flags", ab_region_flags_obj);
  ADD("region_offset", ab_region_offset_obj);
  ADD("region_generation", ab_region_generation_obj);
  ADD("region_count", ab_region_count_obj);
  ADD("read_u8", ab_read_u8_obj);
  ADD("write_u8", ab_write_u8_obj);
  ADD("read", ab_read_obj);
  ADD("read_u16", ab_read_u16_obj);
  ADD("read_u24", ab_read_u24_obj);
  ADD("read_u32", ab_read_u32_obj);
  ADD("asset", ab_asset_obj);
  ADD("config_bool", ab_config_bool_obj);
  ADD("config_number", ab_config_number_obj);
  ADD("config_string", ab_config_string_obj);
  ADD("rgb", ab_rgb_obj);
#undef ADD
  /* ab.GAME: pass as a texture handle to sample the LIVE GAME FRAME, e.g.
   * ab.quad(corners, ab.GAME) maps the running game onto a tilted plane. */
  ab_module_add(d, "GAME", mp_obj_new_int(-1));
  ab_module_add(d, "EVENT", make_consts(EVENT_NAMES, EVENT_VALUES, 7));
  ab_module_add(d, "FIT", make_consts(FIT_NAMES, FIT_VALUES, 4));
  ab_module_add(d, "SAMPLE", make_consts(SAMPLE_NAMES, SAMPLE_VALUES, 2));
  ab_module_add(d, "DEVICE", make_consts(DEVICE_NAMES, DEVICE_VALUES, 2));
  ab_module_add(d, "BTN", make_consts(BTN_NAMES, BTN_VALUES, 13));
  return d;
}

/* -------------------------------------------------------- script loading -- */

/* The script's globals live here so tick() can find its own functions and
 * whatever module-level state it set up. */
static mp_obj_dict_t *g_script_globals = NULL;

static mp_obj_t script_fn(const char *name) {
  if (!g_script_globals) return MP_OBJ_NULL;
  mp_map_elem_t *e = dict_get(&g_script_globals->map, name);
  return e ? e->value : MP_OBJ_NULL;
}

static int run_source(const char *src, size_t len, const char *name) {
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mp_lexer_t *lex = mp_lexer_new_from_str_len(qstr_from_str(name), src, len, 0);
    qstr source_name = lex->source_name;
    mp_parse_tree_t parse_tree = mp_parse(lex, MP_PARSE_FILE_INPUT);
    mp_obj_t module_fun = mp_compile(&parse_tree, source_name, false);
    mp_call_function_0(module_fun);
    nlr_pop();
    return 1;
  }
  set_error_from_exception(MP_OBJ_FROM_PTR(nlr.ret_val));
  return 0;
}

static int call_script(const char *name, mp_obj_t arg, int has_arg) {
  mp_obj_t fn = script_fn(name);
  if (fn == MP_OBJ_NULL || !mp_obj_is_callable(fn)) return 1;
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    if (has_arg) mp_call_function_1(fn, arg);
    else mp_call_function_0(fn);
    nlr_pop();
    return 1;
  }
  set_error_from_exception(MP_OBJ_FROM_PTR(nlr.ret_val));
  return 0;
}

static int load_script(void) {
  static const char *CANDIDATES[] = { "main.py", "assets/main.py" };
  const char *name = NULL;
  int32_t size = -1;
  for (unsigned i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++) {
    size = ab_asset_size(CANDIDATES[i]);
    if (size >= 0) { name = CANDIDATES[i]; break; }
  }
  if (!name) { set_error("python runtime: no main.py (or assets/main.py) in the package"); return 0; }

  const char *err = NULL;
  int len = 0;
  unsigned char *source = ab_bat_asset_slurp(name, (int)strlen(name), &len, &err);
  if (!source) { set_error(err ? err : "python runtime: asset read failed"); return 0; }

  int ok = run_source((const char *)source, (size_t)len, name);
  free(source);
  if (!ok) return 0;

  g_has_tick = script_fn("tick") != MP_OBJ_NULL;
  g_has_event = script_fn("event") != MP_OBJ_NULL;
  if (!g_has_tick) {
    set_error("python runtime: main.py must define a function tick(frame)");
    return 0;
  }
  if (!call_script("init", mp_const_none, 0)) return 0;
  g_error[0] = 0;
  return 1;
}

static void boot(void) {
  if (g_booted) {
    gc_sweep_all();
    mp_deinit();
    g_booted = 0;
  }
  g_has_tick = g_has_event = 0;
  g_script_globals = NULL;

  mp_stack_ctrl_init();
  /* Emscripten's default stack is 64KB; leave the interpreter a margin so a
   * deep expression raises RuntimeError instead of smashing the stack. */
  mp_stack_set_limit(40 * 1024);
  gc_init(g_heap, g_heap + sizeof(g_heap));
  mp_init();
  g_booted = 1;

  /* Register `ab` so `import ab` finds it, and inject it into the script's
   * globals so a bezel can just use `ab.clear(...)` with no import line --
   * the same convenience the Lua runtime's global `ab` table provides. */
  g_ab_module_dict = build_ab_module();
  mp_obj_t ab_mod = mp_obj_new_module(q("ab"));
  mp_obj_dict_t *mod_globals = mp_obj_module_get_globals(ab_mod);
  mp_map_t *src = mp_obj_dict_get_map(g_ab_module_dict);
  for (size_t i = 0; i < src->alloc; i++) {
    if (mp_map_slot_is_filled(src, i)) {
      mp_obj_dict_store(MP_OBJ_FROM_PTR(mod_globals), src->table[i].key, src->table[i].value);
    }
  }
  mp_store_global(q("ab"), ab_mod);

  /* Platform redraw profiles: registers the nes/gb/md/snes/msx/pce modules
   * and injects them as globals, the same convenience `ab` gets. */
  ab_profiles_py_register();

  g_script_globals = mp_globals_get();
  load_script();
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
  if (g_error[0] || !g_booted || !g_has_tick) {
    ab_clear(0x201018ffu);
    ab_draw_game_fit(0, 0.5, 0.35, 0);
    ab_text("python bezel error:", 40, 40, 30, 0xff8080ffu);
    ab_text(g_error[0] ? g_error : "script not loaded", 40, 84, 24, 0xffd0d0ffu);
    ab_text("fix main.py and reload -- the runtime re-reads it", 40, 124, 22, 0x9098b0ffu);
    return;
  }
  call_script("tick", mp_obj_new_int_from_ull(frame), 1);
}

AB_EXPORT("ab_event")
void ab_event(int32_t kind, uint32_t data) {
  (void)data;                          /* reserved by the ABI, unused today */
  /* ASSETS_RELOADED (6): the package archive changed under us -- re-read
   * main.py. This is the whole iteration story: edit, reload, replay. */
  if (kind == 6) { boot(); return; }
  if (!g_booted || !g_has_event || g_error[0]) return;
  call_script("event", mp_obj_new_int(kind), 1);
}

AB_EXPORT("ab_shutdown")
void ab_shutdown(void) {
  if (g_booted) { mp_deinit(); g_booted = 0; }
  ab_bat_shutdown();
  ab_profiles_py_shutdown();
}

/* --------------------------------------------------- MicroPython plumbing -- */

/* MicroPython's import machinery expects a filesystem. A bezel has none --
 * main.py arrives from the package as a string -- so these two hooks exist
 * only to keep them from becoming `env` imports the host cannot satisfy.
 * A script that tries `import foo` gets a clean ImportError instead of a
 * module that fails to instantiate. */
mp_import_stat_t mp_import_stat(const char *path) {
  (void)path;
  return MP_IMPORT_STAT_NO_EXIST;
}

mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
  mp_raise_msg(&mp_type_ImportError, MP_ERROR_TEXT("no filesystem in an active bezel"));
}

/* gc_collect() and nlr_jump_fail() come from the embed port's embed_util.c.
 *
 * mp_hal_stdout_tx_strn_cooked() does NOT: the port's version calls printf,
 * which is precisely what drags fd_write/fd_seek/fd_close WASI imports into
 * a wasm that must import only ab_host. build.sh excludes port/mphalport.c
 * and this definition takes its place -- a script's print() is how a bezel
 * author debugs, so it belongs in the host log anyway. */
void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) {
  ab_log_raw(str, (int32_t)len);
}
void mp_hal_stdout_tx_strn(const char *str, size_t len) {
  ab_log_raw(str, (int32_t)len);
}
mp_uint_t mp_hal_ticks_ms(void) { return (mp_uint_t)ab_elapsed_ms(); }
