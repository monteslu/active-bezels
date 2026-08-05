#include "ab_snes.h"
#include <math.h>
#include <string.h>

static inline int32_t sext13(int32_t v) {
  v &= 0x1FFF;
  return v >= 0x1000 ? v - 0x2000 : v;
}

/* snes9x tileimpl.h CLIP_10_BIT_SIGNED, on the int32 the core uses. */
static inline int32_t clip10(int32_t a) {
  return (a & 0x2000) ? (a | ~0x3ff) : (a & 0x3ff);
}

void ab_snes_m7_uv(const uint8_t *m7, int y, int width,
                   double *u0, double *v0, double *u1, double *v1) {
  const uint8_t *p = m7 + y * 16;
  int16_t r[8];
  memcpy(r, p, 16);                    /* buffer is LE; wasm is LE */
  int32_t A = r[0], B = r[1], C = r[2], D = r[3];
  int32_t CX = sext13(r[4]), CY = sext13(r[5]);
  int32_t HO = sext13(r[6]), VO = sext13(r[7]);
  int32_t starty = y + 1;
  int32_t yy = clip10(VO - CY);
  int32_t BB = ((B * starty) & ~63) + ((B * yy) & ~63) + (CX << 8);
  int32_t DD = ((D * starty) & ~63) + ((D * yy) & ~63) + (CY << 8);
  int32_t xx = clip10(HO - CX);
  int32_t AAc = (A * xx) & ~63;
  int32_t CCc = (C * xx) & ~63;
  const double inv = 1.0 / (1024.0 * 256.0);
  *u0 = (double)(AAc + BB) * inv;
  *v0 = (double)(CCc + DD) * inv;
  *u1 = ((double)A * width + AAc + BB) * inv;
  *v1 = ((double)C * width + CCc + DD) * inv;
}

void ab_snes_wrap_align(double *q) {
  /* q = {au0,av0,au1,av1,bu0,bv0,bu1,bv1}. Bottom corners shift by whole
   * periods to the SHORT path vs their top counterpart, then everything
   * rebases near zero. Both are sampling-identical under REPEAT wrap. */
  q[4] += floor(q[0] - q[4] + 0.5);
  q[5] += floor(q[1] - q[5] + 0.5);
  q[6] += floor(q[2] - q[6] + 0.5);
  q[7] += floor(q[3] - q[7] + 0.5);
  double du = q[0]; double dv = q[1];
  if (q[2] < du) du = q[2];
  if (q[4] < du) du = q[4];
  if (q[6] < du) du = q[6];
  if (q[3] < dv) dv = q[3];
  if (q[5] < dv) dv = q[5];
  if (q[7] < dv) dv = q[7];
  du = floor(du); dv = floor(dv);
  q[0] -= du; q[2] -= du; q[4] -= du; q[6] -= du;
  q[1] -= dv; q[3] -= dv; q[5] -= dv; q[7] -= dv;
}

int ab_snes_depth_runs(const uint8_t *depth_row, int width,
                       int *runs, int max_runs) {
  int n = 0;
  int x = 0;
  while (x < width && n < max_runs) {
    if (ab_snes_depth_is_obj(depth_row[x])) {
      int x0 = x;
      do { x++; } while (x < width && ab_snes_depth_is_obj(depth_row[x]));
      runs[n * 2] = x0;
      runs[n * 2 + 1] = x;
      n++;
    } else {
      x++;
    }
  }
  return n;
}

void ab_snes_565_row(const uint8_t *src, uint8_t *dst, int px) {
  for (int x = 0; x < px; x++) {
    uint16_t v = (uint16_t)(src[x * 2] | (src[x * 2 + 1] << 8));
    uint8_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
    dst[x * 4]     = (uint8_t)((r << 3) | (r >> 2));
    dst[x * 4 + 1] = (uint8_t)((g << 2) | (g >> 4));
    dst[x * 4 + 2] = (uint8_t)((b << 3) | (b >> 2));
    dst[x * 4 + 3] = 255;
  }
}

void ab_snes_palette(const uint8_t *cgram, uint8_t *rgba256) {
  for (int i = 0; i < 256; i++) {
    uint16_t v = (uint16_t)(cgram[i * 2] | (cgram[i * 2 + 1] << 8));
    uint8_t r = v & 0x1F, g = (v >> 5) & 0x1F, b = (v >> 10) & 0x1F;
    rgba256[i * 4]     = (uint8_t)((r << 3) | (r >> 2));
    rgba256[i * 4 + 1] = (uint8_t)((g << 3) | (g >> 2));
    rgba256[i * 4 + 2] = (uint8_t)((b << 3) | (b >> 2));
    rgba256[i * 4 + 3] = 255;
  }
}
