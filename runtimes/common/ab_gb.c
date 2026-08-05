/*
 * ab_gb.c -- GB/GBC profile. See ab_gb.h for the facts this encodes.
 */
#include "ab_gb.h"

#include <string.h>

int ab_gb_frame_read(const ab_gb_regions *r, ab_gb_frame *f) {
  if (!r || !f || !f->bgpix || !f->sprpix) return 0;

  ab_region_slurp(r->lineregs, 0, f->lineregs, AB_GB_H * AB_GB_LR_STRIDE);
  ab_region_slurp(r->bgpix,    0, f->bgpix,    AB_GB_PIX);
  ab_region_slurp(r->sprpix,   0, f->sprpix,   AB_GB_PIX);
  ab_region_slurp(r->palline,  0, f->palline,  AB_GB_H * AB_GB_PAL_STRIDE);

  /* The per-pixel colour planes are what make a mid-LINE palette write land
   * on the right pixels; per-line capture rounds it onto the whole line. */
  /* NOTE `>= 0`, not truthiness: an absent region is -1, which is TRUE in C.
   * Testing `r->bgcol15 &&` silently accepted missing planes and read
   * garbage into them. */
  f->have_percolour = 0;
  if (r->bgcol15 >= 0 && r->sprcol15 >= 0 && f->bgcol15 && f->sprcol15) {
    ab_region_slurp(r->bgcol15,  0, f->bgcol15,  AB_GB_PIX * 2);
    ab_region_slurp(r->sprcol15, 0, f->sprcol15, AB_GB_PIX * 2);
    f->have_percolour = 1;
  }
  f->have_oam = 0;
  if (r->oam >= 0) {
    ab_region_slurp(r->oam, 0, f->oam, 160);
    f->have_oam = 1;
  }
  return 1;
}

uint16_t ab_gb_pal_entry(const ab_gb_frame *f, int line, int obj, int idx) {
  if (!f) return 0;
  if (line < 0) line = 0;
  if (line >= AB_GB_H) line = AB_GB_H - 1;
  if (idx < 0) idx = 0;
  if (idx > 31) idx = 31;
  const size_t o = (size_t)line * AB_GB_PAL_STRIDE + (obj ? 64 : 0) + (size_t)idx * 2;
  return (uint16_t)(f->palline[o] | ((uint16_t)f->palline[o + 1] << 8));
}

uint32_t ab_gb_rgba565(uint16_t c565) {
  /* Widen by replicating the high bits. NOTE the field widths: gambatte packs
   * a 5-bit green at <<6, so the 6-bit green field's low bit is always 0 --
   * this expansion must match how the host presents the core's framebuffer,
   * which the 1396-cart exact run pins. */
  const uint32_t r = (c565 >> 11) & 0x1F;
  const uint32_t g = (c565 >> 5) & 0x3F;
  const uint32_t b = c565 & 0x1F;
  const uint32_t r8 = (r << 3) | (r >> 2);
  const uint32_t g8 = (g << 2) | (g >> 4);
  const uint32_t b8 = (b << 3) | (b >> 2);
  return (r8 << 24) | (g8 << 16) | (b8 << 8) | 0xFFu;
}

/* Colour of one pixel, preferring the per-pixel resolved plane.
 *
 * The valid bit decides: 0 in a colour plane is a legal colour (black), not a
 * "no data" sentinel, so an unwritten pixel must fall back to the per-line
 * table rather than render black. */
static uint32_t pix_colour(const ab_gb_frame *f, int y, int i,
                           unsigned char bp, unsigned char sp,
                           int use_spr, int cgb) {
  if (use_spr) {
    if (f->have_percolour) {
      const size_t o = (size_t)i * 2;
      return ab_gb_rgba565((uint16_t)(f->sprcol15[o] | ((uint16_t)f->sprcol15[o + 1] << 8)));
    }
    const int pal = (sp & AB_GB_PIX_PAL) >> 2;
    return ab_gb_rgba565(ab_gb_pal_entry(f, y, 1, pal * 4 + (sp & AB_GB_PIX_ENTRY)));
  }
  if (f->have_percolour && (bp & AB_GB_PIX_VALID)) {
    const size_t o = (size_t)i * 2;
    return ab_gb_rgba565((uint16_t)(f->bgcol15[o] | ((uint16_t)f->bgcol15[o + 1] << 8)));
  }
  /* Palette INDEXING differs by mode: CGB selects one of 8 four-entry
   * palettes; DMG has a single BG palette (and OBP0/OBP1 for sprites, which
   * the capture already encoded as palette 0/1). */
  const int e = bp & AB_GB_PIX_ENTRY;
  const int idx = cgb ? (((bp & AB_GB_PIX_PAL) >> 2) * 4 + e) : e;
  return ab_gb_rgba565(ab_gb_pal_entry(f, y, 0, idx));
}

/* Emit run-coalesced quads for one layer. Runs are long on real frames (flat
 * colour spans), which keeps the quad count far below the pixel count. */
static int emit_plane(ab_batch *b, const ab_gb_frame *f, const ab_gb_view *v,
                      int sprite_layer, const unsigned char *suppress) {
  if (!b || !f || !v) return 0;

  int quads = 0;

  for (int y = 0; y < AB_GB_H; y++) {
    const int cgb   = ab_gb_line_cgb(f, y);
    const int mprio = ab_gb_line_master_prio(f, y);
    const int blank = ab_gb_line_blank(f, y);

    if (blank) {
      /* Whole-frame solid fill (LCD off): one colour, parked by the capture
       * in palline entry 0. The sprite layer contributes nothing. */
      if (!sprite_layer) {
        ab_batch_solid(b, v->ox, v->oy + y * v->scale,
                       AB_GB_W * v->scale, v->scale,
                       ab_gb_rgba565(ab_gb_pal_entry(f, y, 0, 0)));
        quads++;
      }
      continue;
    }

    const size_t row = (size_t)y * AB_GB_W;
    const unsigned char *srow = suppress ? suppress + row : NULL;

    int x = 0;
    while (x < AB_GB_W) {
      const int i = (int)row + x;
      const unsigned char bp = f->bgpix[i];
      const unsigned char sp = f->sprpix[i];
      const int spr_wins = ab_gb_spr_visible(bp, sp, cgb, mprio);

      int draw;
      if (sprite_layer) {
        draw = spr_wins;
        if (draw && srow && srow[x]) draw = 0;   /* replaced by HD art */
      } else {
        /* The BG layer draws EVERY pixel, including under sprites: the
         * sprite layer composites on top, and a sprite that is later
         * suppressed for replacement art must reveal background, not a hole.
         * This is why the BG pass does not consult the priority rule. */
        draw = 1;
      }
      if (!draw) { x++; continue; }

      const uint32_t rgba = pix_colour(f, y, i, bp, sp, sprite_layer, cgb);
      const int x0 = x;

      /* Extend the run while both the colour AND the draw decision hold. */
      while (x + 1 < AB_GB_W) {
        const int j = (int)row + x + 1;
        const unsigned char nbp = f->bgpix[j];
        const unsigned char nsp = f->sprpix[j];
        const int nwins = ab_gb_spr_visible(nbp, nsp, cgb, mprio);
        if (sprite_layer) {
          if (!nwins) break;
          if (srow && srow[x + 1]) break;
        }
        if (pix_colour(f, y, j, nbp, nsp, sprite_layer, cgb) != rgba) break;
        x++;
      }

      ab_batch_solid(b,
                     v->ox + x0 * v->scale, v->oy + y * v->scale,
                     (x - x0 + 1) * v->scale, v->scale,
                     rgba);
      quads++;
      x++;
    }
  }
  return quads;
}

int ab_gb_emit_background(ab_batch *b, const ab_gb_frame *f,
                          const ab_gb_view *v) {
  return emit_plane(b, f, v, 0, NULL);
}

int ab_gb_emit_sprites(ab_batch *b, const ab_gb_frame *f,
                       const ab_gb_view *v, const unsigned char *suppress) {
  return emit_plane(b, f, v, 1, suppress);
}

int ab_gb_mark_sprites(const ab_gb_frame *f, const ab_registry *reg,
                       int sprite_height, unsigned char *suppress,
                       const ab_sub_rule **out_rule, ab_gb_bounds *out_bounds) {
  if (!f || !reg || !suppress || !f->have_oam) return 0;
  if (sprite_height != 8 && sprite_height != 16) sprite_height = 8;

  int marked = 0;
  int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
  const ab_sub_rule *rule = NULL;

  for (int s = 0; s < 40; s++) {
    const int oy = f->oam[s * 4 + 0];
    const int ox = f->oam[s * 4 + 1];
    /* GB OAM: screen y = oam_y - 16, screen x = oam_x - 8. An object fully
     * off-screen parks at y 0 or >= 160. NOT the NES "y is one less" rule. */
    if (oy == 0 || oy >= 160) continue;
    int tile = f->oam[s * 4 + 2];
    /* In 8x16 mode the hardware ignores the tile id's low bit. */
    if (sprite_height == 16) tile &= 0xFE;

    const ab_sub_rule *m = ab_registry_match_tile(reg, tile);
    if (!m) continue;
    rule = m;

    const int sx  = ox - 8;
    const int top = oy - 16;

    for (int yy = top; yy < top + sprite_height; yy++) {
      if (yy < 0 || yy >= AB_GB_H) continue;
      unsigned char *r = suppress + (size_t)yy * AB_GB_W;
      for (int xx = sx; xx < sx + 8; xx++)
        if (xx >= 0 && xx < AB_GB_W) r[xx] = 1;
    }
    marked++;

    /* Bounds come only from anchoring tiles: shadow/filler tiles must be
     * suppressed but must not stretch the replacement art. */
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
