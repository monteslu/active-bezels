/*
 * ab_snes -- SNES Mode 7 re-projection profile, pure helpers.
 *
 * The hot per-tick loops of an HD Mode 7 bezel, in C. The math is snes9x's
 * DrawTileNormal<>::Draw formula VERBATIM (tileimpl.h) over the captured
 * per-line LineMatrixData records (snes_m7lines, 8 x s16 LE per line) --
 * capture the resolved value, never re-derive. The traps that shaped this
 * (streaming tilemap, depth values, wrap alignment) are documented where
 * each helper is used.
 *
 * Everything here is host-free and testable: no ab_* imports.
 */
#ifndef AB_SNES_H
#define AB_SNES_H

#include <stdint.h>

/* Per-line plane UVs (in plane units 0..1 = 1024px) at screen x=0 and
 * x=width, from one 16-byte m7lines record. Core-verbatim integer walk. */
void ab_snes_m7_uv(const uint8_t *m7, int y, int width,
                   double *u0, double *v0, double *u1, double *v1);

/* Align strip-bottom UVs to the top line's wrap period (short path -- the
 * plane repeats mod 1) and rebase all eight near zero so f32 fract() in the
 * shader keeps texel precision. Operates on {au0,av0,au1,av1,bu0,bv0,bu1,bv1}. */
void ab_snes_wrap_align(double *q);

/* OBJ depth mask: snes9x main-screen depths are exactly 36/40/44/48 for
 * sprites (D+4 + prio*4); the M7 plane writes 39, EXTBG 35/43. */
static inline int ab_snes_depth_is_obj(uint8_t d) {
  return d >= 36 && ((d - 36) & 3) == 0;
}

/* Scan one 512-byte depth row for OBJ runs; writes (x0,x1) pairs (x1
 * exclusive) into runs, returns pair count (capped at max_runs). */
int ab_snes_depth_runs(const uint8_t *depth_row, int width,
                       int *runs, int max_runs);

/* RGB565 -> RGBA8888 widen, the host's exact rule, one row. */
void ab_snes_565_row(const uint8_t *src, uint8_t *dst, int px);

/* CGRAM (BGR555) -> 256x1 RGBA palette. */
void ab_snes_palette(const uint8_t *cgram, uint8_t *rgba256);

#endif
