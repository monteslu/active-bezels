/*
 * runtime.c -- the prebuilt Ruby (mruby) Active Bezel runtime.
 *
 * This wasm IS the package entry point. A Ruby bezel ships this file's build
 * as `main.wasm` plus its own `main.rb` in the archive; iterating on the
 * bezel is edit main.rb + repack, with no compiler in the loop.
 *
 * Script contract (all top-level methods, all optional except tick):
 *   def init()        -- once, after the script loads
 *   def tick(frame)   -- once per emulated frame; draw the whole scene
 *   def event(kind)   -- host lifecycle events (AB_EVENT numbers)
 *
 * The whole ab_* import surface is exposed as module functions on `AB`.
 * Errors never kill the session: they are logged, drawn on screen, and the
 * script is re-read on the next ASSETS_RELOADED event so a fix is one repack
 * away.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/compile.h>
#include <mruby/error.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby/variable.h>

#include "../../sdk/active_bezel.h"

#include "../common/ab_batteries.h"

/* The platform redraw profiles (`NES`/`GB`/`MD`/`SNES`/`MSX`/`PCE`).
 * All logic lives in runtimes/common/ab_profiles.c, shared with the other
 * three runtimes; ab_profiles_rb.c is marshaling only. */
void ab_profiles_rb_define(mrb_state *m);
void ab_profiles_rb_shutdown(void);

/* ---------------------------------------------------------------- state -- */

static mrb_state *mrb = NULL;
static char g_error[512];
static int g_has_tick = 0, g_has_event = 0;

static void set_error(const char *message) {
  size_t n = strlen(message);
  if (n >= sizeof(g_error)) n = sizeof(g_error) - 1;
  memcpy(g_error, message, n);
  g_error[n] = 0;
  ab_log_raw(g_error, (int32_t)n);
}

/* Drain a pending mruby exception into the error panel. mruby signals errors
 * by leaving mrb->exc set rather than by return value, so EVERY entry point
 * that runs Ruby has to check it -- an unchecked exc poisons the next call
 * into the VM. Returns 1 if an exception was consumed. */
static int guard(const char *what) {
  if (!mrb || !mrb->exc) return 0;
  mrb_value exc = mrb_obj_value(mrb->exc);
  mrb->exc = NULL;                       /* clear BEFORE calling back into the
                                          * VM: mrb_funcall on a state with a
                                          * live exc re-raises immediately. */
  char line[sizeof(g_error)];
  mrb_value msg = mrb_funcall(mrb, exc, "message", 0);
  const char *text = (!mrb->exc && mrb_string_p(msg)) ? RSTRING_PTR(msg) : "(unprintable)";
  mrb_value klass = mrb_obj_value(mrb_obj_class(mrb, exc));
  mrb_value kname = mrb_funcall(mrb, klass, "to_s", 0);
  const char *kn = (!mrb->exc && mrb_string_p(kname)) ? RSTRING_PTR(kname) : "Exception";
  snprintf(line, sizeof line, "%s: %s (%s)", what, text, kn);
  mrb->exc = NULL;                       /* message/to_s may have raised too */
  set_error(line);
  return 1;
}

/* ------------------------------------------------- mruby AB bindings ------ */

/* Colours are 0xRRGGBBAA and routinely exceed mrb_int's signed positive range
 * in the literal form scripts write (0xff0000ff). Accept a Float or Integer
 * and wrap into uint32 so 0xff0000ff and -16776961 both mean red. */
static uint32_t to_rgba(mrb_state *m, mrb_value v) {
  if (mrb_float_p(v)) return (uint32_t)(int64_t)mrb_float(v);
  return (uint32_t)(int64_t)mrb_as_int(m, v);
}

static mrb_value ab_m_clear(mrb_state *m, mrb_value self) {
  mrb_value rgba;
  mrb_get_args(m, "o", &rgba);
  ab_clear(to_rgba(m, rgba));
  (void)self;
  return mrb_nil_value();
}

static mrb_value ab_m_draw_game(mrb_state *m, mrb_value self) {
  mrb_float x, y, w, h;
  mrb_int sample = 0;
  mrb_get_args(m, "ffff|i", &x, &y, &w, &h, &sample);
  ab_draw_game(x, y, w, h, (int32_t)sample);
  (void)self;
  return mrb_nil_value();
}

static mrb_value ab_m_draw_game_fit(mrb_state *m, mrb_value self) {
  mrb_int fit = 0, sample = 0;
  mrb_float ax = 0.5, ay = 0.5;
  mrb_get_args(m, "|iffi", &fit, &ax, &ay, &sample);
  ab_draw_game_fit((int32_t)fit, ax, ay, (int32_t)sample);
  (void)self;
  return mrb_nil_value();
}

static mrb_value ab_m_fill_rect(mrb_state *m, mrb_value self) {
  mrb_float x, y, w, h;
  mrb_value rgba;
  mrb_get_args(m, "ffffo", &x, &y, &w, &h, &rgba);
  ab_fill_rect(x, y, w, h, to_rgba(m, rgba));
  (void)self;
  return mrb_nil_value();
}

static mrb_value ab_m_triangle(mrb_state *m, mrb_value self) {
  mrb_float x1, y1, x2, y2, x3, y3;
  mrb_value rgba;
  mrb_get_args(m, "ffffffo", &x1, &y1, &x2, &y2, &x3, &y3, &rgba);
  ab_triangle(x1, y1, x2, y2, x3, y3, to_rgba(m, rgba));
  (void)self;
  return mrb_nil_value();
}

static mrb_value ab_m_text(mrb_state *m, mrb_value self) {
  const char *s; mrb_int slen;
  mrb_float x, y, size;
  mrb_value rgba;
  mrb_get_args(m, "sfffo", &s, &slen, &x, &y, &size, &rgba);
  ab_text_raw(s, (int32_t)slen, x, y, size, to_rgba(m, rgba));
  (void)self;
  return mrb_nil_value();
}

static mrb_value ab_m_scissor(mrb_state *m, mrb_value self) {
  mrb_float x, y, w, h;
  mrb_get_args(m, "ffff", &x, &y, &w, &h);
  ab_scissor(x, y, w, h);
  (void)self;
  return mrb_nil_value();
}

static mrb_value ab_m_scissor_reset(mrb_state *m, mrb_value self) {
  (void)m; (void)self;
  ab_scissor_reset();
  return mrb_nil_value();
}

static mrb_value ab_m_push_transform(mrb_state *m, mrb_value self) {
  (void)self;
  return mrb_int_value(m, ab_push_transform());
}

static mrb_value ab_m_pop_transform(mrb_state *m, mrb_value self) {
  (void)self;
  return mrb_int_value(m, ab_pop_transform());
}

static mrb_value ab_m_reset_transform(mrb_state *m, mrb_value self) {
  (void)m; (void)self;
  ab_reset_transform();
  return mrb_nil_value();
}

static mrb_value ab_m_translate(mrb_state *m, mrb_value self) {
  mrb_float x, y;
  mrb_get_args(m, "ff", &x, &y);
  ab_translate(x, y);
  (void)self;
  return mrb_nil_value();
}

/* AB.scale(s) is uniform; AB.scale(sx, sy) is not. Matching the Lua runtime,
 * where the second argument defaults to the first. */
static mrb_value ab_m_scale(mrb_state *m, mrb_value self) {
  mrb_float x, y;
  mrb_int argc = mrb_get_args(m, "f|f", &x, &y);
  ab_scale(x, argc >= 2 ? y : x);
  (void)self;
  return mrb_nil_value();
}

static mrb_value ab_m_rotate(mrb_state *m, mrb_value self) {
  mrb_float radians;
  mrb_get_args(m, "f", &radians);
  ab_rotate(radians);
  (void)self;
  return mrb_nil_value();
}

/* skew(x, y = 0) -- shear as tangents; skew(Math::PI/6) leans 30 degrees. */
static mrb_value ab_m_skew(mrb_state *m, mrb_value self) {
  mrb_float x, y = 0;
  mrb_get_args(m, "f|f", &x, &y);
  ab_skew(x, y);
  (void)self;
  return mrb_nil_value();
}

static mrb_value ab_m_transform2d(mrb_state *m, mrb_value self) {
  mrb_float a, b, c, d, e, f;
  mrb_get_args(m, "ffffff", &a, &b, &c, &d, &e, &f);
  ab_transform2d(a, b, c, d, e, f);
  (void)self;
  return mrb_nil_value();
}

/* Pull a numeric Hash member with a default, so a vertex may omit u/v. */
static double hash_num(mrb_state *m, mrb_value hash, const char *key, double fallback) {
  mrb_value v = mrb_hash_get(m, hash, mrb_symbol_value(mrb_intern_cstr(m, key)));
  if (mrb_nil_p(v)) return fallback;
  if (mrb_float_p(v)) return mrb_float(v);
  return (double)mrb_as_int(m, v);
}

/* quad([{x:,y:} x4], texture = 0, rgba = white) -- perspective-correct
 * textured quad, corners clockwise from top-left. A tilt drawn this way
 * reads as a receding plane; the same corners through mesh() warp like a
 * PS1 polygon. */
/* --- offscreen surfaces -------------------------------------------------- */
static mrb_value ab_m_surface_create(mrb_state *m, mrb_value self) {
  mrb_int w, h;
  mrb_get_args(m, "ii", &w, &h);
  (void)self;
  return mrb_int_value(m, ab_surface_create((int32_t)w, (int32_t)h));
}
static mrb_value ab_m_surface_target(mrb_state *m, mrb_value self) {
  mrb_int handle;
  mrb_get_args(m, "i", &handle);
  (void)self;
  return mrb_int_value(m, ab_surface_target((int32_t)handle));
}
static mrb_value ab_m_surface_end(mrb_state *m, mrb_value self) {
  (void)self;
  return mrb_int_value(m, ab_surface_end());
}
static mrb_value ab_m_surface_filter(mrb_state *m, mrb_value self) {
  mrb_int src, dst;
  const char *shader;
  mrb_int len;
  mrb_get_args(m, "iis", &src, &dst, &shader, &len);
  (void)self;
  return mrb_int_value(m, ab_surface_filter_raw((int32_t)src, (int32_t)dst,
                                                shader, (int32_t)len));
}

static mrb_value ab_m_surface_preset(mrb_state *m, mrb_value self) {
  mrb_int src, dst;
  const char *preset;
  mrb_int len;
  mrb_get_args(m, "iis", &src, &dst, &preset, &len);
  (void)self;
  return mrb_int_value(m, ab_surface_preset_raw((int32_t)src, (int32_t)dst,
                                                preset, (int32_t)len));
}

static mrb_value ab_m_quad(mrb_state *m, mrb_value self) {
  mrb_value corners;
  mrb_int texture = 0;
  mrb_value rgba_val = mrb_nil_value();
  ab_point pts[4];
  int i;
  mrb_get_args(m, "A|io", &corners, &texture, &rgba_val);
  (void)self;
  if (RARRAY_LEN(corners) != 4)
    mrb_raise(m, E_ARGUMENT_ERROR, "quad: need exactly 4 corners");
  for (i = 0; i < 4; i++) {
    mrb_value c = mrb_ary_ref(m, corners, i);
    pts[i].x = hash_num(m, c, "x", 0);
    pts[i].y = hash_num(m, c, "y", 0);
  }
  {
    uint32_t rgba = 0xffffffffu;
    if (!mrb_nil_p(rgba_val)) rgba = (uint32_t)mrb_as_int(m, rgba_val);
    return mrb_int_value(m, ab_quad(pts, (int32_t)texture, rgba));
  }
}

/* AB.mesh([{x:, y:, rgba:, u:, v:}, ...], texture = 0) -> triangle count */
static mrb_value ab_m_mesh(mrb_state *m, mrb_value self) {
  mrb_value list;
  mrb_int texture = 0;
  mrb_get_args(m, "o|i", &list, &texture);
  (void)self;
  if (!mrb_array_p(list)) mrb_raise(m, E_TYPE_ERROR, "mesh: expected an Array of vertex Hashes");
  mrb_int count = RARRAY_LEN(list);
  if (count <= 0) return mrb_int_value(m, 0);
  ab_vertex *v = (ab_vertex *)malloc(sizeof(ab_vertex) * (size_t)count);
  if (!v) mrb_raise(m, E_RUNTIME_ERROR, "mesh: out of memory");
  for (mrb_int i = 0; i < count; i++) {
    mrb_value item = RARRAY_PTR(list)[i];
    if (!mrb_hash_p(item)) {
      free(v);
      mrb_raisef(m, E_TYPE_ERROR, "mesh: vertex %d is not a Hash", (int)i);
    }
    /* The arena grows by one value per symbol lookup; without the restore a
     * few thousand vertices leak arena slots and mruby aborts on overflow. */
    int ai = mrb_gc_arena_save(m);
    v[i].x = (float)hash_num(m, item, "x", 0);
    v[i].y = (float)hash_num(m, item, "y", 0);
    v[i].u = (float)hash_num(m, item, "u", 0);
    v[i].v = (float)hash_num(m, item, "v", 0);
    v[i].rgba = (uint32_t)(int64_t)hash_num(m, item, "rgba", 0xffffffffu);
    v[i]._pad = 0;
    mrb_gc_arena_restore(m, ai);
  }
  int32_t emitted = ab_mesh(v, (int32_t)count, (int32_t)texture);
  free(v);
  return mrb_int_value(m, emitted);
}

static mrb_value ab_m_texture_create(mrb_state *m, mrb_value self) {
  const char *pixels; mrb_int n;
  mrb_int w, h;
  mrb_get_args(m, "sii", &pixels, &n, &w, &h);
  (void)self;
  if (w <= 0 || h <= 0 || n < w * h * 4)
    mrb_raisef(m, E_ARGUMENT_ERROR, "texture_create: need %d bytes of RGBA, got %d",
               (int)(w * h * 4), (int)n);
  return mrb_int_value(m, ab_texture_create_rgba(pixels, (int32_t)w, (int32_t)h));
}

static mrb_value ab_m_texture_destroy(mrb_state *m, mrb_value self) {
  mrb_int handle;
  mrb_get_args(m, "i", &handle);
  (void)self;
  return mrb_int_value(m, ab_texture_destroy((int32_t)handle));
}

static mrb_value ab_m_draw_texture(mrb_state *m, mrb_value self) {
  mrb_int handle;
  mrb_float x, y, w, h;
  mrb_get_args(m, "iffff", &handle, &x, &y, &w, &h);
  (void)self;
  return mrb_int_value(m, ab_draw_texture((int32_t)handle, x, y, w, h));
}

static mrb_value ab_m_draw_texture_rect(mrb_state *m, mrb_value self) {
  mrb_int handle, sx, sy, sw, sh;
  mrb_float x, y, w, h;
  mrb_get_args(m, "iffffiiii", &handle, &x, &y, &w, &h, &sx, &sy, &sw, &sh);
  (void)self;
  return mrb_int_value(m, ab_draw_texture_rect((int32_t)handle, x, y, w, h,
    (int32_t)sx, (int32_t)sy, (int32_t)sw, (int32_t)sh));
}

static mrb_value ab_m_effect_set(mrb_state *m, mrb_value self) {
  const char *src; mrb_int n;
  mrb_get_args(m, "s", &src, &n);
  (void)self;
  return mrb_bool_value(ab_effect_set_raw(src, (int32_t)n) != 0);
}

static mrb_value ab_m_effect_clear(mrb_state *m, mrb_value self) {
  (void)self;
  return mrb_int_value(m, ab_effect_clear());
}

static mrb_value ab_m_game_width(mrb_state *m, mrb_value self) {
  (void)self; return mrb_int_value(m, ab_game_width());
}
static mrb_value ab_m_game_height(mrb_state *m, mrb_value self) {
  (void)self; return mrb_int_value(m, ab_game_height());
}

static mrb_value ab_m_game_pixel(mrb_state *m, mrb_value self) {
  mrb_int x, y;
  mrb_get_args(m, "ii", &x, &y);
  (void)self;
  /* 0xRRGGBBAA does not fit a positive 31-bit mrb_int on a 32-bit build, so
   * widen through int64 -- otherwise an opaque white pixel reads negative. */
  return mrb_int_value(m, (mrb_int)(int64_t)ab_game_pixel((int32_t)x, (int32_t)y));
}

static mrb_value ab_m_logical_width(mrb_state *m, mrb_value self) {
  (void)self; return mrb_int_value(m, ab_logical_width());
}
static mrb_value ab_m_logical_height(mrb_state *m, mrb_value self) {
  (void)self; return mrb_int_value(m, ab_logical_height());
}
static mrb_value ab_m_physical_width(mrb_state *m, mrb_value self) {
  (void)self; return mrb_int_value(m, ab_physical_width());
}
static mrb_value ab_m_physical_height(mrb_state *m, mrb_value self) {
  (void)self; return mrb_int_value(m, ab_physical_height());
}
static mrb_value ab_m_elapsed_ms(mrb_state *m, mrb_value self) {
  (void)self; return mrb_float_value(m, ab_elapsed_ms());
}
static mrb_value ab_m_delta_ms(mrb_state *m, mrb_value self) {
  (void)self; return mrb_float_value(m, ab_delta_ms());
}

/* AB.input(port, device, index, id) -- port/device/index default the way the
 * Lua runtime does, so AB.input(0, AB::DEVICE[:JOYPAD], 0, AB::BTN[:A]) and
 * the shorter forms agree. mruby cannot default a LEADING argument, so the
 * one-arg convenience form (id only) is handled by argc. */
static mrb_value ab_m_input(mrb_state *m, mrb_value self) {
  mrb_int a = 0, b = 1, c = 0, d = 0;
  mrb_int argc = mrb_get_args(m, "i|iii", &a, &b, &c, &d);
  (void)self;
  if (argc == 1) return mrb_int_value(m, ab_input_state(0, 1, 0, (int32_t)a));
  return mrb_int_value(m, ab_input_state((int32_t)a, (int32_t)b, (int32_t)c, (int32_t)d));
}

static mrb_value ab_m_log(mrb_state *m, mrb_value self) {
  const char *s; mrb_int n;
  mrb_get_args(m, "s", &s, &n);
  ab_log_raw(s, (int32_t)n);
  (void)self;
  return mrb_nil_value();
}

/* AB.region('system_ram') -> id, or nil when the core has no such region. */
static mrb_value ab_m_region(mrb_state *m, mrb_value self) {
  const char *name; mrb_int n;
  mrb_get_args(m, "s", &name, &n);
  (void)self;
  int32_t id = ab_region_find_raw(name, (int32_t)n);
  return id < 0 ? mrb_nil_value() : mrb_int_value(m, id);
}

static mrb_value ab_m_region_find_id(mrb_state *m, mrb_value self) {
  mrb_int retro_id;
  mrb_get_args(m, "i", &retro_id);
  (void)self;
  int32_t id = ab_region_find_id((int32_t)retro_id);
  return id < 0 ? mrb_nil_value() : mrb_int_value(m, id);
}

static mrb_value ab_m_region_size(mrb_state *m, mrb_value self) {
  mrb_int id; mrb_get_args(m, "i", &id); (void)self;
  return mrb_int_value(m, ab_region_size((int32_t)id));
}
static mrb_value ab_m_region_flags(mrb_state *m, mrb_value self) {
  mrb_int id; mrb_get_args(m, "i", &id); (void)self;
  return mrb_int_value(m, ab_region_flags((int32_t)id));
}
static mrb_value ab_m_region_offset(mrb_state *m, mrb_value self) {
  mrb_int id; mrb_get_args(m, "i", &id); (void)self;
  return mrb_int_value(m, ab_region_offset((int32_t)id));
}
static mrb_value ab_m_region_generation(mrb_state *m, mrb_value self) {
  (void)self; return mrb_int_value(m, ab_region_generation());
}
static mrb_value ab_m_region_count(mrb_state *m, mrb_value self) {
  (void)self; return mrb_int_value(m, ab_region_count());
}

static mrb_value ab_m_read_u8(mrb_state *m, mrb_value self) {
  mrb_int id, off;
  mrb_get_args(m, "ii", &id, &off);
  (void)self;
  return mrb_int_value(m, ab_region_read_u8((int32_t)id, (int32_t)off));
}

static mrb_value ab_m_write_u8(mrb_state *m, mrb_value self) {
  mrb_int id, off, value;
  mrb_get_args(m, "iii", &id, &off, &value);
  (void)self;
  return mrb_int_value(m, ab_region_write_u8((int32_t)id, (int32_t)off, (int32_t)value));
}

/* AB.read(id, offset, length) -> binary String. The per-byte import is fine
 * for a few reads; a String is what a table-driven decoder wants. */
static mrb_value ab_m_read(mrb_state *m, mrb_value self) {
  mrb_int id, off, len;
  mrb_get_args(m, "iii", &id, &off, &len);
  (void)self;
  if (len <= 0 || len > (1 << 20)) mrb_raise(m, E_ARGUMENT_ERROR, "read: bad length");
  char *buf = (char *)malloc((size_t)len);
  if (!buf) mrb_raise(m, E_RUNTIME_ERROR, "read: out of memory");
  for (mrb_int i = 0; i < len; i++) {
    int32_t v = ab_region_read_u8((int32_t)id, (int32_t)(off + i));
    buf[i] = (char)(v < 0 ? 0 : v);
  }
  mrb_value out = mrb_str_new(m, buf, (mrb_int)len);
  free(buf);
  return out;
}

/* AB.asset('assets/data.bin') -> binary String, or nil when absent. */
static mrb_value ab_m_asset(mrb_state *m, mrb_value self) {
  const char *name; mrb_int n;
  mrb_get_args(m, "s", &name, &n);
  (void)self;
  int len = 0; const char *err = NULL;
  unsigned char *bytes = ab_bat_asset_slurp(name, (int)n, &len, &err);
  if (!bytes) return mrb_nil_value();
  mrb_value out = mrb_str_new(m, (const char *)bytes, (mrb_int)len);
  free(bytes);
  return out;
}

static mrb_value ab_m_config_bool(mrb_state *m, mrb_value self) {
  const char *key; mrb_int n;
  mrb_get_args(m, "s", &key, &n);
  (void)self;
  return mrb_bool_value(ab_config_bool_raw(key, (int32_t)n) != 0);
}

static mrb_value ab_m_config_number(mrb_state *m, mrb_value self) {
  const char *key; mrb_int n;
  mrb_get_args(m, "s", &key, &n);
  (void)self;
  return mrb_float_value(m, ab_config_number_raw(key, (int32_t)n));
}

static mrb_value ab_m_config_string(mrb_state *m, mrb_value self) {
  const char *key; mrb_int n;
  mrb_get_args(m, "s", &key, &n);
  (void)self;
  int32_t len = ab_config_string_length_raw(key, (int32_t)n);
  if (len < 0) return mrb_nil_value();
  char *buf = (char *)malloc((size_t)len + 1);
  if (!buf) mrb_raise(m, E_RUNTIME_ERROR, "config_string: out of memory");
  int32_t got = ab_config_string_read_raw(key, (int32_t)n, buf, len);
  mrb_value out = mrb_str_new(m, buf, (mrb_int)(got < 0 ? 0 : got));
  free(buf);
  return out;
}

/* --- images --------------------------------------------------------------
 * AB.image('assets/logo.png') -> { texture:, width:, height: }
 * AB.image_data(bytes) does the same for a raw String. Both go through the
 * shared batteries so every runtime decodes identically.
 */
static mrb_value image_hash(mrb_state *m, int ok, int32_t tex, int w, int h, const char *err) {
  if (!ok) mrb_raisef(m, E_RUNTIME_ERROR, "image: %s", err ? err : "decode failed");
  mrb_value hash = mrb_hash_new_capa(m, 3);
  mrb_hash_set(m, hash, mrb_symbol_value(mrb_intern_lit(m, "texture")), mrb_int_value(m, tex));
  mrb_hash_set(m, hash, mrb_symbol_value(mrb_intern_lit(m, "width")), mrb_int_value(m, w));
  mrb_hash_set(m, hash, mrb_symbol_value(mrb_intern_lit(m, "height")), mrb_int_value(m, h));
  return hash;
}

static mrb_value ab_m_image(mrb_state *m, mrb_value self) {
  const char *name; mrb_int n;
  mrb_get_args(m, "s", &name, &n);
  (void)self;
  int32_t tex = 0; int w = 0, h = 0; const char *err = NULL;
  int ok = ab_bat_image_from_asset(name, (int)n, &tex, &w, &h, &err);
  return image_hash(m, ok, tex, w, h, err);
}

static mrb_value ab_m_image_data(mrb_state *m, mrb_value self) {
  const char *bytes; mrb_int n;
  mrb_get_args(m, "s", &bytes, &n);
  (void)self;
  int32_t tex = 0; int w = 0, h = 0; const char *err = NULL;
  int ok = ab_bat_image_from_memory((const unsigned char *)bytes, (int)n, &tex, &w, &h, &err);
  return image_hash(m, ok, tex, w, h, err);
}

/* --- TrueType text -------------------------------------------------------
 * AB.font('assets/font.ttf') -> handle
 * AB.draw_text(font, text, x, y, px, rgba) -> pen X after the last glyph
 * AB.measure(font, text, px) -> width
 * AB.font_metrics(font, px) -> { ascent:, descent:, line_height: }
 * All shared with the other runtimes (see runtimes/common/ab_batteries.c).
 */
static mrb_value ab_m_font(mrb_state *m, mrb_value self) {
  const char *name; mrb_int n;
  mrb_get_args(m, "s", &name, &n);
  (void)self;
  const char *err = NULL;
  int32_t handle = ab_bat_font_load(name, (int)n, &err);
  if (handle <= 0) mrb_raisef(m, E_RUNTIME_ERROR, "font: %s", err ? err : "load failed");
  return mrb_int_value(m, handle);
}

/* Named draw_text, NOT print: `print` is Kernel#print, which every object
 * already answers to. Defining AB.print as a module_function is fine, but a
 * bezel that writes a bare `print(...)` inside a class body would silently
 * hit Kernel's. draw_text is the unambiguous name, spelled the same in all
 * four runtimes; there is no alias. */
static mrb_value ab_m_draw_text(mrb_state *m, mrb_value self) {
  mrb_int font;
  const char *text; mrb_int tlen;
  mrb_float x, y;
  mrb_int px;
  mrb_value rgba;
  mrb_get_args(m, "isffio", &font, &text, &tlen, &x, &y, &px, &rgba);
  (void)self;
  const char *err = NULL;
  double advance = ab_bat_font_print((int32_t)font, text, (int)tlen, x, y,
                                     (int32_t)px, to_rgba(m, rgba), &err);
  if (err) mrb_raisef(m, E_RUNTIME_ERROR, "draw_text: %s", err);
  return mrb_float_value(m, advance);
}

static mrb_value ab_m_measure(mrb_state *m, mrb_value self) {
  mrb_int font;
  const char *text; mrb_int tlen;
  mrb_int px;
  mrb_get_args(m, "isi", &font, &text, &tlen, &px);
  (void)self;
  const char *err = NULL;
  double w = ab_bat_font_measure((int32_t)font, text, (int)tlen, (int32_t)px, &err);
  if (err) mrb_raisef(m, E_RUNTIME_ERROR, "measure: %s", err);
  return mrb_float_value(m, w);
}

static mrb_value ab_m_font_metrics(mrb_state *m, mrb_value self) {
  mrb_int font, px;
  mrb_get_args(m, "ii", &font, &px);
  (void)self;
  double a = 0, d = 0, lh = 0; const char *err = NULL;
  if (!ab_bat_font_metrics((int32_t)font, (int32_t)px, &a, &d, &lh, &err))
    mrb_raisef(m, E_RUNTIME_ERROR, "font_metrics: %s", err ? err : "failed");
  mrb_value hash = mrb_hash_new_capa(m, 3);
  mrb_hash_set(m, hash, mrb_symbol_value(mrb_intern_lit(m, "ascent")), mrb_float_value(m, a));
  mrb_hash_set(m, hash, mrb_symbol_value(mrb_intern_lit(m, "descent")), mrb_float_value(m, d));
  mrb_hash_set(m, hash, mrb_symbol_value(mrb_intern_lit(m, "line_height")), mrb_float_value(m, lh));
  return hash;
}

/* --- multi-byte region reads (shared) ------------------------------------ */
static mrb_value read_uint(mrb_state *m, int bytes) {
  mrb_int id, off;
  mrb_bool big_endian = FALSE;
  mrb_get_args(m, "ii|b", &id, &off, &big_endian);
  return mrb_int_value(m, (mrb_int)(int64_t)ab_bat_read_uint(
    (int32_t)id, (int32_t)off, bytes, big_endian ? 1 : 0));
}
static mrb_value ab_m_read_u16(mrb_state *m, mrb_value self) { (void)self; return read_uint(m, 2); }
static mrb_value ab_m_read_u24(mrb_state *m, mrb_value self) { (void)self; return read_uint(m, 3); }
static mrb_value ab_m_read_u32(mrb_state *m, mrb_value self) { (void)self; return read_uint(m, 4); }

/* AB.rgb(r, g, b, a = 255) -> packed 0xRRGGBBAA, the format every command
 * takes. Returned via int64 so the alpha bit does not read as a sign. */
static mrb_value ab_m_rgb(mrb_state *m, mrb_value self) {
  mrb_int r, g, b, a = 255;
  mrb_get_args(m, "iii|i", &r, &g, &b, &a);
  (void)self;
  return mrb_int_value(m, (mrb_int)(int64_t)ab_bat_rgba((int)r, (int)g, (int)b, (int)a));
}

/* --- module wiring -------------------------------------------------------- */

typedef struct { const char *name; mrb_func_t fn; mrb_aspec spec; } ab_binding;

static const ab_binding AB_FUNCS[] = {
  { "clear", ab_m_clear, MRB_ARGS_REQ(1) },
  { "draw_game", ab_m_draw_game, MRB_ARGS_ARG(4, 1) },
  { "draw_game_fit", ab_m_draw_game_fit, MRB_ARGS_OPT(4) },
  { "fill_rect", ab_m_fill_rect, MRB_ARGS_REQ(5) },
  { "triangle", ab_m_triangle, MRB_ARGS_REQ(7) },
  { "text", ab_m_text, MRB_ARGS_REQ(5) },
  { "scissor", ab_m_scissor, MRB_ARGS_REQ(4) },
  { "scissor_reset", ab_m_scissor_reset, MRB_ARGS_NONE() },
  { "push_transform", ab_m_push_transform, MRB_ARGS_NONE() },
  { "pop_transform", ab_m_pop_transform, MRB_ARGS_NONE() },
  { "reset_transform", ab_m_reset_transform, MRB_ARGS_NONE() },
  { "translate", ab_m_translate, MRB_ARGS_REQ(2) },
  { "scale", ab_m_scale, MRB_ARGS_ARG(1, 1) },
  { "rotate", ab_m_rotate, MRB_ARGS_REQ(1) },
  { "skew", ab_m_skew, MRB_ARGS_ARG(1, 1) },
  { "transform2d", ab_m_transform2d, MRB_ARGS_REQ(6) },
  { "quad", ab_m_quad, MRB_ARGS_ARG(1, 2) },
  { "surface_create", ab_m_surface_create, MRB_ARGS_REQ(2) },
  { "surface_target", ab_m_surface_target, MRB_ARGS_REQ(1) },
  { "surface_end", ab_m_surface_end, MRB_ARGS_NONE() },
  { "surface_filter", ab_m_surface_filter, MRB_ARGS_REQ(3) },
  { "surface_preset", ab_m_surface_preset, MRB_ARGS_REQ(3) },
  { "mesh", ab_m_mesh, MRB_ARGS_ARG(1, 1) },
  { "texture_create", ab_m_texture_create, MRB_ARGS_REQ(3) },
  { "texture_destroy", ab_m_texture_destroy, MRB_ARGS_REQ(1) },
  { "draw_texture", ab_m_draw_texture, MRB_ARGS_REQ(5) },
  { "draw_texture_rect", ab_m_draw_texture_rect, MRB_ARGS_REQ(9) },
  { "effect_set", ab_m_effect_set, MRB_ARGS_REQ(1) },
  { "effect_clear", ab_m_effect_clear, MRB_ARGS_NONE() },
  { "game_width", ab_m_game_width, MRB_ARGS_NONE() },
  { "game_height", ab_m_game_height, MRB_ARGS_NONE() },
  { "game_pixel", ab_m_game_pixel, MRB_ARGS_REQ(2) },
  { "logical_width", ab_m_logical_width, MRB_ARGS_NONE() },
  { "logical_height", ab_m_logical_height, MRB_ARGS_NONE() },
  { "physical_width", ab_m_physical_width, MRB_ARGS_NONE() },
  { "physical_height", ab_m_physical_height, MRB_ARGS_NONE() },
  { "elapsed_ms", ab_m_elapsed_ms, MRB_ARGS_NONE() },
  { "delta_ms", ab_m_delta_ms, MRB_ARGS_NONE() },
  { "input", ab_m_input, MRB_ARGS_ARG(1, 3) },
  { "log", ab_m_log, MRB_ARGS_REQ(1) },
  { "region", ab_m_region, MRB_ARGS_REQ(1) },
  { "region_find_id", ab_m_region_find_id, MRB_ARGS_REQ(1) },
  { "region_size", ab_m_region_size, MRB_ARGS_REQ(1) },
  { "region_flags", ab_m_region_flags, MRB_ARGS_REQ(1) },
  { "region_offset", ab_m_region_offset, MRB_ARGS_REQ(1) },
  { "region_generation", ab_m_region_generation, MRB_ARGS_NONE() },
  { "region_count", ab_m_region_count, MRB_ARGS_NONE() },
  { "read_u8", ab_m_read_u8, MRB_ARGS_REQ(2) },
  { "write_u8", ab_m_write_u8, MRB_ARGS_REQ(3) },
  { "read", ab_m_read, MRB_ARGS_REQ(3) },
  { "asset", ab_m_asset, MRB_ARGS_REQ(1) },
  { "image", ab_m_image, MRB_ARGS_REQ(1) },
  { "image_data", ab_m_image_data, MRB_ARGS_REQ(1) },
  { "font", ab_m_font, MRB_ARGS_REQ(1) },
  /* draw_text is the only name: it is spelled the same in all four
   * runtimes and cannot collide with Kernel#print. */
  { "draw_text", ab_m_draw_text, MRB_ARGS_REQ(6) },
  { "measure", ab_m_measure, MRB_ARGS_REQ(3) },
  { "font_metrics", ab_m_font_metrics, MRB_ARGS_REQ(2) },
  { "read_u16", ab_m_read_u16, MRB_ARGS_ARG(2, 1) },
  { "read_u24", ab_m_read_u24, MRB_ARGS_ARG(2, 1) },
  { "read_u32", ab_m_read_u32, MRB_ARGS_ARG(2, 1) },
  { "config_bool", ab_m_config_bool, MRB_ARGS_REQ(1) },
  { "config_number", ab_m_config_number, MRB_ARGS_REQ(1) },
  { "config_string", ab_m_config_string, MRB_ARGS_REQ(1) },
  { "rgb", ab_m_rgb, MRB_ARGS_ARG(3, 1) },
};

/* Constant Hashes so scripts never hard-code ABI numbers. Mirrors the C SDK's
 * AB_* defines; button ids are the libretro joypad numbering the input_state
 * import speaks. Frozen, because a script that mutates AB::BTN would be
 * rewriting the ABI for everything else in the same VM. */
typedef struct { const char *name; int32_t value; } ab_const;

static void define_const_hash(mrb_state *m, struct RClass *mod, const char *name,
                              const ab_const *rows, int count) {
  mrb_value hash = mrb_hash_new_capa(m, count);
  for (int i = 0; i < count; i++)
    mrb_hash_set(m, hash, mrb_symbol_value(mrb_intern_cstr(m, rows[i].name)),
                 mrb_int_value(m, rows[i].value));
  mrb_obj_freeze(m, hash);
  mrb_define_const(m, mod, name, hash);
}

static void define_ab_module(mrb_state *m) {
  struct RClass *ab = mrb_define_module(m, "AB");
  for (unsigned i = 0; i < sizeof(AB_FUNCS) / sizeof(AB_FUNCS[0]); i++)
    mrb_define_module_function(m, ab, AB_FUNCS[i].name, AB_FUNCS[i].fn, AB_FUNCS[i].spec);

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
  /* AB::GAME: pass as a texture handle to sample the LIVE GAME FRAME, e.g.
   * AB.quad(corners, AB::GAME) maps the running game onto a tilted plane. */
  mrb_define_const(m, ab, "GAME", mrb_int_value(m, -1));
  define_const_hash(m, ab, "EVENT", EVENT, (int)(sizeof(EVENT) / sizeof(EVENT[0])));
  define_const_hash(m, ab, "FIT", FIT, (int)(sizeof(FIT) / sizeof(FIT[0])));
  define_const_hash(m, ab, "SAMPLE", SAMPLE, (int)(sizeof(SAMPLE) / sizeof(SAMPLE[0])));
  define_const_hash(m, ab, "DEVICE", DEVICE, (int)(sizeof(DEVICE) / sizeof(DEVICE[0])));
  define_const_hash(m, ab, "BTN", BTN, (int)(sizeof(BTN) / sizeof(BTN[0])));
}

/* -------------------------------------------------------- script loading -- */

/* Does the top-level object answer to `name`? Scripts define init/tick/event
 * as top-level methods, which mruby installs as private methods on Object --
 * respond_to? would answer false, so ask the class directly. */
static int top_level_defines(mrb_state *m, const char *name) {
  return mrb_obj_respond_to(m, m->object_class, mrb_intern_cstr(m, name)) ? 1 : 0;
}

static int load_script(void) {
  static const char *CANDIDATES[] = { "main.rb", "assets/main.rb" };
  const char *name = NULL;
  int32_t size = -1;
  for (unsigned i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++) {
    size = ab_asset_size(CANDIDATES[i]);
    if (size >= 0) { name = CANDIDATES[i]; break; }
  }
  if (!name) { set_error("ruby runtime: no main.rb (or assets/main.rb) in the package"); return 0; }

  char *source = (char *)malloc((size_t)size + 1);
  if (!source) { set_error("ruby runtime: out of memory reading main.rb"); return 0; }
  int32_t got = ab_asset_read(name, source, size);
  if (got < 0) { free(source); set_error("ruby runtime: asset_read failed for main.rb"); return 0; }
  source[got < 0 ? 0 : got] = 0;

  /* mrbc_context with a filename makes backtraces name main.rb rather than
   * "(eval)", which is the difference between a usable error panel and a
   * riddle. */
  mrbc_context *ctx = mrbc_context_new(mrb);
  if (ctx) {
    mrbc_filename(mrb, ctx, name);
    /* capture_errors is NOT optional here, and it is not about message
     * quality: without it mruby's yyerror does fprintf(stderr, ...) on a
     * syntax error. stderr goes to the WASI fd_write stub, which reports 0
     * bytes written, so libc's flush loop never advances and the whole host
     * session HANGS on a typo in main.rb. With it, the parse error stays in
     * the parser buffer and comes back as a SyntaxError on mrb->exc, which
     * the error panel can actually show. */
    ctx->capture_errors = TRUE;
  }
  mrb_load_nstring_cxt(mrb, source, (mrb_int)(got < 0 ? 0 : got), ctx);
  if (ctx) mrbc_context_free(mrb, ctx);
  free(source);
  if (guard("ruby runtime: main.rb")) return 0;

  g_has_tick = top_level_defines(mrb, "tick");
  g_has_event = top_level_defines(mrb, "event");
  if (!g_has_tick) {
    set_error("ruby runtime: main.rb must define a top-level method tick(frame)");
    return 0;
  }

  if (top_level_defines(mrb, "init")) {
    mrb_funcall(mrb, mrb_top_self(mrb), "init", 0);
    if (guard("ruby runtime: init()")) return 0;
  }
  g_error[0] = 0;
  return 1;
}

static void boot(void) {
  if (mrb) { mrb_close(mrb); mrb = NULL; }
  g_has_tick = g_has_event = 0;
  g_error[0] = 0;
  mrb = mrb_open();
  if (!mrb) { set_error("ruby runtime: mrb_open failed"); return; }
  define_ab_module(mrb);
  /* Platform redraw profiles: registers the NES/GB/MD/SNES/MSX/PCE modules. */
  ab_profiles_rb_define(mrb);
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
  if (g_error[0] || !mrb || !g_has_tick) {
    ab_clear(0x201018ffu);
    ab_draw_game_fit(0, 0.5, 0.35, 0);
    ab_text("ruby bezel error:", 40, 40, 30, 0xff8080ffu);
    ab_text(g_error[0] ? g_error : "script not loaded", 40, 84, 24, 0xffd0d0ffu);
    ab_text("fix main.rb and repack -- the runtime reloads it", 40, 124, 22, 0x9098b0ffu);
    return;
  }
  /* Arena save/restore around every tick: each mrb_funcall and every value a
   * binding returns lands in the GC arena, and 60 unrestored ticks a second
   * overflow it in under a minute of play. */
  int ai = mrb_gc_arena_save(mrb);
  mrb_funcall(mrb, mrb_top_self(mrb), "tick", 1, mrb_int_value(mrb, (mrb_int)frame));
  guard("tick()");
  mrb_gc_arena_restore(mrb, ai);
}

AB_EXPORT("ab_event")
void ab_event(int32_t kind, uint32_t data) {
  (void)data;                          /* reserved by the ABI, unused today */
  /* ASSETS_RELOADED (6): the package archive changed under us -- re-read
   * main.rb. This is the whole iteration story: edit, repack, replay. */
  if (kind == 6) { boot(); return; }
  if (!mrb || !g_has_event || g_error[0]) return;
  int ai = mrb_gc_arena_save(mrb);
  mrb_funcall(mrb, mrb_top_self(mrb), "event", 1, mrb_int_value(mrb, kind));
  guard("event()");
  mrb_gc_arena_restore(mrb, ai);
}

AB_EXPORT("ab_shutdown")
void ab_shutdown(void) {
  if (mrb) { mrb_close(mrb); mrb = NULL; }
  ab_bat_shutdown();
  ab_profiles_rb_shutdown();
}
