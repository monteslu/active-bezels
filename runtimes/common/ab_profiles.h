/*
 * ab_profiles.h -- language-neutral cores for the platform redraw profiles.
 *
 * Everything the `nes` / `gb` / `md` / `snes` / `msx` / `pce` script globals
 * DO lives here; the per-runtime files (ab_profiles_lua.c and friends) are
 * marshaling only. The split happened when the second consumer appeared:
 * the alternative was four hand-kept copies of draw orchestration that was
 * certified against real-cart corpora, which is exactly how certified
 * renderers drift.
 *
 * Contract shared by every profile:
 *   *_bind()   resolves regions and allocates once; idempotent. Returns 1,
 *              or 0 with *err pointing at a static string. A missing
 *              OPTIONAL region is not an error (the profile silently uses
 *              its documented fallback; the draw result flags report which
 *              path ran).
 *   *_draw()   returns 1 and fills the result struct, or 0 with *err set.
 *   *_bound()  lets a shim raise its language's "call bind() first" error
 *              before touching anything else.
 *
 * Buffers are allocated once at bind and freed only by *_shutdown() (called
 * from the runtime's ab_shutdown). Profile state deliberately survives a
 * script reload, matching what the Lua runtime always did: a reloaded
 * script's bind() finds the profile already bound and returns immediately.
 */
#ifndef AB_PROFILES_H
#define AB_PROFILES_H

#include <stddef.h>
#include <stdint.h>

#include "ab_render.h"

/* ------------------------------------------------------------- shared -- */

/* One view shape covers nes/gb/md; msx and pce extend it below. */
typedef struct {
  double x, y, scale;
} ab_prof_view;

/* Substitution management is identical across the five sprite profiles.
 * The shim fills an ab_sub_rule (ab_render.h) -- tiles, anchor_exclude,
 * texture/tex_w/tex_h from the image, base_w/base_h/ring -- and the core
 * owns the registry and the parallel art table. */
typedef enum {
  AB_PROF_NES,
  AB_PROF_GB,
  AB_PROF_MD,
  AB_PROF_MSX,
  AB_PROF_PCE,
  AB_PROF_COUNT
} ab_prof_id;

int  ab_prof_bound(ab_prof_id p);
int  ab_prof_add_rule(ab_prof_id p, const ab_sub_rule *rule);  /* id or 0 */
int  ab_prof_remove_rule(ab_prof_id p, int id);                /* 1 / 0 */
void ab_prof_clear_rules(ab_prof_id p);

/* ---------------------------------------------------------------- NES -- */

typedef struct {
  int bg_quads, spr_quads, hd_drawn, sprites_replaced;
} ab_prof_nes_result;

int  ab_prof_nes_bind(const char **err);
int  ab_prof_nes_draw(const ab_prof_view *v, ab_prof_nes_result *r,
                      const char **err);
int  ab_prof_nes_sprite_bounds(int out[4]);        /* 1 = box valid */
void ab_prof_nes_shutdown(void);

/* ----------------------------------------------------------------- GB -- */

typedef struct {
  int bg_quads, spr_quads, hd_drawn, sprites_replaced;
} ab_prof_gb_result;

int  ab_prof_gb_bind(const char **err);
int  ab_prof_gb_draw(const ab_prof_view *v, ab_prof_gb_result *r,
                     const char **err);
int  ab_prof_gb_sprite_bounds(int out[4]);
void ab_prof_gb_shutdown(void);

/* ----------------------------------------------------------------- MD -- */

typedef struct {
  int quads, hd_drawn, sprites_replaced;
} ab_prof_md_result;

int  ab_prof_md_bind(const char **err);
int  ab_prof_md_draw(const ab_prof_view *v, ab_prof_md_result *r,
                     const char **err);
int  ab_prof_md_sprite_bounds(int out[4]);
void ab_prof_md_shutdown(void);

/* ---------------------------------------------------------------- MSX -- */

typedef struct {
  ab_prof_view v;
  int fit_width;                       /* squeeze 512-wide modes; default 0 */
} ab_prof_msx_view;

typedef struct {
  int quads, hd_drawn, sprites_replaced;
  int supported;                       /* 0 => nothing was drawn, mode = -1 */
  int mode;                            /* AB_MSX_MODE_*, -1 if unsupported */
  int width;                           /* actual pixel width: 272 or 544 */
  int per_line, vram_replay, retained; /* which rendering path actually ran */
} ab_prof_msx_result;

int  ab_prof_msx_bind(const char **err);
int  ab_prof_msx_draw(const ab_prof_msx_view *v, ab_prof_msx_result *r,
                      const char **err);

/* mode query without drawing. Returns:
 *   AB_PROF_MSX_MODE_OK          *mode + *desc filled
 *   AB_PROF_MSX_MODE_UNSUPPORTED *desc says what is genuinely left
 *   AB_PROF_MSX_MODE_READ_FAIL   frame read failed, nothing filled */
enum { AB_PROF_MSX_MODE_READ_FAIL = 0, AB_PROF_MSX_MODE_OK = 1,
       AB_PROF_MSX_MODE_UNSUPPORTED = 2 };
int  ab_prof_msx_mode(int *mode, const char **desc);

typedef struct { int index, x, y, pattern, colour; } ab_prof_msx_sprite;
/* Fills up to AB_MSX_SPRITES entries, stopping at the y==208 terminator
 * like the hardware. Returns the count, or -1 when the frame read failed. */
int  ab_prof_msx_sprites(ab_prof_msx_sprite *out, int max);

int  ab_prof_msx_sprite_bounds(int out[4]);
void ab_prof_msx_shutdown(void);

/* ---------------------------------------------------------------- PCE -- */

typedef struct {
  ab_prof_view v;
  int height;                 /* VDC registers cannot say; default 224 */
  int force_bg, force_sprites;/* tri-state: -1 follow registers, else 0/1 */
  int fb_width;               /* the CORE's framebuffer width; 0 = derive */
  /* diagnostic/control knobs; the shim leaves the sentinel when the script
   * did not pass the field, so live values are only poked when asked */
  int pal_delta_row;          /* INT32_MIN = untouched */
  int pal_delta;              /* INT32_MIN = untouched */
  int no_linepix;             /* -1 = untouched, else 0/1 */
  int no_paldeltas;           /* -1 = untouched, else 0/1 */
} ab_prof_pce_view;

#define AB_PROF_PCE_UNSET INT32_MIN

/* Fills the defaults above (x/y 0, scale 4, height 224, tri-states -1,
 * knobs untouched). Call before copying script fields in. */
void ab_prof_pce_view_init(ab_prof_pce_view *v);

typedef struct {
  int quads, hd_drawn, sprites_replaced, width, height;
} ab_prof_pce_result;

int  ab_prof_pce_bind(const char **err);
int  ab_prof_pce_draw(const ab_prof_pce_view *v, ab_prof_pce_result *r,
                      const char **err);
int  ab_prof_pce_sprite_bounds(int height, int fb_width, int out[4]);

typedef struct {
  int width, bg, sprites, bat_w, bat_h, scroll_x, scroll_y;
} ab_prof_pce_geometry;
int  ab_prof_pce_get_geometry(ab_prof_pce_geometry *g);   /* 1 ok, 0 read fail */

void ab_prof_pce_shutdown(void);

/* --------------------------------------------------------------- SNES -- */

int ab_prof_snes_bound(void);
int ab_prof_snes_bind(const char **err);

/* tiles.bin v2 blob. Returns 1 and the per-mode counts, or 0 with *err. */
int ab_prof_snes_set_hd_tiles(const void *blob, size_t n,
                              int *indexed_count, int *rgba_count,
                              const char **err);

typedef struct { int w, h, m7start, m7stop, plane_rebuilt; } ab_prof_snes_tick_result;
/* Returns:
 *   AB_PROF_SNES_NOT_READY  hi-res / no frame yet; nothing was drawn
 *   AB_PROF_SNES_PLAIN      no Mode 7 span this frame; the plain frame (and
 *                           compare view) were drawn; result NOT filled
 *   AB_PROF_SNES_M7         full result filled
 *   0 with *err set only on allocation failure */
enum { AB_PROF_SNES_ERROR = 0, AB_PROF_SNES_NOT_READY = 1,
       AB_PROF_SNES_PLAIN = 2, AB_PROF_SNES_M7 = 3 };
int ab_prof_snes_tick(int compare, ab_prof_snes_tick_result *r,
                      const char **err);

typedef struct { int w, h, quads; } ab_prof_snes_draw_result;
/* 1 = drew + filled; 0 with *err NULL = not ready (hi-res / no frame);
 * 0 with *err set = allocation failure. */
int ab_prof_snes_draw(double x, double y, double scale,
                      ab_prof_snes_draw_result *r, const char **err);

int ab_prof_snes_frame_size(int *w, int *h);          /* 1 ok, 0 not ready */
void ab_prof_snes_shutdown(void);

#endif /* AB_PROFILES_H */
