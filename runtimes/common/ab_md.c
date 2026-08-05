/*
 * ab_md.c -- Genesis/SMS/GG profile. See ab_md.h for the facts this encodes.
 */
#include "ab_md.h"

#include <string.h>

/* Host ABI import (same pattern as ab_render.c: the test stub provides a
 * local definition, the wasm build imports from ab_host). */
#ifndef AB_IMPORT
#define AB_IMPORT(module, name) __attribute__((import_module(module), import_name(name)))
#endif
AB_IMPORT("ab_host", "region_read_u8") extern int32_t ab_region_read_u8(int32_t, int32_t);

int ab_md_frame_read(const ab_md_regions *r, ab_md_frame *f) {
  if (!r || !f || !f->bgpix || !f->objpix) return 0;
  if (r->linepix >= 0 && f->linepix)
    ab_region_slurp(r->linepix, 0, f->linepix, AB_MD_MAX_W * AB_MD_MAX_H);
  ab_region_slurp(r->bgpix,     0, f->bgpix,     AB_MD_MAX_W * AB_MD_MAX_H);
  ab_region_slurp(r->objpix,    0, f->objpix,    AB_MD_MAX_W * AB_MD_MAX_H * 2);
  ab_region_slurp(r->linestate, 0, f->linestate, AB_MD_MAX_H * 16);
  ab_region_slurp(r->pixrgb,    0, f->pixrgb,    512);
  f->have_pixlines = 0;
  if (r->pixlines >= 0 && f->pixlines) {
    ab_region_slurp(r->pixlines, 0, f->pixlines, AB_MD_MAX_H * 512);
    f->have_pixlines = 1;
  }
  return 1;
}

int ab_md_lines(const ab_md_frame *f) {
  if (!f) return 224;
  /* Captured viewport.h ([10..11], added for Mode 4: SMS renders 192 lines
   * and GG 144, neither derivable from reg1 bit3). Fall back to the Mode 5
   * rule when a pre-height capture reports zero. */
  const int h = f->linestate[10] | (f->linestate[11] << 8);
  if (h > 0 && h <= AB_MD_MAX_H) return h;
  return (f->linestate[1] & 0x08) ? 240 : 224;
}

int ab_md_line_width(const ab_md_frame *f, int y) {
  if (!f || y < 0 || y >= AB_MD_MAX_H) return 320;
  const unsigned char *ls = &f->linestate[y * 16];
  const int w = ls[6] | (ls[7] << 8);
  return (w > 0 && w <= AB_MD_MAX_W) ? w : 320;
}

static uint32_t md_rgba_from(const unsigned char *tbl, int code) {
  const int i = code & 0xFF;
  const unsigned v = tbl[i * 2] | (tbl[i * 2 + 1] << 8);
  const unsigned r5 = (v >> 11) & 0x1F;
  const unsigned g6 = (v >> 5) & 0x3F;
  const unsigned b5 = v & 0x1F;
  const unsigned r = (r5 << 3) | (r5 >> 2);
  const unsigned g = (g6 << 2) | (g6 >> 4);
  const unsigned b = (b5 << 3) | (b5 >> 2);
  return (r << 24) | (g << 16) | (b << 8) | 0xFFu;
}

uint32_t ab_md_rgba(const ab_md_frame *f, int code) {
  return md_rgba_from(f->pixrgb, code);
}

void ab_md_build_lut(const ab_md_frame *f, uint32_t out_lut[256]) {
  for (int i = 0; i < 256; i++) out_lut[i] = ab_md_rgba(f, i);
}

int ab_md_emit(ab_batch *b, const ab_md_frame *f, const ab_md_view *v,
               const unsigned char *suppress) {
  if (!b || !f || !v) return 0;

  uint32_t lut[256];
  const unsigned char *cur_tbl = NULL;

  const int lines = ab_md_lines(f);
  int quads = 0;

  for (int y = 0; y < lines; y++) {
    /* Raster palette effects rewrite CRAM mid-frame (the gradient-sky
     * idiom); use THIS line's captured pixel[] table, rebuilding the LUT
     * only when the table actually changed since the previous line. */
    const unsigned char *tbl = f->have_pixlines
                             ? &f->pixlines[(size_t)y * 512] : f->pixrgb;
    if (!cur_tbl || (tbl != f->pixrgb && memcmp(tbl, cur_tbl, 512) != 0)
        || (cur_tbl != tbl && tbl == f->pixrgb)) {
      for (int i = 0; i < 256; i++) lut[i] = md_rgba_from(tbl, i);
      cur_tbl = tbl;
    }
    const int width = ab_md_line_width(f, y);
    const int blankL = (f->linestate[y * 16] & 0x20) != 0;
    const unsigned char *lpx = f->linepix ? &f->linepix[y * AB_MD_MAX_W] : NULL;
    const unsigned char *bg = &f->bgpix[y * AB_MD_MAX_W];
    const unsigned char *ob = &f->objpix[(size_t)y * AB_MD_MAX_W * 2];
    const unsigned char *sup = suppress ? &suppress[y * AB_MD_MAX_W] : NULL;

    /* Compose base: linepix (S/H state resolved). A pixel whose sprite is
     * suppressed falls back to the pre-merge BG code. Without linepix
     * (older capture), the pre-S/H compose is the best available. */
    int x = 0;
    while (x < width) {
      int code;
      if (sup && sup[x] && ob[x * 2 + 1]) code = bg[x];
      else if (lpx) code = lpx[x];
      else if (blankL && x < 8) code = AB_MD_LEFT_BLANK_CODE;
      else if (ob[x * 2 + 1]) code = ob[x * 2];
      else code = bg[x];

      const int x0 = x;
      while (x + 1 < width) {
        const int nx = x + 1;
        int ncode;
        if (sup && sup[nx] && ob[nx * 2 + 1]) ncode = bg[nx];
        else if (lpx) ncode = lpx[nx];
        else if (blankL && nx < 8) ncode = AB_MD_LEFT_BLANK_CODE;
        else if (ob[nx * 2 + 1]) ncode = ob[nx * 2];
        else ncode = bg[nx];
        if (ncode != code) break;
        x++;
      }

      ab_batch_solid(b,
                     v->ox + x0 * v->scale, v->oy + y * v->scale,
                     (x - x0 + 1) * v->scale, v->scale,
                     lut[code & 0xFF]);
      quads++;
      x++;
    }
  }
  return quads;
}

/* VRAM u16 with gpgx's word-byte swap: byte addr XOR 1. */
static unsigned md_vram_u16(int32_t vram, int addr) {
  const int a = addr & 0xFFFF;
  return (unsigned)(ab_region_read_u8(vram, a ^ 1))
       | ((unsigned)(ab_region_read_u8(vram, a ^ 0)) << 8);
}

int ab_md_mark_sprites(const ab_md_regions *r, const ab_md_frame *f,
                       const ab_registry *reg, unsigned char *suppress,
                       const ab_sub_rule **out_rule, ab_md_bounds *out_bounds) {
  if (!r || !f || !reg || !suppress) return 0;
  if (r->vram < 0 || r->vdpregs < 0) return 0;

  const int h40 = (f->linestate[3] & 0x01) != 0;   /* reg12 bit0 */
  const int lines = ab_md_lines(f);
  const int width = h40 ? 320 : 256;
  /* SAT base: reg5. H40 masks bit0 off (hardware ignores it). */
  int reg5 = ab_region_read_u8(r->vdpregs, 5);
  const int sat = (h40 ? (reg5 & 0x7E) : (reg5 & 0x7F)) << 9;
  const int max_spr = h40 ? 80 : 64;

  int marked = 0;
  int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -(1 << 30), by1 = -(1 << 30);
  const ab_sub_rule *rule = NULL;

  /* The SAT is a linked list: entry 3's low 7 bits = next index, 0 ends. */
  int idx = 0;
  for (int n = 0; n < max_spr; n++) {
    const int base = sat + idx * 8;
    const unsigned ypos = md_vram_u16(r->vram, base) & 0x3FF;
    const unsigned size = md_vram_u16(r->vram, base + 2);
    const unsigned attr = md_vram_u16(r->vram, base + 4);
    const unsigned xpos = md_vram_u16(r->vram, base + 6) & 0x3FF;
    /* SAT word 1: bits 0-6 link, bits 8-9 height cells-1, 10-11 width. */
    const int link = size & 0x7F;
    const int wcells = ((size >> 10) & 3) + 1;
    const int hcells = ((size >> 8) & 3) + 1;
    const int tile = attr & 0x7FF;

    const int sy = (int)ypos - 128;
    const int sx = (int)xpos - 128;

    const ab_sub_rule *m = ab_registry_match_tile(reg, tile & 0xFF);
    if (m && sy > -64 && sy < lines && sx > -64 && sx < width) {
      rule = m;
      for (int yy = sy; yy < sy + hcells * 8; yy++) {
        if (yy < 0 || yy >= lines) continue;
        unsigned char *row = &suppress[yy * AB_MD_MAX_W];
        for (int xx = sx; xx < sx + wcells * 8; xx++)
          if (xx >= 0 && xx < width) row[xx] = 1;
      }
      marked++;
      if (ab_registry_tile_anchors(m, tile & 0xFF)) {
        if (sx < bx0) bx0 = sx;
        if (sy < by0) by0 = sy;
        if (sx + wcells * 8 > bx1) bx1 = sx + wcells * 8;
        if (sy + hcells * 8 > by1) by1 = sy + hcells * 8;
      }
    }

    if (link == 0) break;
    idx = link;
  }

  if (out_rule) *out_rule = rule;
  if (out_bounds && bx1 > bx0) {
    out_bounds->x0 = bx0; out_bounds->y0 = by0;
    out_bounds->x1 = bx1; out_bounds->y1 = by1;
  } else if (out_bounds) {
    memset(out_bounds, 0, sizeof(*out_bounds));
  }
  return marked;
}
