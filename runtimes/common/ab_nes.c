/*
 * ab_nes.c -- NES profile. See ab_nes.h for the facts this encodes.
 */
#include "ab_nes.h"

#include <string.h>

/* fceumm ApplyDeemphasisClassic multipliers (palette.c rtmul/gtmul/btmul),
 * indexed by PPUMASK>>5 minus one; emphasis 0 is unmodified. */
static const double EMPH_R[7] = { 1.239, 0.794, 1.019, 0.905, 1.023, 0.741, 0.75 };
static const double EMPH_G[7] = { 0.915, 1.086, 0.980, 1.026, 0.908, 0.987, 0.75 };
static const double EMPH_B[7] = { 0.743, 0.882, 0.653, 1.277, 0.979, 0.101, 0.75 };

/* Fallback NTSC table, used ONLY when nes_palrgb is absent. A VS System cart
 * selects a different table, which is exactly why palrgb exists -- this is a
 * degradation path, not the normal one. */
static const unsigned char NES_RGB_FALLBACK[64 * 3] = {
  0x74,0x74,0x74, 0x24,0x18,0x8c, 0x00,0x00,0xa8, 0x44,0x00,0x9c,
  0x8c,0x00,0x74, 0xa8,0x00,0x10, 0xa4,0x00,0x00, 0x7c,0x08,0x00,
  0x40,0x2c,0x00, 0x00,0x44,0x00, 0x00,0x50,0x00, 0x00,0x3c,0x14,
  0x18,0x3c,0x5c, 0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00,
  0xbc,0xbc,0xbc, 0x00,0x70,0xec, 0x20,0x38,0xec, 0x80,0x00,0xf0,
  0xbc,0x00,0xbc, 0xe4,0x00,0x58, 0xd8,0x28,0x00, 0xc8,0x4c,0x0c,
  0x88,0x70,0x00, 0x00,0x94,0x00, 0x00,0xa8,0x00, 0x00,0x90,0x38,
  0x00,0x80,0x88, 0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00,
  0xfc,0xfc,0xfc, 0x3c,0xbc,0xfc, 0x5c,0x94,0xfc, 0xcc,0x88,0xfc,
  0xf4,0x78,0xfc, 0xfc,0x74,0xb4, 0xfc,0x74,0x60, 0xfc,0x98,0x38,
  0xf0,0xbc,0x3c, 0x80,0xd0,0x10, 0x4c,0xdc,0x48, 0x58,0xf8,0x98,
  0x00,0xe8,0xd8, 0x78,0x78,0x78, 0x00,0x00,0x00, 0x00,0x00,0x00,
  0xfc,0xfc,0xfc, 0xa8,0xe4,0xfc, 0xc4,0xd4,0xfc, 0xd4,0xc8,0xfc,
  0xfc,0xc4,0xfc, 0xfc,0xc4,0xd8, 0xfc,0xbc,0xb0, 0xfc,0xd8,0xa8,
  0xfc,0xe4,0xa0, 0xe0,0xfc,0xa0, 0xa8,0xf0,0xbc, 0xb0,0xfc,0xcc,
  0x9c,0xfc,0xf0, 0xc4,0xc4,0xc4, 0x00,0x00,0x00, 0x00,0x00,0x00
};

int ab_nes_frame_read(const ab_nes_regions *r, ab_nes_frame *f) {
  if (!r || !f || !f->bgval || !f->sprdrawn) return 0;

  ab_region_slurp(r->bgval,    0, f->bgval,    AB_NES_W * AB_NES_LINES);
  ab_region_slurp(r->sprdrawn, 0, f->sprdrawn, AB_NES_W * AB_NES_LINES);
  ab_region_slurp(r->palette,  0, f->palette,  32);
  ab_region_slurp(r->oam,      0, f->oam,      256);

  f->have_bgpix = 0;
  if (r->bgpix && f->bgpix) {
    ab_region_slurp(r->bgpix, 0, f->bgpix, AB_NES_W * AB_NES_LINES);
    f->have_bgpix = 1;
  }

  f->have_masklines = 0;
  if (r->masklines) {
    ab_region_slurp(r->masklines, 0, f->masklines, AB_NES_LINES);
    f->have_masklines = 1;
  }
  f->have_palrgb = 0;
  if (r->palrgb) {
    ab_region_slurp(r->palrgb, 0, f->palrgb, 192);
    f->have_palrgb = 1;
  }
  return 1;
}

int ab_nes_line_mask(const ab_nes_frame *f, int ppu_line, int frame_mask) {
  if (!f) return frame_mask;
  if (!f->have_masklines) return frame_mask;
  if (ppu_line < 0 || ppu_line >= AB_NES_LINES) return frame_mask;
  return f->masklines[ppu_line];
}

uint32_t ab_nes_rgba(const ab_nes_frame *f, int value, int mask) {
  int i = value;
  /* Greyscale ANDs the palette INDEX with $30 (fceumm: ret &= 0x30) and
   * applies to the backdrop too. */
  if (mask & 0x01) i &= 0x30;
  i &= 0x3F;                       /* bit-6 transparency flag lives above */

  const unsigned char *tbl = (f && f->have_palrgb) ? f->palrgb : NES_RGB_FALLBACK;
  double r = tbl[i * 3];
  double g = tbl[i * 3 + 1];
  double b = tbl[i * 3 + 2];

  const int e = (mask >> 5) & 0x07;
  if (e) {
    r *= EMPH_R[e - 1];
    g *= EMPH_G[e - 1];
    b *= EMPH_B[e - 1];
    if (r > 255.0) r = 255.0;
    if (g > 255.0) g = 255.0;
    if (b > 255.0) b = 255.0;
  }
  return ((uint32_t)(int)r << 24) | ((uint32_t)(int)g << 16) |
         ((uint32_t)(int)b << 8) | 0xFFu;
}

void ab_nes_build_lut(const ab_nes_frame *f, int mask, uint32_t out_lut[256]) {
  /* Index over the FULL byte range: bgval/sprdrawn values carry bit 6, and a
   * 64-entry table returns garbage for flagged bytes. */
  for (int v = 0; v < 256; v++) out_lut[v] = ab_nes_rgba(f, v, mask);
}

/* Emit run-coalesced quads for one pixel plane. `pick` returns the palette
 * value to draw, or -1 to skip. Runs are long on real frames (flat colour
 * spans), which is what keeps the quad count far below the pixel count. */
static int emit_plane(ab_batch *b, const ab_nes_frame *f, const ab_nes_view *v,
                      int frame_mask, const unsigned char *plane,
                      const unsigned char *bgpix, const unsigned char *bgval,
                      const unsigned char *suppress, int sprite_rules) {
  if (!b || !f || !v || !plane) return 0;
  const int have_bgpix = bgpix != NULL;

  uint32_t lut[256];
  int lut_mask = -1;
  int quads = 0;

  for (int y = 0; y < AB_NES_H; y++) {
    const int line = y + AB_NES_OVERSCAN_TOP;
    const int mask = ab_nes_line_mask(f, line, frame_mask);

    /* Rebuild the LUT only when the line's emphasis/greyscale state changes;
     * on a normal frame that is once. */
    if (mask != lut_mask) { ab_nes_build_lut(f, mask, lut); lut_mask = mask; }

    const unsigned char *row = plane + (size_t)line * AB_NES_W;
    const unsigned char *prow = bgpix ? bgpix + (size_t)line * AB_NES_W : NULL;
    const unsigned char *vrow = bgval ? bgval + (size_t)line * AB_NES_W : NULL;
    const unsigned char *srow = suppress ? suppress + (size_t)line * AB_NES_W : NULL;

    int x = 0;
    while (x < AB_NES_W) {
      const unsigned char px = row[x];
      int draw;
      if (sprite_rules) {
        draw = ab_nes_spr_visible(px, prow ? prow[x] : 0, have_bgpix,
                                  vrow ? vrow[x] : 0);
        if (draw && srow && srow[x]) draw = 0;   /* replaced by HD art */
      } else {
        draw = 1;
      }
      if (!draw) { x++; continue; }

      const int x0 = x;
      /* extend the run while the drawn value and the draw decision hold */
      while (x + 1 < AB_NES_W) {
        const unsigned char nx = row[x + 1];
        if (nx != px) break;
        if (sprite_rules) {
          if (!ab_nes_spr_visible(nx, prow ? prow[x + 1] : 0, have_bgpix,
                                  vrow ? vrow[x + 1] : 0)) break;
          if (srow && srow[x + 1]) break;
        }
        x++;
      }

      ab_batch_solid(b,
                     v->ox + x0 * v->scale, v->oy + y * v->scale,
                     (x - x0 + 1) * v->scale, v->scale,
                     lut[px]);
      quads++;
      x++;
    }
  }
  return quads;
}

int ab_nes_emit_background(ab_batch *b, const ab_nes_frame *f,
                           const ab_nes_view *v, int frame_mask) {
  return emit_plane(b, f, v, frame_mask, f ? f->bgval : NULL, NULL, NULL,
                    NULL, 0);
}

int ab_nes_emit_sprites(ab_batch *b, const ab_nes_frame *f,
                        const ab_nes_view *v, int frame_mask,
                        const unsigned char *suppress) {
  if (!f) return 0;
  return emit_plane(b, f, v, frame_mask, f->sprdrawn,
                    f->have_bgpix ? f->bgpix : NULL, f->bgval, suppress, 1);
}

int ab_nes_mark_sprites(const ab_nes_frame *f, const ab_registry *reg,
                        int sprite_height, unsigned char *suppress,
                        const ab_sub_rule **out_rule, ab_nes_bounds *out_bounds) {
  if (!f || !reg || !suppress) return 0;
  if (sprite_height != 8 && sprite_height != 16) sprite_height = 8;

  int marked = 0;
  int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
  const ab_sub_rule *rule = NULL;

  for (int s = 0; s < 64; s++) {
    const unsigned char sy = f->oam[s * 4 + 0];
    if (sy >= 0xEF) continue;                    /* parked off-screen */
    const int tile = f->oam[s * 4 + 1];
    const ab_sub_rule *m = ab_registry_match_tile(reg, tile);
    if (!m) continue;
    rule = m;

    const int sx  = f->oam[s * 4 + 3];
    const int top = sy + 1;                      /* OAM y is one less */

    for (int yy = top; yy < top + sprite_height; yy++) {
      if (yy < 0 || yy >= AB_NES_LINES) continue;
      unsigned char *row = suppress + (size_t)yy * AB_NES_W;
      for (int xx = sx; xx < sx + 8; xx++)
        if (xx >= 0 && xx < AB_NES_W) row[xx] = 1;
    }
    marked++;

    /* Bounds come only from anchoring tiles: shadow/filler must be suppressed
     * but must not stretch the replacement art. */
    if (ab_registry_tile_anchors(m, tile)) {
      if (sx < x0) x0 = sx;
      if (top < y0) y0 = top;
      if (sx + 8 > x1) x1 = sx + 8;
      if (top + sprite_height > y1) y1 = top + sprite_height;
    }
  }

  if (out_rule) *out_rule = rule;
  if (out_bounds && x1 > x0) {
    out_bounds->x0 = x0; out_bounds->y0 = y0;
    out_bounds->x1 = x1; out_bounds->y1 = y1;
  } else if (out_bounds) {
    memset(out_bounds, 0, sizeof(*out_bounds));
  }
  return marked;
}
