/*
 * ab_pce.c -- PC Engine / TurboGrafx-16 profile. See ab_pce.h for the facts
 * this encodes.
 *
 * The line renderer below is a transcription of geargrafx's
 * HuC6270::RenderBackground / FetchSprites / RenderSprites. Structure and
 * variable roles are kept close to the original ON PURPOSE: when the core is
 * updated, a diff against huc6270.cpp should be readable.
 */
#include "ab_pce.h"

/* Test-only: ignore the core's resolved line buffer and re-run the line
 * renderer instead. Lets a control prove the linepix fast path is what is
 * actually being exercised, and lets a diagnosis compare the two paths. */
int ab_pce_test_no_linepix = 0;


#include <string.h>
#ifdef AB_PCE_BIAS_OVERRIDE
#include <stdlib.h>
#endif

/* huc6270.h lookup tables. Indexed by (MWR >> 4) & 7 -- note entries 2 and 3
 * are both 128 wide, and the top half of the table is the 64-tall variant. */
static const int k_screen_x[8]      = { 32, 64, 128, 128, 32, 64, 128, 128 };
static const int k_screen_x_mask[8] = { 32*8-1, 64*8-1, 128*8-1, 128*8-1,
                                        32*8-1, 64*8-1, 128*8-1, 128*8-1 };
static const int k_screen_y_mask[8] = { 32*8-1, 32*8-1, 32*8-1, 32*8-1,
                                        64*8-1, 64*8-1, 64*8-1, 64*8-1 };

/* Sprite geometry, keyed by the CGX (width) and CGY (height) flag fields.
 * The mask tables force the pattern number to a multiple of the cell count a
 * larger sprite occupies -- the hardware ignores those low bits rather than
 * wrapping, which is why they are AND masks and not divisions. */
static const int k_spr_w[2]      = { 16, 32 };
static const int k_spr_h[4]      = { 16, 32, 64, 64 };
static const int k_spr_mask_w[2] = { 0xFFFF, 0xFFFE };
static const int k_spr_mask_h[4] = { 0xFFFF, 0xFFFD, 0xFFF9, 0xFFF9 };

int ab_pce_frame_read(const ab_pce_regions *r, ab_pce_frame *f) {
  if (!r || !f || !f->vram) return 0;
  /* NOTE `< 0`, not truthiness: an absent region is -1, which is TRUE in C.
   * The GB profile learned this the hard way (see ab_gb.c). */
  if (r->vram < 0 || r->satb < 0 || r->regs < 0 || r->palette < 0) return 0;

  ab_region_slurp(r->vram,    0, f->vram, AB_PCE_VRAM_WORDS * 2);
  ab_region_slurp(r->satb,    0, f->sat,  AB_PCE_SAT_WORDS * 2);
  ab_region_slurp(r->regs,    0, f->regs, AB_PCE_REGS * 2);
  ab_region_slurp(r->palette, 0, f->pal,  AB_PCE_PAL_ENTRIES * 2);

  /* pce_vdc_reglines is OPTIONAL -- active-bezels ships against romdev,
   * romdeck and retroemu, and only a freshly patched geargrafx has it. An
   * absent region must degrade to the frame-end registers, not fail the
   * frame. Zero the buffer too: a partial slurp must not leave the previous
   * frame's scroll values readable as if they were this frame's. */
  f->has_reglines = 0;
  memset(f->reglines, 0, sizeof(f->reglines));
  if (r->reglines >= 0) {
    const int want = AB_PCE_REGLINES * AB_PCE_REGLINE_SIZE;
    if (ab_region_slurp(r->reglines, 0, f->reglines, want) == want)
      f->has_reglines = 1;
    else
      memset(f->reglines, 0, sizeof(f->reglines));
  }

  /* Per-line palette, same optional contract as reglines. The buffer is
   * caller-owned: a consumer that never needs mid-frame recolour leaves
   * f->pallines NULL and this is skipped entirely. */
  f->has_pallines = 0;
  if (r->pallines >= 0 && f->pallines) {
    const int want = AB_PCE_PALLINES * AB_PCE_PALLINE_SIZE;
    if (ab_region_slurp(r->pallines, 0, f->pallines, want) == want)
      f->has_pallines = 1;
    else
      memset(f->pallines, 0, (size_t)want);
  }
  f->has_xofflines = 0;
  memset(f->xofflines, 0, sizeof(f->xofflines));
  if (r->xofflines >= 0) {
    const int want = AB_PCE_XOFFLINES * 2;
    if (ab_region_slurp(r->xofflines, 0, f->xofflines, want) == want)
      f->has_xofflines = 1;
    else
      memset(f->xofflines, 0, sizeof(f->xofflines));
  }
  f->has_srclines = 0;
  memset(f->srclines, 0, sizeof(f->srclines));
  if (r->srclines >= 0) {
    const int want = AB_PCE_XOFFLINES * 2;
    if (ab_region_slurp(r->srclines, 0, f->srclines, want) == want)
      f->has_srclines = 1;
    else
      memset(f->srclines, 0, sizeof(f->srclines));
  }
  /* Dot-stamped palette write log. Reject a truncated log WHOLE: undoing a
   * partial log rebuilds a line-start table that never existed. count==0 is
   * fine (has_paldeltas stays 1 with nothing to split -- the normal case). */
  f->has_paldeltas = 0;
  if (r->paldeltas >= 0 && f->paldeltas) {
    const int want = AB_PCE_PALDELTAS_SIZE;
    if (ab_region_slurp(r->paldeltas, 0, f->paldeltas, want) == want) {
      const int count = f->paldeltas[0] | (f->paldeltas[1] << 8);
      const int truncated = f->paldeltas[2];
      if (!truncated && count <= AB_PCE_PALDELTA_CAP)
        f->has_paldeltas = 1;
    } else {
      memset(f->paldeltas, 0, (size_t)want);
    }
  }
  f->has_linepix = 0;
  if (r->linepix >= 0 && f->linepix) {
    const int want = AB_PCE_LINEPIX_LINES * AB_PCE_LINEPIX_SIZE;
    if (ab_region_slurp(r->linepix, 0, f->linepix, want) == want)
      f->has_linepix = 1;
    else
      memset(f->linepix, 0, (size_t)want);
  }
  return 1;
}

/* The colour table as of VCE scanline `vpos`. A slot the VCE never wrote this
 * frame is all-zero; an all-zero table is not a legitimate palette (entry 0
 * would be black AND every other entry black too), so it is treated as absent
 * and the frame-end table is used. */
int ab_pce_test_pal_vpos_delta = 0;   /* diagnostic sweep hook */
int ab_pce_test_no_paldeltas = 0;     /* ignore the dot-stamped palette log */
/* Diagnostic: restrict the delta to ONE framebuffer row, so a sweep can ask
 * "which vpos does row N's palette actually match?" without disturbing the
 * rest of the frame. -1 = apply to every row. */
int ab_pce_test_pal_delta_row = -1;
int ab_pce_test_cur_row = -1;

static const unsigned char *pal_for_vpos(const ab_pce_frame *f, int vpos) {
  if (ab_pce_test_pal_delta_row < 0 ||
      ab_pce_test_cur_row == ab_pce_test_pal_delta_row)
    vpos += ab_pce_test_pal_vpos_delta;
  if (!f->has_pallines || !f->pallines) return f->pal;
  if (vpos < 0 || vpos >= AB_PCE_PALLINES) return f->pal;
  const unsigned char *p = f->pallines + (size_t)vpos * AB_PCE_PALLINE_SIZE;
  for (int i = 0; i < AB_PCE_PALLINE_SIZE; i++)
    if (p[i]) return p;
  return f->pal;
}

/* The framebuffer-row -> VDC-raster-line bias. See the header's FACTS block:
 * it is MEASURED, and AB_PCE_RASTER_BIAS is the value every capture agrees on.
 * The hook exists so the bias can be swept from a harness without rebuilding,
 * which is how it was measured in the first place; production callers get the
 * constant. */
int ab_pce_raster_bias(const ab_pce_frame *f) {
  (void)f;
#ifdef AB_PCE_BIAS_OVERRIDE
  {
    const char *e = getenv("RASTER_BIAS");
    if (e && *e) return atoi(e);
  }
#endif
  return AB_PCE_RASTER_BIAS;
}

int ab_pce_regline_at(const ab_pce_frame *f, int raster, ab_pce_regline *out) {
  if (!out) return 0;
  memset(out, 0, sizeof(*out));
  if (!f || !f->has_reglines) return 0;
  if (raster < 0 || raster >= AB_PCE_REGLINES) return 0;

  const unsigned char *p = f->reglines + (size_t)raster * AB_PCE_REGLINE_SIZE;
  /* Byte 12 is `valid`. A slot the VDC never rendered this frame is zeroed,
   * and zero is a LEGITIMATE scroll value, so the flag is the only way to
   * tell "line 200 scrolled to 0" from "line 200 never ran". */
  if (!p[12]) return 0;

  out->bxr    = ab_pce_u16(p, 0);
  out->bgy    = ab_pce_u16(p, 1);
  out->cr     = ab_pce_u16(p, 2);
  out->mwr    = ab_pce_u16(p, 3);
  out->hdw    = ab_pce_u16(p, 4);
  out->raster = ab_pce_u16(p, 5);
  /* Word 7 (bytes 14-15) is the VCE scanline. It replaced a pad field, so the
   * record is still 16 bytes and the region id is unchanged -- an older core
   * writes 0 here, which ab_pce_regline_for_row treats as "no mapping" and
   * falls back to the constant bias. */
  out->vpos   = ab_pce_u16(p, 7);
  out->valid  = 1;
  out->burst  = p[13] ? 1 : 0;
  return 1;
}

/* Does this capture's reglines carry the vpos field? An older core zeroed the
 * word (it was padding), and vpos 0 is not a value the VDC can legitimately
 * record for a rendered line -- the display window cannot start before the
 * VCE's own scanline 14. So "some valid record has a non-zero vpos" cleanly
 * separates a new core from an old one, and an old one keeps the previous
 * constant-bias behaviour instead of blanking the whole frame. */
int ab_pce_reglines_have_vpos(const ab_pce_frame *f) {
  if (!f || !f->has_reglines) return 0;
  for (int r = 0; r < AB_PCE_REGLINES; r++) {
    const unsigned char *p = f->reglines + (size_t)r * AB_PCE_REGLINE_SIZE;
    if (p[12] && ab_pce_u16(p, 7) != 0) return 1;
  }
  return 0;
}

int ab_pce_regline_for_row(const ab_pce_frame *f, int fb_row, int fb_height,
                           ab_pce_regline *out) {
  if (!out) return 0;
  memset(out, 0, sizeof(*out));
  if (!f || !f->has_reglines) return 0;

  const int want_vpos = fb_row + ab_pce_vpos_origin(fb_height);
  /* Linear scan of 263 records. The table is tiny and the alternative -- an
   * inverted index rebuilt per frame -- costs more than it saves, because a
   * caller renders each row exactly once.
   *
   * SCANNED BACKWARDS SO THE **LAST** WRITER WINS, and that is load-bearing:
   * a vpos can legitimately be claimed by more than one raster line. The
   * raster counter resets whenever a new display window starts, so a game
   * that restarts VDW mid-frame (one shooter does, at raster 133) leaves the
   * PREVIOUS frame's low-numbered records still sitting in the table with the
   * same vpos as this frame's real lines. Both are `valid`; only the higher
   * raster actually rendered into the line buffer for that scanline. Taking
   * the first match painted a six-row band with the wrong scroll -- 98.31% on
   * every frame of that cart, always the same 969 pixels. */
  for (int r = AB_PCE_REGLINES - 1; r >= 0; r--) {
    const unsigned char *p = f->reglines + (size_t)r * AB_PCE_REGLINE_SIZE;
    if (!p[12]) continue;
    if ((int)ab_pce_u16(p, 7) != want_vpos) continue;
    return ab_pce_regline_at(f, r, out);
  }
  return 0;
}

static uint32_t pce_rgba_from(const unsigned char *tbl, int pal_index) {
  const uint16_t c = ab_pce_u16(tbl, pal_index & 0x1FF);
  /* 9-bit GGGRRRBBB -- green occupies the HIGH bits. Quantise to 5/6/5 the
   * way huc6260.cpp InitPalettes builds m_rgb565_palette, then widen back the
   * way the host presents RGB565. Both halves matter: the quantisation is
   * what makes green land on the 6-bit ladder. */
  uint32_t g3 = (c >> 6) & 7, r3 = (c >> 3) & 7;
#ifdef AB_PCE_BIAS_OVERRIDE
  if (getenv("CTL_GRB")) { uint32_t t = g3; g3 = r3; r3 = t; }  /* control */
#endif
  const uint32_t g6 = (uint32_t)(g3 * 63 / 7);
  const uint32_t r5 = (uint32_t)(r3 * 31 / 7);
  const uint32_t b5 = (uint32_t)((c & 7) * 31 / 7);
  const uint32_t r8 = (r5 << 3) | (r5 >> 2);
  const uint32_t g8 = (g6 << 2) | (g6 >> 4);
  const uint32_t b8 = (b5 << 3) | (b5 >> 2);
  return (r8 << 24) | (g8 << 16) | (b8 << 8) | 0xFFu;
}

uint32_t ab_pce_rgba(const ab_pce_frame *f, int pal_index) {
  if (!f) return 0x000000FFu;
  return pce_rgba_from(f->pal, pal_index);
}

void ab_pce_build_lut_at(const ab_pce_frame *f, int vpos,
                         uint32_t out[AB_PCE_PAL_ENTRIES]) {
  if (!f || !out) return;
  const unsigned char *tbl = pal_for_vpos(f, vpos);
  for (int i = 0; i < AB_PCE_PAL_ENTRIES; i++) out[i] = pce_rgba_from(tbl, i);
}

int ab_pce_pal_events_for_row(const ab_pce_frame *f, int vpos,
                              ab_pce_pal_ev *ev, int max) {
  if (!f || !f->has_paldeltas || !f->paldeltas || !ev || max <= 0) return 0;
  const int count = f->paldeltas[0] | (f->paldeltas[1] << 8);
  int n = 0;
  for (int i = 0; i < count; i++) {
    const unsigned char *e = f->paldeltas + AB_PCE_PALDELTA_HDR
                           + (size_t)i * AB_PCE_PALDELTA_ENTRY;
    const int evpos = e[0] | (e[1] << 8);
    if (evpos != vpos) continue;
    if (n >= max) break;   /* keep the earliest; later ones are already in
                              pallines[vpos], losing a split is a visible
                              (scoreable) degradation, not a crash */
    ev[n].dot   = e[4] | (e[5] << 8);
    ev[n].index = e[6] | (e[7] << 8);
    ev[n].oldv  = (uint16_t)(e[8] | (e[9] << 8));
    ev[n].newv  = (uint16_t)(e[10] | (e[11] << 8));
    n++;
  }
  return n;
}

void ab_pce_lut_set(uint32_t lut[AB_PCE_PAL_ENTRIES], int index, uint16_t raw) {
  if (!lut) return;
  unsigned char tmp[2] = { (unsigned char)(raw & 0xFF),
                           (unsigned char)(raw >> 8) };
  /* pce_rgba_from reads entry `pal_index` from a table pointer; feed it a
   * one-entry table by biasing the pointer so index 0 lands on tmp. */
  lut[index & 0x1FF] = pce_rgba_from(tmp, 0);
}

/* Resolve a line-buffer code to RGBA, honouring the black bypass. Callers
 * that index the LUT directly must route through this, or a burst line comes
 * out as the backdrop colour. */
uint32_t ab_pce_resolve(const uint32_t lut[AB_PCE_PAL_ENTRIES], uint16_t code) {
  if (code & AB_PCE_LB_BLACK) return 0x000000FFu;
  return lut[code & 0x1FF];
}

void ab_pce_build_lut(const ab_pce_frame *f, uint32_t out[AB_PCE_PAL_ENTRIES]) {
  if (!f || !out) return;
  for (int i = 0; i < AB_PCE_PAL_ENTRIES; i++) out[i] = ab_pce_rgba(f, i);
}

/* --- background ----------------------------------------------------------
 * HuC6270::RenderBackground. The BAT is a flat row-major u16 array at VRAM
 * word 0; each entry is (colour_table << 12) | tile_index. Tile data lives at
 * tile_index << 4 -- sixteen words per 8x8 tile, planes 0/1 packed in the low
 * and high bytes of the first eight words and planes 2/3 in the next eight.
 */
static void render_background(const ab_pce_frame *f, int width, int bg_y,
                              int bxr, int mwr, uint16_t *line) {
  const int scr = (mwr >> 4) & 7;
  const int ssx = k_screen_x[scr];

  bg_y &= k_screen_y_mask[scr];
  const int tile_y = bg_y & 7;
  const int bat_off = (bg_y >> 3) * ssx;

  int prev_col = -1;
  unsigned char b1 = 0, b2 = 0, b3 = 0, b4 = 0;
  int colour_table = 0;

  for (int i = 0; i < width; i++) {
    const int bg_x = (bxr + i) & k_screen_x_mask[scr];
    const int col = bg_x >> 3;

    /* Refetch only when crossing a tile boundary -- the core does the same,
     * and it is what keeps this loop cheap enough to run every line. */
    if (col != prev_col) {
      const uint16_t entry = ab_pce_vram(f, bat_off + col);
      const int tile_index = entry & 0x07FF;
      colour_table = (entry >> 12) & 0x0F;
      const int base = tile_index << 4;
      const uint16_t wa = ab_pce_vram(f, base + tile_y);
      const uint16_t wb = ab_pce_vram(f, base + tile_y + 8);
      b1 = (unsigned char)(wa & 0xFF);
      b2 = (unsigned char)(wa >> 8);
      b3 = (unsigned char)(wb & 0xFF);
      b4 = (unsigned char)(wb >> 8);
      prev_col = col;
    }

    const int tx = 7 - (bg_x & 7);
    line[i] = (uint16_t)((colour_table << 4)
              | ((b1 >> tx) & 1)
              | (((b2 >> tx) & 1) << 1)
              | (((b3 >> tx) & 1) << 2)
              | (((b4 >> tx) & 1) << 3));
  }
}

/* --- sprites -------------------------------------------------------------
 * HuC6270::FetchSprites collects the sprites intersecting this raster line,
 * in SATB order, honouring the 16-per-line cap. A 32-wide sprite is emitted
 * as TWO 16-wide halves that each consume a slot.
 */
typedef struct {
  int      index;      /* SATB index, for the collision/priority rules */
  int      x;          /* raw SATB x, still biased by 0x20 */
  uint16_t flags;
  unsigned char palette;   /* already shifted into bits 4-7 */
  uint16_t data[4];        /* the four bitplanes for this line */
} pce_sprite;

static int fetch_sprites(const ab_pce_frame *f, int raster, int mwr,
                         pce_sprite *out, int max_out) {
  int count = 0;
  const int mode1 = ((mwr >> 2) & 3) == 1;

  for (int i = 0; i < AB_PCE_SPRITES; i++) {
    const int o = i << 2;
    const uint16_t sat0 = ab_pce_u16(f->sat, o + 0);
    const uint16_t sat3 = ab_pce_u16(f->sat, o + 3);
    const int sprite_y = (int)(sat0 & 0x3FF) - 64;
    const int cgy = (sat3 >> 12) & 3;
    const int h = k_spr_h[cgy];

    if (!(sprite_y <= raster && (sprite_y + h) > raster)) continue;
    int y = raster - sprite_y;
    if (y >= h) continue;

    if (count >= AB_PCE_SPR_PER_LINE) break;

    const uint16_t sat1 = ab_pce_u16(f->sat, o + 1);
    const uint16_t sat2 = ab_pce_u16(f->sat, o + 2);
    /* Mode 1 halves the pattern depth (two planes) and steals sat2 bit 0 as
     * a half-cell selector. */
    const int mode1_off = mode1 ? ((sat2 & 1) << 5) : 0;
    const int cgx = (sat3 >> 8) & 1;
    const int w = k_spr_w[cgx];
    int pattern = (sat2 >> 1) & 0x3FF;
    pattern &= k_spr_mask_w[cgx];
    pattern &= k_spr_mask_h[cgy];

    const int addr = pattern << 6;
    const unsigned char palette = (unsigned char)((sat3 & 0x0F) << 4);
    const int xflip = (sat3 & AB_PCE_SPR_XFLIP) != 0;

    if (sat3 & AB_PCE_SPR_YFLIP) y = h - 1 - y;

    /* A tall sprite is a column of 16-line cells, 128 words apart. */
    const int tile_line_off = (y >> 4) * 128;
    const int offset_y = y & 0xF;
    const int line_start = addr + tile_line_off + offset_y + mode1_off;

    if (w == 16) {
      pce_sprite *s = &out[count++];
      s->index = i; s->x = (int)(sat1 & 0x3FF);
      s->flags = sat3; s->palette = palette;
      s->data[0] = ab_pce_vram(f, line_start + 0);
      s->data[1] = ab_pce_vram(f, line_start + 16);
      s->data[2] = mode1 ? 0 : ab_pce_vram(f, line_start + 32);
      s->data[3] = mode1 ? 0 : ab_pce_vram(f, line_start + 48);
    } else {
      /* 32 wide: two cells 64 words apart. X-flip swaps WHICH CELL feeds the
       * left half -- it is not just a pixel mirror. */
      int line = line_start + (xflip ? 64 : 0);
      pce_sprite *s = &out[count++];
      s->index = i; s->x = (int)(sat1 & 0x3FF);
      s->flags = sat3; s->palette = palette;
      s->data[0] = ab_pce_vram(f, line + 0);
      s->data[1] = ab_pce_vram(f, line + 16);
      s->data[2] = mode1 ? 0 : ab_pce_vram(f, line + 32);
      s->data[3] = mode1 ? 0 : ab_pce_vram(f, line + 48);

      if (count >= AB_PCE_SPR_PER_LINE || count >= max_out) break;

      line = line_start + (xflip ? 0 : 64);
      s = &out[count++];
      s->index = i; s->x = (int)(sat1 & 0x3FF) + 16;
      s->flags = sat3; s->palette = palette;
      s->data[0] = ab_pce_vram(f, line + 0);
      s->data[1] = ab_pce_vram(f, line + 16);
      s->data[2] = mode1 ? 0 : ab_pce_vram(f, line + 32);
      s->data[3] = mode1 ? 0 : ab_pce_vram(f, line + 48);
    }

    if (count >= AB_PCE_SPR_PER_LINE || count >= max_out) break;
  }
  return count;
}

/* HuC6270::RenderSprites. Two buffers, back to front, and only the pixels
 * tagged AB_PCE_LB_INFRONT are copied over the background -- see the header
 * note on why the second buffer is load-bearing. */
static void render_sprites(const ab_pce_frame *f, int width,
                           const pce_sprite *sprites, int count,
                           uint16_t *line, uint16_t *sprite_line) {
  (void)f;
  memset(sprite_line, 0, (size_t)width * sizeof(*sprite_line));

  for (int i = count - 1; i >= 0; i--) {
    const pce_sprite *s = &sprites[i];
    const int pos = s->x - 0x20;
    if ((pos + 15) < 0 || pos >= width) continue;

    const int priority = (s->flags & AB_PCE_SPR_PRIORITY) != 0;
    const uint16_t p1 = s->data[0], p2 = s->data[1];
    const uint16_t p3 = s->data[2], p4 = s->data[3];

    const int start_x = (pos < 0) ? -pos : 0;
    const int end_x = (pos + 15 >= width) ? (width - pos - 1) : 15;

    for (int x = start_x; x <= end_x; x++) {
      const int px = (s->flags & AB_PCE_SPR_XFLIP) ? (x & 0xF) : (15 - (x & 0xF));
      uint16_t pixel = (uint16_t)(((p1 >> px) & 1)
                     | (((p2 >> px) & 1) << 1)
                     | (((p3 >> px) & 1) << 2)
                     | (((p4 >> px) & 1) << 3));
      if (pixel & 0x0F) {
        const int xs = pos + x;
        /* A behind-background sprite over an opaque BG pixel still lands in
         * the sprite buffer (occluding lower-numbered sprites) but WITHOUT
         * the in-front tag, so it loses the final copy. */
        if (!priority && (line[xs] & 0x0F))
          pixel |= AB_PCE_LB_SPRITE;
        else
          pixel |= (uint16_t)(s->palette | AB_PCE_LB_SPRITE | AB_PCE_LB_INFRONT);
        sprite_line[xs] = pixel;
      }
    }
  }

  for (int i = 0; i < width; i++)
    if (sprite_line[i] & AB_PCE_LB_INFRONT)
      line[i] = (uint16_t)(sprite_line[i] & ~AB_PCE_LB_INFRONT);
}

void ab_pce_render_line(const ab_pce_frame *f, int fb_row, int fb_height,
                        int width, int force_bg, int force_sprites,
                        uint16_t *line) {
  if (!f || !line || width <= 0) return;

  /* Both layers take the same bias: the background's BYR-relative row and the
   * sprite fetch's raster line are the SAME vertical coordinate. It also
   * indexes the per-line table -- ONE mapping from framebuffer row to VDC
   * raster line, used by both paths. */
  uint16_t cr, mwr;
  int bxr, bg_y;
  int raster;

  ab_pce_regline rl;
  memset(&rl, 0, sizeof(rl));
  /* EXACT PATH: find the record whose recorded vpos lands on this framebuffer
   * row. No bias guess is involved -- the core told us which line it drew
   * where. A row that no rendered line claims is OUTSIDE the display window
   * (the VDC's VDW is shorter than the VCE's visible area, or starts below
   * row 0), and the VCE fills it with the VDC's idle 0x100 pixel. Painting
   * such a row with a nearby line's scroll is what the constant bias used to
   * do, and it is why games whose window did not start at vpos 22 smeared. */
  if (f->has_reglines && ab_pce_regline_for_row(f, fb_row, fb_height, &rl)) {
    raster = rl.raster;

    /* BURST FIRST, BEFORE THE RESOLVED PLANE. In burst mode HuC6270::Clock
     * returns HUC6270_PIXEL_BLACK without consulting the line buffer at all,
     * and RenderLine skips both renderers -- so the recorded m_line_buffer
     * still holds whatever the last non-burst line left there. Reading the
     * resolved plane here would paint stale picture over a blanked screen.
     * The VCE writes literal black for this code, bypassing the palette, so
     * it is NOT the 0x100 idle pixel. Parasol Stars' screen transition sat at
     * 0.0000% -- a flat blue frame against the hardware's flat black. */
    if (rl.burst) {
      for (int i = 0; i < width; i++) line[i] = AB_PCE_LB_BLACK;
      return;
    }

    /* RESOLVED PATH. The core recorded its own m_line_buffer for this raster
     * line, so there is nothing to reconstruct: these ARE the palette indices
     * it will hand the VCE. Prefer it unconditionally -- re-running the line
     * renderer can only reintroduce error, and it is the ONLY way to be right
     * when the game DMA'd new patterns into VRAM mid-frame.
     *
     * force_bg/force_sprites cannot be honoured here (the layers are already
     * merged), so a caller asking to suppress a layer falls through to the
     * reconstruction below. That is exactly what ab_pce_emit's suppress path
     * needs and why the reconstruction is still carried. */
    if (f->has_linepix && !ab_pce_test_no_linepix &&
        force_bg < 0 && force_sprites < 0 &&
        raster >= 0 && raster < AB_PCE_LINEPIX_LINES) {
      const unsigned char *src =
          f->linepix + (size_t)raster * AB_PCE_LINEPIX_SIZE;
      for (int i = 0; i < width && i < AB_PCE_LINEPIX_WIDTH; i++)
        line[i] = ab_pce_u16(src, i);
      for (int i = AB_PCE_LINEPIX_WIDTH; i < width; i++)
        line[i] = AB_PCE_LB_SPRITE;
      /* The core applies the entry-0 collapse in Clock(), AFTER the line
       * buffer -- so the recorded buffer has NOT had it applied yet and it
       * must still happen here. */
#ifdef AB_PCE_BIAS_OVERRIDE
      if (getenv("CTL_NO_ENTRY0")) return;   /* must-fail control */
#endif
      for (int i = 0; i < width; i++)
        if ((line[i] & 0x0F) == 0) line[i] = 0;
      return;
    }
  } else if (f->has_reglines && ab_pce_reglines_have_vpos(f)) {
    /* The core records vpos and no line claimed this row: it is genuinely
     * blank. Do NOT fall back to the frame-end registers here -- that would
     * invent picture where the hardware emitted border. */
    for (int i = 0; i < width; i++) line[i] = AB_PCE_LB_SPRITE;
    return;
  } else {
    raster = fb_row + ab_pce_raster_bias(f);
    ab_pce_regline_at(f, raster, &rl);
  }

  if (rl.valid) {
    /* PER-LINE PATH. rl.bgy is the EFFECTIVE BYR the core rendered this line
     * with (m_bg_offset_y), already advanced by the per-line counter -- so it
     * is used DIRECTLY, not added to the raster line the way the frame-end
     * BYR must be. Adding the raster here double-counts the scroll and is the
     * obvious wrong turn. */
    cr = rl.cr; mwr = rl.mwr; bxr = rl.bxr; bg_y = rl.bgy;
    /* Burst is handled above, before the resolved plane, because the recorded
     * line buffer is stale on a burst line. Kept here too for the fallback
     * path, which reaches this point without passing that check. */
    if (rl.burst) {
      for (int i = 0; i < width; i++) line[i] = AB_PCE_LB_BLACK;
      return;
    }
  } else {
    /* FALLBACK: frame-end registers, one scroll for the whole picture. Correct
     * only for frames without a raster split; see the header note. */
    cr  = ab_pce_reg(f, AB_PCE_REG_CR);
    mwr = ab_pce_reg(f, AB_PCE_REG_MWR);
    bxr = ab_pce_reg(f, AB_PCE_REG_BXR);
    bg_y = (int)ab_pce_reg(f, AB_PCE_REG_BYR) + raster;
  }

  /* The live CR can lie about a frame that already rendered (see the header
   * note on latched registers), so the caller may override. The per-line CR is
   * the LATCHED value and does not lie, but the override stays available on
   * both paths so callers do not have to know which one ran. */
  const int bg_on  = (force_bg  < 0) ? ((cr & AB_PCE_CR_BG_ON)  != 0) : force_bg;
  const int spr_on = (force_sprites < 0) ? ((cr & AB_PCE_CR_SPR_ON) != 0)
                                         : force_sprites;

  if (bg_on) {
    render_background(f, width, bg_y, bxr, mwr, line);
  } else {
    /* CR bit 7 clear parks the whole line at 0x100 -- the VDC's "no
     * background" code, which the VCE resolves to palette index 0x100. */
    for (int i = 0; i < width; i++) line[i] = AB_PCE_LB_SPRITE;
  }

  if (spr_on) {
    pce_sprite sprites[AB_PCE_SPR_PER_LINE];
    uint16_t sprite_line[AB_PCE_MAX_W];
    const int n = fetch_sprites(f, raster, mwr, sprites, AB_PCE_SPR_PER_LINE);
    if (n > 0) render_sprites(f, width, sprites, n, line, sprite_line);
  }

  /* HuC6270::Clock: a pixel whose colour ENTRY is 0 collapses to palette
   * index 0 -- the shared backdrop -- not to "sub-palette N entry 0". */
#ifdef AB_PCE_BIAS_OVERRIDE
  if (getenv("CTL_NO_ENTRY0")) return;   /* must-fail control */
#endif
  for (int i = 0; i < width; i++)
    if ((line[i] & 0x0F) == 0) line[i] = 0;
}

void ab_pce_render_fb_row(const ab_pce_frame *f, int fb_row, int fb_height,
                          int fb_width, int force_bg, int force_sprites,
                          uint16_t *row) {
  if (!f || !row || fb_width <= 0) return;

  /* Outside the VDC's HDW window every VCE clock samples 0x100 -- HuC6270::
   * Clock's default. That is a real palette index, not "undrawn": the VCE
   * resolves it through the colour table like any other pixel, which is why
   * the border is the backdrop colour and not black. */
  for (int i = 0; i < fb_width; i++) row[i] = AB_PCE_LB_SPRITE;

  const int aw = ab_pce_active_width(f);
  if (aw <= 0) return;
  /* Prefer the RECORDED placement: the arithmetic in ab_pce_x_offset is
   * right for most of the library but 8px wrong on some games, because the
   * VDC's HSW/HDS chain and the VCE's m_screen_start_x are different clock
   * domains. See the FACTS block. */
  int xoff = ab_pce_x_offset(f, fb_width);
  if (f->has_xofflines) {
    const int vpos = fb_row + ab_pce_vpos_origin(fb_height);
    if (vpos >= 0 && vpos < AB_PCE_XOFFLINES) {
      const uint16_t rec = ab_pce_u16(f->xofflines, vpos);
      /* 0xFFFF means the VDC emitted no picture on this scanline, which is a
       * genuine blank row -- not a reason to fall back to the formula. */
      if (rec == AB_PCE_XOFF_NONE) return;
      xoff = (int)rec;
    }
  }
#ifdef AB_PCE_BIAS_OVERRIDE
  if (getenv("CTL_XOFF0")) xoff = 0;   /* must-fail control */
#endif

  /* Nothing of the picture lands inside the row. Legal: a game can park the
   * display window entirely outside what the VCE copies. */
  if (xoff >= fb_width || xoff + aw <= 0) return;

  uint16_t line[AB_PCE_MAX_W];
  ab_pce_render_line(f, fb_row, fb_height, aw, force_bg, force_sprites, line);

  /* A negative offset CLIPS the left of the picture rather than shifting it --
   * the VDC opened its window before the VCE started copying, so those pixels
   * were emitted into a part of the line the framebuffer never sees.
   *
   * When the core recorded it, the clip amount comes from pce_vce_srclines
   * rather than from -xoff. The two disagree whenever the VDC's active window
   * is WIDER than the VCE's line (two carts in the corpus
   * program 352px into a 341px line): there the picture lands at x=0, so
   * xoff is 0 and carries no clip information at all, while the first 8
   * SOURCE pixels are still dropped. Scored 73.4% / 76.5% until this was
   * separated out. */
  int src = (xoff < 0) ? -xoff : 0;
  int dst = (xoff < 0) ? 0 : xoff;
  if (f->has_srclines) {
    const int vpos = fb_row + ab_pce_vpos_origin(fb_height);
    if (vpos >= 0 && vpos < AB_PCE_XOFFLINES)
      src = (int)ab_pce_u16(f->srclines, vpos);
  }
  for (; src < aw && dst < fb_width; src++, dst++) row[dst] = line[src];
}

/* --- emit ---------------------------------------------------------------- */
int ab_pce_emit(ab_batch *b, const ab_pce_frame *f, const ab_pce_view *v,
                const unsigned char *suppress) {
  if (!b || !f || !v) return 0;

  /* Prefer the width the CALLER measured from the core's framebuffer; fall
   * back to the VDC active window only when it did not supply one. See the
   * note on ab_pce_view.fb_width -- these differ on 341-wide games and the
   * difference is the entire left-clip. */
  const int width = (v->fb_width > 0) ? v->fb_width : ab_pce_width(f);
  int height = v->height;
  if (height <= 0 || height > AB_PCE_MAX_H) height = AB_PCE_MAX_H;

  uint32_t lut[AB_PCE_PAL_ENTRIES];
  /* Built per ROW, not per frame: the VCE colour table can change mid-frame
   * (see the FACTS block). With no pallines region this rebuilds the same
   * table every row, which is cheap and keeps one code path. */
  const int vpos0 = ab_pce_vpos_origin(height);

  uint16_t line[AB_PCE_MAX_W];
  uint16_t bg_line[AB_PCE_MAX_W];
  int quads = 0;

  for (int y = 0; y < height; y++) {
    /* THE PALETTE MUST BE LOOKED UP AT THE **RECORDED** vpos FOR THIS ROW,
     * the same vpos render_fb_row resolves the line at. ab_pce_vpos_origin is
     * a DERIVED fallback (scanline_start + 14) for cores too old to record
     * vpos; when the records exist the two can disagree -- measured 25 derived
     * vs 14 recorded on a 224p frame, an 11-line skew. Using the derived
     * origin applied every mid-frame recolour ~9 rows late, which shows up as
     * a handful of scattered wrong rows on raster-demo content (Eaggy's Little
     * Demo: 9 of 224 rows, 98.582%) and is invisible on everything that never
     * changes its palette mid-frame. */
    ab_pce_test_cur_row = y;
    int lut_vpos = y + vpos0;
    {
      ab_pce_regline rl_lut;
      if (ab_pce_regline_for_row(f, y, height, &rl_lut) && rl_lut.valid)
        lut_vpos = rl_lut.vpos;
    }
    ab_pce_build_lut_at(f, lut_vpos, lut);
    /* MID-ROW palette splits (pce_paldeltas). pallines[vpos] is the END-of-
     * line table, so the head of a row with mid-line writes must render
     * through the values those writes REPLACED: undo the row's events in
     * reverse to reach the line-start table, then re-apply each newv when x
     * reaches its dot. Events with dot 0 are hblank writes and re-apply
     * before x 0 -- whole-row, no visible split. */
    ab_pce_pal_ev pev[16];
    int pev_n = ab_pce_pal_events_for_row(f, lut_vpos, pev, 16);
    if (ab_pce_test_no_paldeltas) pev_n = 0;
    for (int i = pev_n - 1; i >= 0; i--)
      ab_pce_lut_set(lut, pev[i].index, pev[i].oldv);
    int pev_i = 0;
    /* render_fb_ROW, not render_LINE. render_line produces only the VDC's
     * active window and knows nothing about where that window sits inside the
     * framebuffer; render_fb_row applies ab_pce_x_offset (or the recorded
     * xofflines/srclines placement) and fills the border. Emitting the raw
     * window put the whole picture 8px off on every game whose VDC window is
     * wider than the VCE line -- the 341-wide set -- while the C harness,
     * which scores through render_fb_row, read 100%. */
    ab_pce_render_fb_row(f, y, height, width, v->force_bg, v->force_sprites, line);

    /* A suppressed pixel must reveal BACKGROUND, not a hole -- same rule the
     * GB profile documents. Rendering the line a second time with sprites off
     * is the cheapest way to get that, and it only happens on rows that
     * actually carry replacement art. */
    const unsigned char *srow = suppress ? suppress + (size_t)y * width : NULL;
    int have_bg_line = 0;
    if (srow) {
      for (int x = 0; x < width; x++) {
        if (srow[x]) {
          ab_pce_render_fb_row(f, y, height, width, v->force_bg, 0, bg_line);
          have_bg_line = 1;
          break;
        }
      }
    }

    int x = 0;
    while (x < width) {
      while (pev_i < pev_n && pev[pev_i].dot <= x) {
        ab_pce_lut_set(lut, pev[pev_i].index, pev[pev_i].newv);
        pev_i++;
      }
      const int idx = (srow && srow[x] && have_bg_line) ? bg_line[x] : line[x];
      const uint32_t rgba = ab_pce_resolve(lut, (uint16_t)idx);
      const int x0 = x;

      while (x + 1 < width) {
        /* A pending palette event at the next pixel ends the run: the same
         * line-buffer code resolves to a DIFFERENT colour past the dot. */
        if (pev_i < pev_n && pev[pev_i].dot <= x + 1) break;
        const int nidx = (srow && srow[x + 1] && have_bg_line)
                       ? bg_line[x + 1] : line[x + 1];
        if (ab_pce_resolve(lut, (uint16_t)nidx) != rgba) break;
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

/* --- SATB helpers -------------------------------------------------------- */
int ab_pce_mark_sprites(const ab_pce_frame *f, const ab_registry *reg,
                        int width, int height, unsigned char *suppress,
                        const ab_sub_rule **out_rule,
                        ab_pce_bounds *out_bounds) {
  if (!f || !reg || !suppress) return 0;
  if (width <= 0 || height <= 0) return 0;

  int marked = 0;
  int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
  const ab_sub_rule *rule = NULL;

  for (int i = 0; i < AB_PCE_SPRITES; i++) {
    const int o = i * 4;
    const uint16_t sat0 = ab_pce_u16(f->sat, o + 0);
    const uint16_t sat1 = ab_pce_u16(f->sat, o + 1);
    const uint16_t sat3 = ab_pce_u16(f->sat, o + 3);

    /* The PATTERN number is the PCE's tile id -- that is the substitution
     * key, and it is sat2 >> 1 (bit 0 is the mode-1 half selector). */
    const int pattern = ab_pce_sprite_pattern(f, i);
    const ab_sub_rule *m = ab_registry_match_tile(reg, pattern);
    if (!m) continue;
    rule = m;

    const int sx = (int)(sat1 & 0x3FF) - 0x20;
    /* Screen y is biased by 64, x by 0x20 -- different per axis. */
    const int sy = (int)(sat0 & 0x3FF) - 64 - AB_PCE_RASTER_BIAS;
    const int w = k_spr_w[(sat3 >> 8) & 1];
    const int h = k_spr_h[(sat3 >> 12) & 3];

    for (int yy = sy; yy < sy + h; yy++) {
      if (yy < 0 || yy >= height) continue;
      unsigned char *row = suppress + (size_t)yy * width;
      for (int xx = sx; xx < sx + w; xx++)
        if (xx >= 0 && xx < width) row[xx] = 1;
    }
    marked++;

    /* Bounds come only from anchoring patterns: shadow/filler sprites must be
     * suppressed but must not stretch the replacement art (the filler-tile
     * trap). */
    if (ab_registry_tile_anchors(m, pattern)) {
      if (sx < x0) x0 = sx;
      if (sy < y0) y0 = sy;
      if (sx + w > x1) x1 = sx + w;
      if (sy + h > y1) y1 = sy + h;
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
