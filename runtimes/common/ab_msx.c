/*
 * ab_msx.c -- MSX profile. See ab_msx.h for the facts this encodes.
 *
 * Transcribed from bluemsx Src/VideoChips/Common.h (RefreshLine0/1/2/3/4/5/6/
 * 7/8), VDP.c (updateScreenMode, onScrModeChange, paletteFixed, paletteSprite8)
 * and SpriteLine.h (spritesLine, colorSpritesLine). Where a wiki and the core
 * disagree, the core wins -- it is what actually produced the pixels being
 * scored.
 *
 * MEASURED against the core's own framebuffer, captured THROUGH THE ROMDEV MCP
 * SERVER (session msx-mass-validation), on a random 250-ROM sample of an
 * 816-ROM corpus (500 sampled MSX1 titles + all 316 MSX2 titles).
 *
 * Frames count only when the GAME drew them. A flat frame is a FAIL, never a
 * pass: "all one colour, be it blue or black or anything, is not passing."
 * A one-colour rectangle is trivially pixel-exact, so counting it inflates any
 * accuracy figure -- an earlier pass of this profile scored 630 such frames at
 * a perfect 100% and they were 30% of the denominator.
 *
 *   227 game frames scored with per-scanline records
 *   208 pixel-exact, 15,525,500 / 15,536,640 = 99.9283%
 *     SCREEN 0    2 frames 100.0000%     SCREEN 5   62 frames  99.9944%
 *     SCREEN 1   16 frames  99.9969%     SCREEN 6    4 frames 100.0000%
 *     SCREEN 2  120 frames  99.8611%     SCREEN 7   10 frames 100.0000%
 *     SCREEN 4    3 frames 100.0000%     SCREEN 8    7 frames 100.0000%
 *
 *   Per-scanline records took the whole set from 99.5611% to 99.9283%, and
 *   SCREEN 5 from 98.6060% to 99.9944%.
 *
 * The 16 remaining imperfect frames are NOT raster splits: 13 of them show no
 * per-line register or palette variation whatsoever, so there is nothing for a
 * register record to follow. Their cause is mid-frame VRAM mutation (Car
 * Fighter rewrites 158 VRAM bytes per frame with constant registers), which
 * would need a per-line VRAM snapshot to reconstruct. See ab_msx.h.
 *
 * The canonical example is a 1987 MSX2 action-RPG that runs a mid-frame RASTER
 * SPLIT: rows 30..130 are drawn in a
 * different mode from the registers left at vblank, and contain colours that
 * only SCREEN 8's fixed palette can produce while the registers say SCREEN 5.
 * Outside that band the same frame scores 37808/37808 = 100.0000%. A stable
 * raster split reproduces itself every frame, so a frame-identity filter does
 * not catch it; it is a limit of snapshot reconstruction, not a renderer bug,
 * and is reported rather than filtered away.
 *
 * SCREEN 6 is the thinnest evidence in the table: ONE frame in the whole corpus
 * reaches it. Its 100% is real but is a single data point, not a verified mode.
 *
 * The must-fail controls live in tests/run.sh; each deliberately breaks one
 * rule below and the suite asserts the score DROPS. A control that does not
 * drop the score means the rule is untested, not that it is right.
 */
#include "ab_msx.h"

#include <string.h>

/* The core builds every table base as (reg << shift) | ~(-1 << shift) and then
 * uses it as an AND-MASK. Spelled out here so the intent survives: the low
 * bits are forced to ones so they never mask the index. */
static int32_t mask_base(int32_t value, int shift, int32_t vram_mask) {
  return (int32_t)(((uint32_t)value << shift) | (uint32_t)((1u << shift) - 1u)) & vram_mask;
}

void ab_msx_decode(const unsigned char *regs, const unsigned char *status,
                   uint32_t vram_size, ab_msx_state *st) {
  memset(st, 0, sizeof(*st));

  if (vram_size == 0) vram_size = AB_MSX_VRAM_SIZE;
  st->vram_pages = (int)(vram_size >> 14);
  st->vram_mask  = ((uint32_t)st->vram_pages << 14) - 1u;
  /* vdp->vram128 is 0x10000 once the VDP has at least 128K; SCREEN 7/8 use it
   * directly as the "odd bytes" half of the interleaved address space. */
  st->vram128 = st->vram_pages >= 8 ? 0x10000 : 0;
  /* vramAccMask (VDP.c): selected by R8 bit3 and R#45 bit6. Only the 64 base
   * registers are exposed, so R#45 is unavailable and the index's low bit is 0.
   * On a 128K VDP that picks 0x7fff or 0x1ffff. */
  st->vram_acc_mask = (regs[8] & 0x08)
      ? (vram_size > 0x20000 ? 0x1ffffu : vram_size - 1u)
      : (vram_size > 0x8000  ? 0x7fffu  : vram_size - 1u);

  /* updateScreenMode(): R0 bits 3-1 and R1 bits 4,3 select the mode. */
  const int sel = ((regs[0] & 0x0e) >> 1) | (regs[1] & 0x18);
  st->yjk = (regs[25] & 0x08) != 0;
  switch (sel) {
    case 0x10: st->mode = AB_MSX_MODE_SCREEN0; break;
    case 0x00: st->mode = AB_MSX_MODE_SCREEN1; break;
    case 0x01: st->mode = AB_MSX_MODE_SCREEN2; break;
    case 0x08: st->mode = AB_MSX_MODE_SCREEN3; break;
    case 0x02: st->mode = AB_MSX_MODE_SCREEN4; break;
    case 0x03: st->mode = AB_MSX_MODE_SCREEN5; break;
    case 0x04: st->mode = AB_MSX_MODE_SCREEN6; break;
    /* SCREEN 7 and 8 both become the YJK modes 10/12 when R25 bit3 is set;
     * those decode Y/J/K through a precomputed colour table rather than the
     * palette and are NOT implemented, so they report unsupported. */
    case 0x05: st->mode = st->yjk ? AB_MSX_MODE_UNSUPPORTED : AB_MSX_MODE_SCREEN7; break;
    case 0x07: st->mode = st->yjk ? AB_MSX_MODE_UNSUPPORTED : AB_MSX_MODE_SCREEN8; break;
    /* TEXT80 (13), the SCREEN 0+2 / 0+3 combinations (16/32) and every unknown
     * selector. On a V9938/V9958 -- which is what romdev runs -- the combos
     * select RefreshLineBlank, i.e. the core draws no picture either. */
    default:   st->mode = AB_MSX_MODE_UNSUPPORTED; break;
  }

  /* SCREEN 4..8 drive the V9938 colour sprite plane; 0..3 use the TMS plane.
   * SCREEN 0 has no sprites at all (RefreshLine0 never consults a sprite
   * buffer), which the renderer honours by not compositing one. */
  st->sprite_mode2 = st->mode >= AB_MSX_MODE_SCREEN4;
  st->wide  = (st->mode == AB_MSX_MODE_SCREEN6 || st->mode == AB_MSX_MODE_SCREEN7);
  st->out_w = st->wide ? AB_MSX_MAXW : AB_MSX_W;

  /* onScrModeChange(). chrTabBase additionally clears bit 15 when R25 bit0 is
   * set (the V9958 512-wide hscroll page bit). */
  const int32_t vm = (int32_t)st->vram_mask;
  st->chr_tab = (int32_t)((((uint32_t)regs[2] << 10) & ~((uint32_t)(regs[25] & 1) << 15))
                          | (uint32_t)((1u << 10) - 1u)) & vm;
  st->chr_gen = mask_base(regs[4], 11, vm);
  st->col_tab = (int32_t)((((uint32_t)regs[10] << 14) | ((uint32_t)regs[3] << 6)
                          | (uint32_t)((1u << 6) - 1u))) & vm;
  st->spr_tab = (int32_t)((((uint32_t)regs[11] << 15) | ((uint32_t)regs[5] << 7)
                          | (uint32_t)((1u << 7) - 1u))) & vm;
  st->spr_gen = mask_base(regs[6], 11, vm);

  st->fg = regs[7] >> 4;
  st->bg = regs[7] & 0x0f;
  st->screen_on    = (regs[1] & 0x40) != 0;
  st->sprites_16   = (regs[1] & 0x02) != 0;
  st->sprites_big  = (regs[1] & 0x01) != 0;
  st->sprites_off  = (regs[8] & 0x02) != 0;
  st->color0_solid = (regs[8] & 0x20) != 0;
  st->vscroll      = regs[23];

  /* onDisplay(): PAL pushes the whole picture down 27 lines; R18's nibbles are
   * SIGNED 4-bit display adjustments. Assuming NTSC/192 is right by luck on
   * most carts and silently wrong on the rest. */
  const int is_pal = (regs[9] & 0x02) != 0;
  const int lines212 = (regs[9] & 0x80) != 0;
  const signed char v_adj_raw = (signed char)regs[18];
  const signed char h_adj_raw = (signed char)(regs[18] << 4);
  const int v_adjust = -(v_adj_raw >> 4);

  st->display_offset = is_pal ? 27 : 0;
  st->active_lines   = lines212 ? 212 : 192;
  st->first_line     = st->display_offset + (lines212 ? 14 : 24) + v_adjust;
  st->h_adjust       = -(h_adj_raw >> 4);
  /* onScrModeChange adds 4 to HAdjust in the YJK modes. */
  if (regs[25] & 0x08) st->h_adjust += 4;

  /* vdpHScroll512 needs BOTH R25 bit0 and R2 bit5; vdpHScroll's mask collapses
   * to 8 bits unless the 512 mode is active. */
  st->hscroll512 = regs[25] & (regs[2] >> 5) & 0x01;
  st->hscroll = (int)(((((uint32_t)(regs[26] & 0x3f)) << 3) - (uint32_t)(regs[27] & 0x07))
                      & ~((uint32_t)(~st->hscroll512) << 8));
  st->edge_masked = (regs[25] & 0x02) != 0;

  /* vdpIsOddPage() reads STATUS register 2, not a control register. Without a
   * status buffer the even page is the safe assumption. */
  st->odd_page = status
      ? ((((~status[2] & 0x02) << 7) & ((regs[9] & 0x04) << 6)))
      : 0;

  /* vdpCreate sets hAdjustSc0 per VDP VERSION: -2 on the TMS9918 parts, +1 on
   * the V9938/V9958. romdev runs machine MSX2+ (V9958). */
  st->hadjust_sc0 = 1;
}

uint16_t ab_msx_palreg_to_565(uint16_t p) {
  /* writePaletteLatch/updatePalette: R lives in the low byte's 0x70 field, B in
   * its 0x07 field, G in the high byte's low 3 bits. The scaling is INTEGER
   * division -- rounding instead drifts a third of the entries by one LSB. */
  const int r = (int)((p & 0x0070) * 255 / 112);
  const int g = (int)(((p >> 8) & 0x07) * 255 / 7);
  const int b = (int)((p & 0x0007) * 255 / 7);
  /* FrameBuffer.h VIDEO_COLOR_TYPE_RGB565: a TRUE six-bit green at <<5. */
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint16_t rgb_to_565(int r, int g, int b) {
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* SCREEN 8's background palette: paletteFixed[256] from VDP.c. GRB332, but
 * BLUE is two bits with an (i & 3) == 3 -> 7 special case rather than a linear
 * ramp, so the top blue step is bigger than the others. */
static uint16_t pal_fixed(int i) {
  return rgb_to_565(255 * ((i >> 2) & 7) / 7,
                    255 * ((i >> 5) & 7) / 7,
                    255 * (((i & 3) == 3) ? 7 : 2 * (i & 3)) / 7);
}

/* SCREEN 8's SPRITE palette: paletteSprite8[16] from VDP.c. A separate fixed
 * table -- neither the palette registers nor paletteFixed. */
static uint16_t pal_sprite8(int i) {
  static const unsigned char rgb[16][3] = {
    {0,0,0},{0,0,2},{3,0,0},{3,0,2},{0,3,0},{0,3,2},{3,3,0},{3,3,2},
    {7,4,2},{0,0,7},{7,0,0},{7,0,7},{0,7,0},{0,7,7},{7,7,0},{7,7,7}
  };
  const unsigned char *c = rgb[i & 15];
  return rgb_to_565(c[0] * 255 / 7, c[1] * 255 / 7, c[2] * 255 / 7);
}

void ab_msx_build_palette(const unsigned char *pb, const ab_msx_state *st,
                          uint16_t out[AB_MSX_PALREG]) {
  for (int i = 0; i < AB_MSX_PALREG; i++) {
    const uint16_t v = (uint16_t)(pb[i * 2] | ((uint16_t)pb[i * 2 + 1] << 8));
    out[i] = ab_msx_palreg_to_565(v);
  }
  /* vdp_updateOutputMode: with transparency on, colour 0 SHOWS THE BACKDROP.
   * The core bakes this by pointing palette[0] at palette[BGColor]. Without it
   * every colour-0 pixel renders black -- a whole frame lost to "black is
   * obviously black". */
  const int transparency = !st->color0_solid;
  if (st->bg != 0 && transparency) out[0] = out[st->bg];
}

uint32_t ab_msx_rgba565(uint16_t c) {
  const uint32_t r = (c >> 11) & 0x1f;
  const uint32_t g = (c >> 5) & 0x3f;
  const uint32_t b = c & 0x1f;
  const uint32_t r8 = (r << 3) | (r >> 2);
  const uint32_t g8 = (g << 2) | (g >> 4);
  const uint32_t b8 = (b << 3) | (b >> 2);
  return (r8 << 24) | (g8 << 16) | (b8 << 8) | 0xFFu;
}

int ab_msx_frame_read(const ab_msx_regions *r, ab_msx_frame *f) {
  /* NOTE `< 0`, not truthiness: an absent region is -1, which is TRUE in C.
   * The GB profile learned this the expensive way. */
  if (!r || !f || !f->vram) return 0;
  if (r->vram < 0 || r->regs < 0 || r->palette < 0) return 0;

  ab_region_slurp(r->vram, 0, f->vram, AB_MSX_VRAM_SIZE);
  ab_region_slurp(r->regs, 0, f->regs, AB_MSX_REGS);
  ab_region_slurp(r->palette, 0, f->palreg, AB_MSX_PALREG * 2);

  f->have_status = 0;
  if (r->status >= 0) {
    ab_region_slurp(r->status, 0, f->status, AB_MSX_STATUS);
    f->have_status = 1;
  }

  /* Per-scanline records, when the core exposes them. This is what lets the
   * profile follow a mid-frame register/palette/mode change instead of
   * applying one vblank snapshot to all 240 rows. */
  f->have_reglines = 0;
  if (r->reglines >= 0 && f->reglines) {
    ab_region_slurp(r->reglines, 0, (unsigned char *)f->reglines,
                    AB_MSX_REGLINES * AB_MSX_REGLINE_STRIDE);
    /* Trust it only if SOME line is marked valid -- an all-zero region means
     * the core allocated it but never rendered, and 0 is a legal register
     * value, so an unchecked read would silently render mode 1 everywhere. */
    for (int i = 0; i < AB_MSX_REGLINES; i++) {
      if (f->reglines[i].valid) { f->have_reglines = 1; break; }
    }
  }

  /* VRAM delta log, when the core exposes it AND the caller gave us both the
   * raw buffer and a working copy to replay into. `truncated` forces the
   * fallback: a partial undo would rebuild a frame-start state that never
   * existed, so a full log is the only log worth trusting. */
  f->have_vdeltas = 0;
  f->have_cut = 0;
  if (r->vdeltas >= 0 && f->vdeltas && f->vram_work) {
    ab_region_slurp(r->vdeltas, 0, f->vdeltas, AB_MSX_VDELTAS_SIZE);
    const int count = f->vdeltas[0] | (f->vdeltas[1] << 8);
    const int truncated = f->vdeltas[2];
    if (!truncated && count > 0 && count <= AB_MSX_VDELTA_CAP)
      f->have_vdeltas = 1;
    /* The cut is meaningful even when the write log is empty. */
    f->cut_line   = f->vdeltas[4] | (f->vdeltas[5] << 8);
    f->cut_offset = (int)(int16_t)(f->vdeltas[6] | (f->vdeltas[7] << 8));
    if (!truncated && f->cut_line != AB_MSX_CUT_NONE)
      f->have_cut = 1;
  }

  /* Core fossil snapshot. Trusted only when its recorded cut MATCHES the
   * vdeltas cut: both are captured from the same struct at retro_run end, so
   * a disagreement means the region reads straddled a frame boundary and the
   * pixel rows may belong to a different frame than the state describes. */
  f->have_fbtail = 0;
  if (r->fbtail >= 0 && f->fbtail && f->have_cut) {
    ab_region_slurp(r->fbtail, 0, f->fbtail, AB_MSX_FBTAIL_SIZE);
    const int fcl = f->fbtail[0] | (f->fbtail[1] << 8);
    const int fco = (int)(int16_t)(f->fbtail[2] | (f->fbtail[3] << 8));
    const int frows = f->fbtail[4] | (f->fbtail[5] << 8);
    const int fw = f->fbtail[6] | (f->fbtail[7] << 8);
    if (fcl == f->cut_line && fco == f->cut_offset &&
        frows > 0 && frows <= AB_MSX_FBTAIL_ROWS &&
        fw > 0 && fw <= AB_MSX_FBTAIL_W)
      f->have_fbtail = 1;
  }

  ab_msx_decode(f->regs, f->have_status ? f->status : NULL,
                AB_MSX_VRAM_SIZE, &f->st);
  ab_msx_build_palette(f->palreg, &f->st, f->palette);
  return 1;
}

/* --- test-only hooks -----------------------------------------------------
 * Each disables ONE documented rule so tests/run.sh can prove that rule is
 * load-bearing. A control that does not drop the score means the rule is
 * untested, not that it is right. Never set any of these in production.
 */
int ab_msx_test_no_interleave = 0;   /* skip MAP_VRAM's mode 7..12 rewrite */
int ab_msx_test_no_sprites    = 0;   /* composite no sprite plane at all */
int ab_msx_test_ccmask_f0     = 0;   /* start ccColorMask at 0xf0, not 0xff */
int ab_msx_test_spr4_limit    = 0;   /* apply the mode-1 4-per-line cap in mode 2 */
int ab_msx_test_sprend_208    = 0;   /* use the mode-1 terminator in mode 2 */
int ab_msx_test_no_reglines   = 0;   /* ignore the per-scanline records */
int ab_msx_test_reglines_novalid = 0; /* trust every regline, valid bit or not */
int ab_msx_test_no_vdeltas    = 0;   /* ignore the VRAM delta log */
int ab_msx_test_no_subline    = 0;   /* apply the log per-LINE only: no splits */
int ab_msx_test_no_retention  = 0;   /* never substitute the caller's prior rows */
int ab_msx_test_no_fbtail     = 0;   /* ignore the core fossil snapshot only */

/* One parsed state-log entry. */
typedef struct {
  int line, dot, kind;
  uint32_t addr;
  uint16_t oldv, newv;
} ab_msx_vd_entry;

static void ab_msx_vd_parse(const unsigned char *vd, int i, ab_msx_vd_entry *e) {
  const unsigned char *p = vd + AB_MSX_VDELTA_HDR + (size_t)i * AB_MSX_VDELTA_ENTRY;
  e->line = p[0] | (p[1] << 8);
  e->dot  = p[2] | (p[3] << 8);
  const uint32_t a = (uint32_t)p[4] | ((uint32_t)p[5] << 8)
                   | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
  e->kind = (int)(a >> 30);
  e->addr = a & 0x3FFFFFFFu;
  e->oldv = (uint16_t)(p[8] | (p[9] << 8));
  e->newv = (uint16_t)(p[10] | (p[11] << 8));
}

/* Table-base / geometry corruptions, applied to the state a ROW actually
 * renders with. They used to be applied to the frame state by the harness, but
 * per-row resolution overwrites that, which silently disarmed them -- a
 * control that cannot reach the renderer proves nothing. */
int ab_msx_test_corrupt_chrtab = 0;
int ab_msx_test_corrupt_coltab = 0;
int ab_msx_test_corrupt_sprgen = 0;
int ab_msx_test_corrupt_hscroll = 0;
int ab_msx_test_corrupt_edge = 0;
int ab_msx_test_corrupt_sc0adjust = 0;
int ab_msx_test_corrupt_sprmode1 = 0;
int ab_msx_test_corrupt_bgtransparent = 0;
int ab_msx_test_corrupt_palette = 0;

static void ab_msx_apply_test_corruptions(ab_msx_frame *out) {
  if (ab_msx_test_corrupt_chrtab)  out->st.chr_tab ^= 0x0400;
  if (ab_msx_test_corrupt_coltab)  out->st.col_tab ^= 0x0040;
  if (ab_msx_test_corrupt_sprgen)  out->st.spr_gen ^= 0x0800;
  if (ab_msx_test_corrupt_hscroll) out->st.hscroll ^= 3;
  if (ab_msx_test_corrupt_edge)    out->st.edge_masked = 0;
  if (ab_msx_test_corrupt_sc0adjust) out->st.hadjust_sc0 = -2;
  if (ab_msx_test_corrupt_sprmode1)  out->st.sprite_mode2 = 0;
  if (ab_msx_test_corrupt_bgtransparent) {
    const uint16_t v = (uint16_t)(out->palreg[0] | ((uint16_t)out->palreg[1] << 8));
    out->palette[0] = ab_msx_palreg_to_565(v);
  }
  if (ab_msx_test_corrupt_palette) {
    const uint16_t t = out->palette[4];
    out->palette[4] = out->palette[7]; out->palette[7] = t;
  }
}

/* MAP_VRAM: modes 7..12 address VRAM INTERLEAVED -- even bytes in the low 64K,
 * odd bytes in the high 64K. Every sprite fetch in SCREEN 7/8 goes through this
 * or it reads scrambled bytes. */
static unsigned char vram_read(const ab_msx_frame *f, int32_t addr) {
  const ab_msx_state *st = &f->st;
  uint32_t a = (uint32_t)addr;
  if (!ab_msx_test_no_interleave && st->mode >= 7 && st->mode <= 12)
    a = (a >> 1) | ((a & 1u) << 16);
  return f->vram[a & st->vram_acc_mask];
}

/* --- sprites, TMS9918 plane (mode 1) -------------------------------------
 * SpriteLine.h spritesLine().
 */
static int sprite_line_mode1(const ab_msx_frame *f, int vdp_line,
                             unsigned char *buf) {
  const ab_msx_state *st = &f->st;
  /* The core returns the null buffer for line 0 outright. */
  if (vdp_line == 0) return 0;
  if (!st->screen_on || st->sprites_off) return 0;

  const uint32_t vm = st->vram_mask;
  const int size  = st->sprites_16 ? 16 : 8;
  const int scale = st->sprites_big ? 2 : 1;
  const int pattern_mask = st->sprites_16 ? 0xfc : 0xff;
  const int32_t attr_base = st->spr_tab & (int32_t)~0x7fu;

  const int line = (vdp_line - st->first_line + st->vscroll) & 0xff;

  int vis_attr[AB_MSX_SPR_PERLINE];
  int vis_row[AB_MSX_SPR_PERLINE];
  int vis = 0;

  for (int i = 0; i < AB_MSX_SPRITES; i++) {
    const int32_t a = (attr_base + i * 4);
    const int sy = f->vram[a & vm];
    /* y == 208 TERMINATES the list -- it does not merely skip this sprite. */
    if (sy == AB_MSX_SPR_END) break;
    const int row = ((line - sy) & 0xff) / scale;
    if (row >= size) continue;
    /* The 5th sprite on a line is dropped (and would set status bit 0x40). */
    if (vis == AB_MSX_SPR_PERLINE) break;
    vis_attr[vis] = (int)a;
    vis_row[vis] = row;
    vis++;
  }
  if (vis == 0) return 0;

  int wrote = 0;
  /* REVERSE order: the core walks `while (visibleCnt--)`, so the LOWEST sprite
   * index is written LAST and therefore wins every overlap. */
  for (int i = vis - 1; i >= 0; i--) {
    const int32_t a = vis_attr[i];
    const int row = vis_row[i];
    const int colour = f->vram[(a + 3) & vm] & 0x0f;
    /* Attribute bit 7 ("early clock") shifts the sprite 32px LEFT. */
    const int x0 = f->vram[(a + 1) & vm] + AB_MSX_SPRBUF_ORG
                 - ((f->vram[(a + 3) & vm] >> 2) & 0x20);

    /* A colour-0 sprite is INVISIBLE but still collides; drawing it paints
     * black blobs over the background. */
    if (!st->color0_solid && colour == 0) continue;

    const int32_t pp = (st->spr_gen & (int32_t)~0x7ffu)
                     + ((f->vram[(a + 2) & vm] & pattern_mask) << 3) + row;

    for (int half = 0; half < (st->sprites_16 ? 2 : 1); half++) {
      /* The right half of a 16x16 sprite lives 16 BYTES further on, not 8. */
      const int pat = f->vram[(pp + half * 16) & vm];
      if (!pat) continue;
      for (int bit = 0; bit < 8; bit++) {
        if (!(pat & (0x80 >> bit))) continue;
        const int px = (half * 8 + bit) * scale;
        for (int k = 0; k < scale; k++) {
          const int o = x0 + px + k;
          if (o >= 0 && o < AB_MSX_SPRBUF) { buf[o] = (unsigned char)colour; wrote = 1; }
        }
      }
    }
  }
  return wrote;
}

/* --- sprites, V9938 plane (mode 2) ---------------------------------------
 * SpriteLine.h colorSpritesLine(). See ab_msx.h for the six ways this differs
 * from mode 1; every one of them is load-bearing.
 */
typedef struct {
  int colour;     /* the RAW per-line colour byte, CC/IC bits intact */
  int pos;        /* horizontalPos: x + 24, +8 more for 16x16, -32 early clock */
  unsigned pattern; /* 8 or 16 bits, MSB first */
} spr2_attr;

static void spr2_paint(const ab_msx_frame *f, const spr2_attr *a,
                       unsigned char *buf, int scr6, int scale, int solid,
                       int or_mode, int *wrote) {
  int colour;
  if (scr6) {
    /* SCREEN 6 packs the 4-colour sprite into the same byte differently. */
    colour = ((a->colour & 0x0c) << 2) | ((a->colour & 0x03) << 1) | (solid * 9);
  } else {
    /* The stored value is pre-shifted: the line renderers do `col >> 1`. */
    colour = ((a->colour & 0x0f) << 1) | solid;
  }
  if (colour == 0) return;

  unsigned pattern = a->pattern;
  int offset = scale * 15;
  while (pattern) {
    if (pattern & 1u) {
      for (int k = 0; k < scale; k++) {
        const int o = a->pos + offset + k;
        if (o >= 0 && o < AB_MSX_SPRBUF) {
          if (or_mode) buf[o] |= (unsigned char)colour;
          else         buf[o]  = (unsigned char)colour;
          *wrote = 1;
        }
      }
    }
    offset -= scale;
    pattern >>= 1;
  }
}

static int sprite_line_mode2(const ab_msx_frame *f, int vdp_line,
                             unsigned char *buf, unsigned char *cc_mask) {
  const ab_msx_state *st = &f->st;
  if (vdp_line == 0) return 0;
  /* colorSpritesLine also bails on vdpStatus[2] & 0x40, but that is the LIVE
   * "not currently displaying" flag -- always set in a between-frames snapshot.
   * Honouring it here would disable sprites on every frame. */
  if (!st->screen_on || st->sprites_off) return 0;

  const int solid = st->color0_solid ? 1 : 0;
  const int32_t attr_base = st->spr_tab & 0x1fe00;
  const int size  = st->sprites_16 ? 16 : 8;
  const int scale = st->sprites_big ? 2 : 1;
  const int pattern_mask = st->sprites_16 ? 0xfc : 0xff;
  const int scr6 = (st->mode == AB_MSX_MODE_SCREEN6);
  const int line = (vdp_line - st->first_line + st->vscroll) & 0xff;

  unsigned char cc_local[2] = { 0xff, 0xf0 };
  if (!cc_mask) cc_mask = cc_local;

  spr2_attr vis[AB_MSX_SPR_PERLINE2];
  int nvis = 0;

  for (int i = 0; i < AB_MSX_SPRITES; i++) {
    const int32_t off = attr_base + i * 4;
    const int sy = vram_read(f, off);
    /* The mode-2 terminator is 216, NOT 208. */
    if (sy == (ab_msx_test_sprend_208 ? AB_MSX_SPR_END : AB_MSX_SPR_END2)) break;
    const int row = ((line - sy) & 0xff) / scale;
    if (row >= size) continue;
    if (nvis == (ab_msx_test_spr4_limit ? AB_MSX_SPR_PERLINE : AB_MSX_SPR_PERLINE2)) break;

    const int32_t pat_off = (st->spr_gen & 0x1f800)
                          + ((vram_read(f, off + 2) & pattern_mask) << 3) + row;
    /* The colour is a PER-LINE byte, indexed by sprite AND row -- not the
     * per-sprite attribute byte mode 1 uses. */
    int colour = vram_read(f, st->spr_tab & (int32_t)((~0x3ffu) | (uint32_t)(i * 16 + row)));

    if (colour & 0x40) {
      /* A CC sprite is skipped outright when it is the FIRST visible sprite --
       * it has nothing to OR onto. */
      if (nvis == 0) continue;
      colour &= cc_mask[0];
    } else if ((colour & 0x0f) || solid) {
      cc_mask[1] = 0xff;
    }

    int pos = vram_read(f, off + 1) + 24 - ((colour >> 2) & 0x20);
    unsigned pattern = vram_read(f, pat_off);
    if (st->sprites_16) {
      pattern = (pattern << 8) | vram_read(f, pat_off + 16);
      pos += 8;
    }
    vis[nvis].colour = colour;
    vis[nvis].pos = pos;
    vis[nvis].pattern = pattern;
    nvis++;
  }
  if (nvis == 0) return 0;

  int wrote = 0;
  /* Reverse order, same as mode 1: sprite 0 paints last and wins. After each
   * sprite the core ORs in the run of CC sprites that FOLLOW it in the list,
   * stopping at the first non-CC entry. */
  for (int idx = nvis - 1; idx >= 0; idx--) {
    spr2_paint(f, &vis[idx], buf, scr6, scale, solid, 0, &wrote);
    for (int j = idx + 1; j < nvis; j++) {
      if (!(vis[j].colour & 0x40)) break;
      spr2_paint(f, &vis[j], buf, scr6, scale, solid, 1, &wrote);
    }
  }
  return wrote;
}

int ab_msx_sprite_line(const ab_msx_frame *f, int vdp_line, unsigned char *buf,
                       unsigned char *cc_mask) {
  memset(buf, 0, AB_MSX_SPRBUF);
  if (f->st.sprite_mode2) return sprite_line_mode2(f, vdp_line, buf, cc_mask);
  return sprite_line_mode1(f, vdp_line, buf);
}

/* --- background ----------------------------------------------------------
 * One character cell's two colours plus its 8 pattern bits, for the three
 * TMS9918 graphics modes and SCREEN 4 (which shares SCREEN 2's tile engine).
 * `pat` is consumed MSB-first.
 */
static void cell_lookup(const ab_msx_frame *f, int y, int32_t name_addr,
                        int *pat, uint16_t *c0, uint16_t *c1) {
  const ab_msx_state *st = &f->st;
  const uint32_t vm = st->vram_mask;
  const int name = f->vram[name_addr & vm];

  if (st->mode == AB_MSX_MODE_SCREEN2 || st->mode == AB_MSX_MODE_SCREEN4) {
    /* The three-thirds index: (y & 0xc0) << 5 selects one of three independent
     * 256-tile banks, and the SAME index feeds both tables through their own
     * masks. */
    const int32_t index = (int32_t)((~0x1fffu)
                        | (((uint32_t)y & 0xc0) << 5) | ((uint32_t)y & 7)
                        | ((uint32_t)name * 8));
    const int col = f->vram[(st->col_tab & index) & vm];
    *pat = f->vram[(st->chr_gen & index) & vm];
    *c0 = f->palette[col & 0x0f];
    *c1 = f->palette[col >> 4];
    return;
  }

  if (st->mode == AB_MSX_MODE_SCREEN1) {
    /* Graphic I: ONE colour byte per EIGHT tiles, and no thirds. */
    const int32_t pbase = st->chr_gen & (int32_t)((~0x7ffu) | ((uint32_t)y & 7));
    const int col = f->vram[(st->col_tab & ((name / 8) | (int32_t)~0x3fu)) & vm];
    *pat = f->vram[(pbase | (name * 8)) & vm];
    *c0 = f->palette[col & 0x0f];
    *c1 = f->palette[col >> 4];
    return;
  }

  /* SCREEN 3 (Multicolour): there are no pattern bits at all. Each byte is two
   * 4x4 blocks -- high nibble paints the LEFT four pixels, low nibble the
   * right -- and the row within the byte advances every FOUR lines (y >> 2). */
  const int32_t pbase = st->chr_gen & (int32_t)((~0x7ffu) | (((uint32_t)y >> 2) & 7));
  const int col = f->vram[(pbase | (name * 8)) & vm];
  *pat = 0xf0;
  *c0 = f->palette[col & 0x0f];
  *c1 = f->palette[col >> 4];
}

/* The page-flip walk shared by SCREEN 4..8. `jump` is jumpTable (or jumpTable4
 * for SCREEN 4) offset by hScroll512, and `scroll` counts cells so that every
 * wrap flips the page and slides charTable by the jump amount. */
typedef struct {
  const int32_t *jump;
  int32_t charTable;
  int page;
  int scroll;
  int wrap_mask;
} pageflip;

static void pf_step(pageflip *p) {
  if (((++p->scroll) & p->wrap_mask) == 0) {
    p->page ^= 1;
    p->charTable += p->jump[p->page];
  }
}

static const int32_t JUMPTABLE[4]  = { -128, -128, -0x8080, 0x7f80 };
static const int32_t JUMPTABLE4[4] = {  -32,  -32, -0x8020, 0x7fe0 };

/* SCREEN 5/6/7/8 all start their line the same way. */
static void pf_init_bitmap(const ab_msx_frame *f, int Y, int div, int wrap_mask,
                           pageflip *p) {
  const ab_msx_state *st = &f->st;
  p->jump = JUMPTABLE + st->hscroll512 * 2;
  p->page = (int)((uint32_t)st->chr_tab / 0x8000u) & 1;
  p->wrap_mask = wrap_mask;
  p->scroll = st->hscroll / div;

  const int y = Y - st->first_line + st->vscroll;
  p->charTable = (st->chr_tab & (int32_t)(~(uint32_t)st->odd_page << 7)
                              & (int32_t)((~0x7fffu) | ((uint32_t)y << 7)))
               + p->scroll;

  if (st->hscroll512) {
    /* The 512-wide modes test a different scroll bit than SCREEN 4/5 do. */
    const int bit = (div == 2 && wrap_mask == 0x7f) ? 0x80 : 0x100;
    if (p->scroll & bit) { p->page ^= 1; p->charTable += p->jump[p->page]; }
    if (st->chr_tab & (1 << 15)) { p->page ^= 1; p->charTable += p->jump[p->page] + 128; }
  }
}

/* Render one line's DISPLAY AREA into `out` (256 or 512 samples). The border is
 * applied by the caller, which is also what RefreshBorder/RefreshRightBorder do
 * around the RefreshLineN body. */
static void render_display(const ab_msx_frame *f, int Y, const unsigned char *spr,
                           uint16_t bg, uint16_t *out) {
  const ab_msx_state *st = &f->st;
  const uint32_t vm = st->vram_mask;

  /* ---- SCREEN 0: 40 columns of six-pixel characters ---------------------- */
  if (st->mode == AB_MSX_MODE_SCREEN0) {
    /* scr0splitLine is reset to 0 at every frame start and only becomes
     * non-zero on a MID-FRAME mode change, which a once-per-frame snapshot can
     * never observe -- 0 is the correct reconstruction. */
    const int y = Y - st->first_line + st->vscroll;
    const int32_t pbase = st->chr_gen & (int32_t)((~0x7ffu) | ((uint32_t)y & 7));
    const int hs = st->hscroll % 6;      /* MODULO six, not masked */
    const uint16_t c0 = f->palette[st->bg];
    const uint16_t c1 = f->palette[st->fg];

    int p = 0;
    for (int i = 0; i < hs; i++) out[p++] = c0;

    int x = 0, shift = 0, pattern = 0;
    for (int X = 0; X < 32; X++) {
      if (X == 0 || X == 31) {
        /* Cells 0 and 31 are the in-display margins, forced to BGColor. Cell 31
         * additionally rewinds by hScroll first. */
        if (X == 31) p -= hs;
        for (int i = 0; i < 8; i++, p++) if (p >= 0 && p < 512) out[p] = c0;
        continue;
      }
      for (int j = 0; j < 4; j++) {
        if (shift <= 2) {
          const int32_t idx = 0xc00 + 40 * (y / 8) + x; x++;
          const int32_t ct = st->chr_tab & (int32_t)((~0xfffu) | (uint32_t)idx);
          pattern = f->vram[(pbase | (f->vram[ct & vm] * 8)) & vm];
          shift = 8;
        }
        for (int k = 0; k < 2; k++, p++) {
          shift--;
          if (p >= 0 && p < 512) out[p] = ((pattern >> shift) & 1) ? c1 : c0;
        }
      }
    }
    return;
  }

  /* ---- SCREEN 1/2/3: the TMS tile modes, sprite plane 1 ------------------- */
  if (st->mode >= AB_MSX_MODE_SCREEN1 && st->mode <= AB_MSX_MODE_SCREEN3) {
    const int y = Y - st->first_line + st->vscroll;
    const int32_t char_tab = (st->chr_tab & (int32_t)((~0x3ffu)
                            | (uint32_t)(32 * (y / 8)))) & (int32_t)vm;
    for (int cx = 0; cx < 32; cx++) {
      int pat; uint16_t c0, c1;
      cell_lookup(f, y, char_tab + cx, &pat, &c0, &c1);
      for (int b = 0; b < 8; b++) {
        const int sx = cx * 8 + b;
        const int col = spr ? spr[sx + AB_MSX_SPRBUF_ORG] : 0;
        /* Mode-1 sprite bytes ARE the palette index. */
        out[sx] = col ? f->palette[col] : (((pat >> (7 - b)) & 1) ? c1 : c0);
      }
    }
    return;
  }

  /* ---- SCREEN 4: SCREEN 2's tiles + mode-2 sprites + hscroll -------------- */
  if (st->mode == AB_MSX_MODE_SCREEN4) {
    const int y = Y - st->first_line + st->vscroll;
    pageflip p;
    p.jump = JUMPTABLE4 + st->hscroll512 * 2;
    p.page = (int)((uint32_t)st->chr_tab / 0x8000u) & 1;
    p.wrap_mask = 0x1f;
    p.scroll = st->hscroll >> 3;
    p.charTable = (st->chr_tab & (int32_t)((~0x3ffu) | (uint32_t)(32 * (y / 8))))
                + p.scroll;
    if (st->hscroll512) {
      if (p.scroll & 0x20) { p.page ^= 1; p.charTable += p.jump[p.page]; }
      if (st->chr_tab & (1 << 15)) { p.page ^= 1; p.charTable += p.jump[p.page] + 32; }
    }

    int x = 0;
    if (st->edge_masked) {
      for (int i = 0; i < 8; i++) out[x + i] = bg;
      p.charTable++; pf_step(&p);
      x = 8;
    }

    int pat; uint16_t c0, c1;
    cell_lookup(f, y, p.charTable, &pat, &c0, &c1);

    /* The switch-fallthrough prologue: hScroll & 7 pixels of the first cell are
     * consumed before the aligned loop begins. */
    const int frac = st->hscroll & 7;
    if (frac) {
      for (int k = frac; k < 8 && x < 256; k++, x++) {
        const int col = spr ? spr[x + AB_MSX_SPRBUF_ORG] : 0;
        out[x] = col ? f->palette[col >> 1] : (((pat >> (7 - k)) & 1) ? c1 : c0);
      }
      p.charTable++; pf_step(&p);
    }

    while (x < 256) {
      cell_lookup(f, y, p.charTable, &pat, &c0, &c1);
      for (int b = 0; b < 8 && x < 256; b++, x++) {
        const int col = spr ? spr[x + AB_MSX_SPRBUF_ORG] : 0;
        out[x] = col ? f->palette[col >> 1] : (((pat >> (7 - b)) & 1) ? c1 : c0);
      }
      p.charTable++; pf_step(&p);
    }
    return;
  }

  /* ---- SCREEN 5: 4bpp linear bitmap, two pixels per byte ------------------ */
  if (st->mode == AB_MSX_MODE_SCREEN5) {
    pageflip p;
    pf_init_bitmap(f, Y, 2, 0x7f, &p);
    const int hs = st->hscroll;

    int x = 0;
    if (st->edge_masked) {
      for (int i = 0; i < 8; i++) out[i] = bg;
      pf_step(&p); pf_step(&p); pf_step(&p); pf_step(&p);
      p.charTable += 4;
      x = 8;
    } else {
      /* The sub-byte hScroll phase: the first cell emits `hs & 7` backdrop
       * pixels, then finishes the cell reading nibbles in phase. */
      const int i = hs & 7;
      int j = 0;
      for (; j < i; j++, x++) {
        out[x] = bg;
        if ((hs ^ j) & 1) { p.charTable++; pf_step(&p); }
      }
      for (; j < 8; j++, x++) {
        const int col = spr ? spr[x + AB_MSX_SPRBUF_ORG] : 0;
        const int byte = f->vram[p.charTable & st->vram_acc_mask];
        if ((hs ^ j) & 1) {
          out[x] = col ? f->palette[col >> 1] : f->palette[byte & 0x0f];
          p.charTable++; pf_step(&p);
        } else {
          out[x] = col ? f->palette[col >> 1] : f->palette[byte >> 4];
        }
      }
    }

    while (x < 256) {
      /* Two interleavings depending on the scroll's low bit: which nibble of
       * which byte lands on the even output pixel. */
      static const signed char PH1[8][2] = {{0,0},{1,1},{1,0},{2,1},{2,0},{3,1},{3,0},{4,1}};
      static const signed char PH0[8][2] = {{0,1},{0,0},{1,1},{1,0},{2,1},{2,0},{3,1},{3,0}};
      const signed char (*ph)[2] = (hs & 1) ? PH1 : PH0;
      for (int k = 0; k < 8 && x < 256; k++, x++) {
        const int col = spr ? spr[x + AB_MSX_SPRBUF_ORG] : 0;
        if (col) {
          out[x] = f->palette[col >> 1];
        } else {
          const int byte = f->vram[(p.charTable + ph[k][0]) & st->vram_acc_mask];
          out[x] = f->palette[ph[k][1] ? (byte >> 4) : (byte & 0x0f)];
        }
        if (!ph[k][1]) pf_step(&p);   /* the LOW nibble advances the table */
      }
      p.charTable += 4;
    }
    return;
  }

  /* ---- SCREEN 6: 512x212 in 4 colours, four pixels per byte --------------- */
  if (st->mode == AB_MSX_MODE_SCREEN6) {
    pageflip p;
    pf_init_bitmap(f, Y, 2, 0xff, &p);
    const int hs = st->hscroll;

    int x = 0;
    if (st->edge_masked) {
      for (int i = 0; i < 16; i++) out[i] = bg;
      for (int i = 0; i < 8; i++) pf_step(&p);
      p.charTable += 4;
      x = 16;
    }

    while (x < 512) {
      static const signed char S1[16][2] = {
        {0,2},{0,0},{1,6},{1,4},{1,2},{1,0},{2,6},{2,4},
        {2,2},{2,0},{3,6},{3,4},{3,2},{3,0},{4,6},{4,4}};
      static const signed char S0[16][2] = {
        {0,6},{0,4},{0,2},{0,0},{1,6},{1,4},{1,2},{1,0},
        {2,6},{2,4},{2,2},{2,0},{3,6},{3,4},{3,2},{3,0}};
      const signed char (*sl)[2] = (hs & 1) ? S1 : S0;
      for (int k = 0; k < 16 && x < 512; k++, x++) {
        const int si = (x / 2) + AB_MSX_SPRBUF_ORG;
        const int sv = (spr && si < AB_MSX_SPRBUF) ? spr[si] : 0;
        /* SCREEN 6 splits the sprite byte into two 3-bit halves: the EVEN
         * output sample takes bits 7..3, the ODD one bits 2..0. */
        const int col = (k & 1) ? (sv & 7) : (sv >> 3);
        if (col) {
          out[x] = f->palette[(col >> 1) & 3];
        } else {
          const int byte = f->vram[(p.charTable + sl[k][0]) & st->vram_acc_mask];
          out[x] = f->palette[(byte >> sl[k][1]) & 3];
        }
        if (k & 1) pf_step(&p);
      }
      p.charTable += 4;
    }
    return;
  }

  /* ---- SCREEN 7: 512x212 in 16 colours, interleaved VRAM ------------------ */
  if (st->mode == AB_MSX_MODE_SCREEN7) {
    pageflip p;
    pf_init_bitmap(f, Y, 2, 0xff, &p);
    const int hs = st->hscroll;
    const int32_t v128 = st->vram128;
    const uint32_t full = (uint32_t)st->vram_mask;

    int x = 0;
    if (st->edge_masked) {
      for (int i = 0; i < 16; i++) out[i] = bg;
      for (int i = 0; i < 8; i++) pf_step(&p);
      p.charTable += 4;
      x = 16;
    }

    while (x < 512) {
      /* SCREEN 7/8 index the interleaved array BY HAND with vram128 rather
       * than going through MAP_VRAM, so these offsets are literal. */
      const int32_t O1[8] = { v128, 1, v128 | 1, 2, v128 | 2, 3, v128 | 3, 4 };
      const int32_t O0[8] = { 0, v128, 1, v128 | 1, 2, v128 | 2, 3, v128 | 3 };
      const int32_t *o = (hs & 1) ? O1 : O0;
      for (int k = 0; k < 8 && x < 512; k++, x += 2) {
        const int si = (x / 2) + AB_MSX_SPRBUF_ORG;
        const int sv = (spr && si < AB_MSX_SPRBUF) ? spr[si] : 0;
        if (sv) {
          out[x] = f->palette[sv >> 1];
          if (x + 1 < 512) out[x + 1] = f->palette[sv >> 1];
        } else {
          const int byte = f->vram[(uint32_t)(p.charTable + o[k]) & full];
          out[x] = f->palette[byte >> 4];
          if (x + 1 < 512) out[x + 1] = f->palette[byte & 0x0f];
        }
        pf_step(&p);
      }
      p.charTable += 4;
    }
    return;
  }

  /* ---- SCREEN 8: 256x212 direct GRB332, fixed palettes -------------------- */
  if (st->mode == AB_MSX_MODE_SCREEN8) {
    pageflip p;
    pf_init_bitmap(f, Y, 2, 0xff, &p);
    const int hs = st->hscroll;
    const int32_t v128 = st->vram128;
    const uint32_t full = (uint32_t)st->vram_mask;

    int x = 0;
    if (st->edge_masked) {
      for (int i = 0; i < 8; i++) out[i] = bg;
      for (int i = 0; i < 8; i++) pf_step(&p);
      p.charTable += 4;
      x = 8;
    }

    while (x < 256) {
      const int32_t O1[8] = { v128, 1, v128 | 1, 2, v128 | 2, 3, v128 | 3, 4 };
      const int32_t O0[8] = { 0, v128, 1, v128 | 1, 2, v128 | 2, 3, v128 | 3 };
      const int32_t *o = (hs & 1) ? O1 : O0;
      for (int k = 0; k < 8 && x < 256; k++, x++) {
        const int col = spr ? spr[x + AB_MSX_SPRBUF_ORG] : 0;
        /* SCREEN 8 sprites use their OWN fixed table, not the palette regs. */
        out[x] = col ? pal_sprite8(col >> 1)
                     : pal_fixed(f->vram[(uint32_t)(p.charTable + o[k]) & full]);
        pf_step(&p);
      }
      p.charTable += 4;
    }
    return;
  }
}

/* Colour of every pixel of one framebuffer row, as RGB565. `spr` is the sprite
 * buffer for THIS row (already delayed by the caller) or NULL. */
static void render_row(const ab_msx_frame *f, int fy, const unsigned char *spr,
                       const unsigned char *suppress_row, uint16_t *row) {
  const ab_msx_state *st = &f->st;
  const int Y = fy + st->display_offset;
  const int line_size = st->wide ? 2 : 1;
  const int out_w = st->out_w;

  /* The backdrop differs by mode: SCREEN 8 takes the WHOLE R7 through the fixed
   * palette, SCREEN 6 takes only the low two bits, everything else the low
   * nibble through the palette registers. */
  uint16_t backdrop;
  if (st->mode == AB_MSX_MODE_SCREEN8)      backdrop = pal_fixed(f->regs[7]);
  else if (st->mode == AB_MSX_MODE_SCREEN6) backdrop = f->palette[st->bg & 0x03];
  else                                      backdrop = f->palette[st->bg];

  for (int x = 0; x < out_w; x++) row[x] = backdrop;

  /* Outside the active display -- or in a mode we do not claim -- the whole
   * line is backdrop, which is exactly what the core's blank path writes. */
  if (Y < st->first_line || Y >= st->first_line + st->active_lines
      || !st->screen_on || st->mode == AB_MSX_MODE_UNSUPPORTED) {
    return;
  }

  /* SCREEN 0 emits up to 6 extra samples from its hScroll prefix and starts two
   * early, so the scratch buffer carries slack past the display width. */
  uint16_t samples[512 + 16];
  const int width = st->wide ? 512 : 256;
  for (int i = 0; i < width + 16; i++) samples[i] = backdrop;

  render_display(f, Y, spr, backdrop, samples);

  /* RefreshBorder writes lineSize*(BORDER_WIDTH + HAdjust + borderExtra) border
   * samples and the display follows; RefreshRightBorder then repaints
   * lineSize*(BORDER_WIDTH - HAdjust + borderExtraRight) at the far end.
   * SCREEN 0 passes hAdjustSc0 on the left and -hAdjustSc0 on the right. */
  const int extra_l = (st->mode == AB_MSX_MODE_SCREEN0) ?  st->hadjust_sc0 : 0;
  const int extra_r = (st->mode == AB_MSX_MODE_SCREEN0) ? -st->hadjust_sc0 : 0;
  const int left = line_size * (AB_MSX_BORDER + st->h_adjust + extra_l);

  for (int x = 0; x < out_w; x++) {
    const int s = x - left;
    if (s >= 0 && s < width) row[x] = samples[s];
  }
  for (int off = line_size * (AB_MSX_BORDER - st->h_adjust + extra_r); off > 0; off--) {
    const int x = line_size * AB_MSX_W - off;
    if (x >= 0 && x < out_w) row[x] = backdrop;
  }

  /* A suppressed pixel must reveal BACKGROUND, not a hole: the HD art is drawn
   * over it afterwards and any gap would show through. Re-render the row with
   * the sprite plane withheld and copy those pixels in. */
  if (suppress_row) {
    int any = 0;
    for (int x = 0; x < out_w; x++) if (suppress_row[x]) { any = 1; break; }
    if (any) {
      uint16_t nospr[512 + 16];
      for (int i = 0; i < width + 16; i++) nospr[i] = backdrop;
      render_display(f, Y, NULL, backdrop, nospr);
      for (int x = 0; x < out_w; x++) {
        if (!suppress_row[x]) continue;
        const int s = x - left;
        if (s >= 0 && s < width) row[x] = nospr[s];
      }
    }
  }
}

/* Walk the frame top to bottom, handing each row the sprite buffer built for
 * the PREVIOUS line. That one-line lag is the core's double buffering (see the
 * header note) and the framebuffer contains it, so a match requires it. */
typedef void (*ab_msx_row_sink)(void *ctx, int fy, const uint16_t *row);

/* Test-only hook: set non-zero to composite the CURRENT line's sprites instead
 * of the previous line's. It exists so the acceptance harness can prove the
 * one-line delay matters -- a control that cannot be switched off cannot be
 * shown to fail. Never set in production. */
int ab_msx_test_no_sprite_delay = 0;

/* Resolve the state for ONE framebuffer row. With per-scanline records this
 * follows a mid-frame change exactly; without them every row uses the frame
 * snapshot, which is what the profile did before and is still correct for any
 * frame that never changes VDP state mid-screen.
 *
 * The record is indexed by VDP SCANLINE (fy + displayOffest), the same index
 * the core recorded under -- not by framebuffer row. */
static void resolve_row_state(const ab_msx_frame *f, int fy, ab_msx_frame *out) {
  *out = *f;
  if (!f->have_reglines || ab_msx_test_no_reglines) {
    ab_msx_apply_test_corruptions(out);
    return;
  }
  const int Y = fy + f->st.display_offset;
  if (Y < 0 || Y >= AB_MSX_REGLINES) { ab_msx_apply_test_corruptions(out); return; }
  const ab_msx_regline *rl = &f->reglines[Y];
  /* An unrendered line is all zeroes, and 0 is a LEGAL register value: reading
   * it renders mode 1 with every base at 0 instead of falling back. Same class
   * of bug as the PCE profile's reglines valid bit. */
  if (!rl->valid && !ab_msx_test_reglines_novalid) {
    ab_msx_apply_test_corruptions(out);
    return;
  }

  /* status[2] is recorded as a single byte; splice it into a copy of the frame
   * status so vdpIsOddPage sees the value THIS line had. */
  {
    unsigned char st16[AB_MSX_STATUS];
    memcpy(st16, f->status, AB_MSX_STATUS);
    st16[2] = rl->status2;
    ab_msx_decode(rl->regs, st16, AB_MSX_VRAM_SIZE, &out->st);
  }
  /* The core resolved these for this line; take them rather than re-deriving,
   * per the rule about capturing the resolved value. */
  out->st.first_line     = rl->first_line;
  out->st.active_lines   = rl->active_lines;
  out->st.display_offset = rl->display_offset;

  memcpy(out->palreg, rl->palette, sizeof(rl->palette));
  ab_msx_build_palette(out->palreg, &out->st, out->palette);
  ab_msx_apply_test_corruptions(out);
}

static void walk_rows(const ab_msx_frame *f, const unsigned char *suppress,
                      void *ctx, ab_msx_row_sink sink) {
  unsigned char spr_a[AB_MSX_SPRBUF], spr_b[AB_MSX_SPRBUF];
  uint16_t row[AB_MSX_MAXW];
  unsigned char *cur = spr_a, *prev = spr_b;
  int prev_valid = 0;
  /* ccColorMask starts at 0xff, not 0xf0: it carries the PREVIOUS frame's
   * check mask, and any frame of a game that draws ordinary sprites has it
   * saturated. See the header note. */
  unsigned char cc_mask[2] = { ab_msx_test_ccmask_f0 ? 0xf0 : 0xff, 0xf0 };

  memset(prev, 0, AB_MSX_SPRBUF);

  /* SCREEN 0 has no sprite plane at all -- RefreshLine0 never reads one. That
   * is decided PER ROW below, because the mode itself can change mid-frame. */

  /* VRAM replay. The snapshot is the END of the frame; the log holds every
   * write the frame made, in chronological (hence line) order. Undo it all in
   * reverse to reach the frame-START state, then, walking down the frame,
   * redo the writes each line had already seen. Rows then render from the
   * same bytes the core's own line renderer read -- which is the ONLY way to
   * follow a game that rewrites VRAM mid-frame with constant registers. */
  const unsigned char *vd = NULL;
  int vd_count = 0, vd_idx = 0;
  if (f->have_vdeltas && !ab_msx_test_no_vdeltas) {
    vd = f->vdeltas;
    vd_count = vd[0] | (vd[1] << 8);
    memcpy(f->vram_work, f->vram, AB_MSX_VRAM_SIZE);
    /* Undo ONLY the VRAM entries: register/palette line-START state comes from
     * the reglines records, which are authoritative; those entries exist
     * purely to describe mid-line splits on their own line. */
    for (int i = vd_count - 1; i >= 0; i--) {
      ab_msx_vd_entry e;
      ab_msx_vd_parse(vd, i, &e);
      if (e.kind == AB_MSX_VDELTA_KIND_VRAM)
        f->vram_work[e.addr & (AB_MSX_VRAM_SIZE - 1)] = (unsigned char)e.oldv;
    }
  }

  ab_msx_frame rowf;
  for (int fy = 0; fy < AB_MSX_H; fy++) {
    /* Per-row state. Without reglines this is a straight copy of the frame. */
    resolve_row_state(f, fy, &rowf);
    int split_n = 0;
    ab_msx_vd_entry split_ev[16];
    if (vd) {
      /* Redo every VRAM write this line had already seen (line < Y). Entries
       * are chronological, so line is non-decreasing; one cursor suffices. */
      const int Y = fy + rowf.st.display_offset;
      while (vd_idx < vd_count) {
        ab_msx_vd_entry e;
        ab_msx_vd_parse(vd, vd_idx, &e);
        if (e.line >= Y) break;
        if (e.kind == AB_MSX_VDELTA_KIND_VRAM)
          f->vram_work[e.addr & (AB_MSX_VRAM_SIZE - 1)] = (unsigned char)e.newv;
        vd_idx++;
      }
      /* Collect THIS line's mid-line events (valid dot) for split rendering.
       * Peek without consuming: the cursor advances when Y passes them. */
      if (!ab_msx_test_no_subline) {
        for (int j = vd_idx; j < vd_count && split_n < 16; j++) {
          ab_msx_vd_entry e;
          ab_msx_vd_parse(vd, j, &e);
          if (e.line > Y) break;
          if (e.line == Y && e.dot != AB_MSX_VDELTA_DOT_NONE)
            split_ev[split_n++] = e;
        }

        /* PRE-DISPLAY-WINDOW writes. An entry {line = Y-1, dot = none} is a
         * write that landed AFTER line Y's regline was recorded (the record
         * fires at the empty border prologue) but BEFORE any display block of
         * line Y rendered. The regline therefore holds the PRE-write value
         * while the core rendered the line with the POST-write one:
         * SCREEN 4..8's RefreshLine re-latches vscroll/chrTab mid-line when
         * they changed, and palette entries resolve live in every mode.
         * One SCREEN 7 shooter wobbles R23 in exactly this window on every line; its
         * scattered 12px rows were the reglines being one write behind.
         * SCREEN 0..3 keep their X==-1 latch for registers, so the REG
         * correction applies only to the bitmap modes. */
        int redecode = 0;
        for (int j = vd_idx - 1; j >= 0; j--) {
          ab_msx_vd_entry e;
          ab_msx_vd_parse(vd, j, &e);
          if (e.line < Y - 1) break;
          if (e.line != Y - 1 || e.dot != AB_MSX_VDELTA_DOT_NONE) continue;
          if (e.kind == AB_MSX_VDELTA_KIND_REG &&
              rowf.st.mode >= AB_MSX_MODE_SCREEN4) {
            rowf.regs[e.addr & 63] = (unsigned char)e.newv;
            redecode = 1;
          } else if (e.kind == AB_MSX_VDELTA_KIND_PAL) {
            rowf.palreg[(e.addr & 15) * 2]     = (unsigned char)(e.newv & 0xff);
            rowf.palreg[(e.addr & 15) * 2 + 1] = (unsigned char)(e.newv >> 8);
            redecode = 1;
          }
        }
        if (redecode) {
          /* Re-decode for the table bases and enables, but RESTORE the
           * geometry the regline resolved: firstLine is computed ONCE per
           * frame (onDisplay) and HAdjust only on a mode change, so a
           * mid-frame R18 write must NOT move the picture -- the core's does
           * not. Re-deriving them from the new R18 shifted every subsequent
           * row by the adjust delta (29px rows on the raster-split action-RPG). */
          unsigned char st16[AB_MSX_STATUS];
          const int keep_fl = rowf.st.first_line, keep_al = rowf.st.active_lines;
          const int keep_do = rowf.st.display_offset, keep_ha = rowf.st.h_adjust;
          memcpy(st16, f->status, AB_MSX_STATUS);
          st16[2] = f->status[2];
          ab_msx_decode(rowf.regs, st16, AB_MSX_VRAM_SIZE, &rowf.st);
          rowf.st.first_line = keep_fl;   rowf.st.active_lines = keep_al;
          rowf.st.display_offset = keep_do; rowf.st.h_adjust = keep_ha;
          ab_msx_build_palette(rowf.palreg, &rowf.st, rowf.palette);
        }
      }
      rowf.vram = f->vram_work;
    }
    int valid = 0, spr_done = 0;
    const unsigned char *use = ab_msx_test_no_sprites ? NULL
                             : ab_msx_test_no_sprite_delay
                             ? (valid ? cur : NULL)
                             : (prev_valid ? prev : NULL);
    const unsigned char *srow = suppress ? suppress + (size_t)fy * f->st.out_w : NULL;
    render_row(&rowf, fy, use, srow, row);

    /* SPLIT-ROW rendering: this line's state changed at recorded dot(s). The
     * core rendered blocks [0, dot) with the OLD state and [dot, end) with the
     * NEW one -- vdp_sync advances lines in 8-pixel blocks, so the recorded
     * block boundary is exactly where the core switched. Render the row again
     * after applying each event and stitch from the boundary rightward.
     * One cart's mid-line palette writes and two others' mid-line
     * scroll writes were unreachable any other way: per-line records place the
     * change at a line boundary that the core plainly did not honour. */
    if (split_n > 0) {
      uint16_t seg[AB_MSX_MAXW];
      ab_msx_frame curf = rowf;
      const int ls = rowf.st.wide ? 2 : 1;
      for (int k = 0; k < split_n; k++) {
        const ab_msx_vd_entry *e = &split_ev[k];
        /* SPRITE EVALUATION TIMING. The core fills the sprite buffer for the
         * NEXT line at block 24 (colorSpritesLine, V9938 modes) or when the
         * line's render reaches block 33 (spritesLine at rightBorder, TMS
         * modes). So the sprite state must EXCLUDE this line's events past
         * the fill point: evaluate just before applying the first such
         * event. V9938: dot > 24. TMS: dot >= 33 -- vdp_sync's catch-up can
         * complete the line (render to X2=33, fill included) and leave
         * lineOffset at 33, so a write stamped dot=33 happened AFTER the
         * fill; including it moved one cart's mid-frame sprite slide a line
         * early (2px: old and new X differ by 1, only the edges disagree).
         * Another cart's last 4px were the same class on the V9938 side. */
        if (!spr_done &&
            ((curf.st.sprite_mode2 && e->dot > 24) ||
             (!curf.st.sprite_mode2 && curf.st.mode != AB_MSX_MODE_SCREEN0 &&
              e->dot >= 33))) {
          valid = ab_msx_sprite_line(&curf, fy + rowf.st.display_offset, cur, cc_mask);
          spr_done = 1;
        }
        if (e->kind == AB_MSX_VDELTA_KIND_VRAM) {
          f->vram_work[e->addr & (AB_MSX_VRAM_SIZE - 1)] = (unsigned char)e->newv;
        } else if (e->kind == AB_MSX_VDELTA_KIND_REG) {
          /* Same frame-latch rule as the pre-window path above. */
          unsigned char st16[AB_MSX_STATUS];
          const int keep_fl = curf.st.first_line, keep_al = curf.st.active_lines;
          const int keep_do = curf.st.display_offset, keep_ha = curf.st.h_adjust;
          curf.regs[e->addr & 63] = (unsigned char)e->newv;
          memcpy(st16, f->status, AB_MSX_STATUS);
          ab_msx_decode(curf.regs, st16, AB_MSX_VRAM_SIZE, &curf.st);
          curf.st.first_line = keep_fl;   curf.st.active_lines = keep_al;
          curf.st.display_offset = keep_do; curf.st.h_adjust = keep_ha;
          ab_msx_build_palette(curf.palreg, &curf.st, curf.palette);
          /* A mid-line MODE/width switch cannot be stitched sample-for-sample;
           * keep the pre-switch rendering for that (rare) case. */
          if (curf.st.out_w != rowf.st.out_w) continue;
        } else {   /* AB_MSX_VDELTA_KIND_PAL */
          curf.palreg[(e->addr & 15) * 2]     = (unsigned char)(e->newv & 0xff);
          curf.palreg[(e->addr & 15) * 2 + 1] = (unsigned char)(e->newv >> 8);
          ab_msx_build_palette(curf.palreg, &curf.st, curf.palette);
        }
        render_row(&curf, fy, use, srow, seg);
        /* Stitch boundary. Block D of the VDP's line walk lands at display
         * pixel D*8 -- MINUS the fine horizontal scroll: the bitmap modes'
         * RefreshLine consumes (hScroll & 7) sub-block pixels in its prologue,
         * so every later block boundary sits that many pixels EARLIER in the
         * output row. One fine-scrolled cart's split rows measured a
         * consistent 12px/row miss (8px block + 4px scroll) until this term
         * was applied. Use the CURRENT state's scroll: a scroll write earlier
         * in this very chain changes where later boundaries land. */
        int fine = 0;
        if (curf.st.mode >= AB_MSX_MODE_SCREEN4) fine = curf.st.hscroll & 7;
        int bx = ls * (AB_MSX_BORDER + rowf.st.h_adjust)
               + (e->dot * 8 - fine) * ls;
        if (bx < 0) bx = 0;
        for (int x = bx; x < rowf.st.out_w; x++) row[x] = seg[x];
      }
      /* Evaluate the sprite buffer for the next row with the post-chain state
       * (TMS fill-at-line-end; V9938 handled at block 24 above). */
      if (!spr_done && curf.st.mode != AB_MSX_MODE_SCREEN0) {
        valid = ab_msx_sprite_line(&curf, fy + rowf.st.display_offset, cur, cc_mask);
        spr_done = 1;
      }
    }
    if (!spr_done && rowf.st.mode != AB_MSX_MODE_SCREEN0)
      valid = ab_msx_sprite_line(&rowf, fy + rowf.st.display_offset, cur, cc_mask);

    /* FRAME-END-CUT RETENTION. Rows at and beyond the cut were never
     * re-rendered by this frame; the core's framebuffer keeps whatever an
     * earlier frame left there. Substitute the CALLER'S OWN prior composite
     * for exactly that region -- see the header note for why this is
     * state-faithful and not the banned final-plane shortcut. The prior-row
     * store is refreshed with what we emit, so a caller drawing every frame
     * converges to the core precisely. */
    if (f->have_cut && !ab_msx_test_no_retention) {
      const int Yc = fy + rowf.st.display_offset;
      const int ow = rowf.st.out_w;
      int bx = -1;                       /* first substituted x; -1 = none */
      if (Yc > f->cut_line) bx = 0;
      else if (Yc == f->cut_line) {
        const int ls2 = rowf.st.wide ? 2 : 1;
        bx = ls2 * (AB_MSX_BORDER + rowf.st.h_adjust)
           + f->cut_offset * 8 * ls2;
        if (bx < 0) bx = 0;
      }
      if (bx >= 0) {
        /* PREFERRED: the core fossil snapshot -- always describes the frame
         * being rendered, works on the FIRST compose, cannot go stale. */
        int done = 0;
        if (f->have_fbtail && !ab_msx_test_no_fbtail) {
          const int frows = f->fbtail[4] | (f->fbtail[5] << 8);
          const int fw = f->fbtail[6] | (f->fbtail[7] << 8);
          const int frow0 = f->fbtail[8] | (f->fbtail[9] << 8);
          const int idx = fy - frow0;
          if (idx >= 0 && idx < frows) {
            const unsigned char *fp = f->fbtail + AB_MSX_FBTAIL_HDR
              + ((size_t)idx * AB_MSX_FBTAIL_W + bx) * 2;
            const int xe = ow < fw ? ow : fw;
            for (int x = bx; x < xe; x++, fp += 2)
              row[x] = (uint16_t)(fp[0] | (fp[1] << 8));
            done = 1;
          }
        }
        /* FALLBACK: the caller's own prior composite -- exact ONLY for a
         * caller that composes every frame (ctest; never the live bezel,
         * whose ticks are per-compose). */
        if (!done && f->prev_rows && f->retain_valid) {
          const uint16_t *prow = f->prev_rows + (size_t)fy * AB_MSX_MAXW;
          for (int x = bx; x < ow; x++) row[x] = prow[x];
        }
      }
    }
    if (f->prev_rows) {
      const int ow = rowf.st.out_w;
      uint16_t *prow = f->prev_rows + (size_t)fy * AB_MSX_MAXW;
      for (int x = 0; x < ow; x++) prow[x] = row[x];
    }
    sink(ctx, fy, row);
    unsigned char *t = prev; prev = cur; cur = t;
    prev_valid = valid;
  }
}

/* --- RGBA render (the acceptance gate) ----------------------------------- */
typedef struct { uint32_t *out; int w; } rgba_ctx;

static void rgba_sink(void *vctx, int fy, const uint16_t *row) {
  rgba_ctx *c = (rgba_ctx *)vctx;
  uint32_t *dst = c->out + (size_t)fy * c->w;
  for (int x = 0; x < c->w; x++) dst[x] = ab_msx_rgba565(row[x]);
}

int ab_msx_render_rgba(const ab_msx_frame *f, uint32_t *out) {
  if (!f || !f->vram || !out) return 0;
  rgba_ctx c; c.out = out; c.w = f->st.out_w;
  walk_rows(f, NULL, &c, rgba_sink);
  return 1;
}

/* --- quad emit ------------------------------------------------------------
 * Run-coalesced solid quads. Long flat runs are the common case on MSX (the
 * backdrop alone is most of a frame), so the quad count stays far below the
 * pixel count.
 */
typedef struct {
  ab_batch *b;
  const ab_msx_view *v;
  int quads;
  int w;
  double px;   /* logical width of ONE sample -- halved in the 512-wide modes */
} emit_ctx;

static void emit_sink(void *vctx, int fy, const uint16_t *row) {
  emit_ctx *c = (emit_ctx *)vctx;
  const double y = c->v->oy + fy * c->v->scale;
  int x = 0;
  while (x < c->w) {
    const uint16_t col = row[x];
    int x1 = x + 1;
    while (x1 < c->w && row[x1] == col) x1++;
    ab_batch_solid(c->b,
                   c->v->ox + x * c->px, y,
                   (x1 - x) * c->px, c->v->scale,
                   ab_msx_rgba565(col));
    c->quads++;
    x = x1;
  }
}

int ab_msx_emit(ab_batch *b, const ab_msx_frame *f, const ab_msx_view *v,
                const unsigned char *suppress) {
  if (!b || !f || !v || !f->vram) return 0;
  emit_ctx c;
  c.b = b; c.v = v; c.quads = 0; c.w = f->st.out_w;
  /* HOW WIDE IS ONE SAMPLE?
   *
   * Two legitimate answers, and picking one silently broke the other:
   *
   *   fit_width = 0 (default): one sample = `scale` logical pixels. The frame
   *     occupies out_w * scale, so a 544-wide SCREEN 6/7 frame is twice as
   *     wide on screen as a 272-wide one -- which is what a caller that
   *     computed `scale` FROM the real width (out_w) already expects.
   *
   *   fit_width = 1: the old behaviour -- squeeze any frame into the same
   *     AB_MSX_W * scale footprint, so 512-wide modes draw at half scale and
   *     line up with the narrow modes in a fixed-size window.
   *
   * The emitter used to force the second unconditionally. A bezel that sized
   * its layout from ab.game_width() (544) and asked for scale 3 therefore got
   * a picture drawn at 1.5 -- half the width it had reserved -- and every
   * scored column past the first sampled the wrong place. Measured: 0 of 6
   * wide-mode carts passed, scoring 24%-94% depending on how much of the row
   * happened to be flat. Narrow modes were unaffected because out_w ==
   * AB_MSX_W makes the ratio 1.
   *
   * This is the same width-source mismatch class as ab_pce_view.fb_width: the
   * consumer knew the real width, the API silently used a different one. */
  c.px = v->fit_width ? (v->scale * (double)AB_MSX_W / (double)f->st.out_w)
                      : v->scale;
  walk_rows(f, suppress, &c, emit_sink);
  return c.quads;
}

/* --- sprite marking ------------------------------------------------------ */
int ab_msx_mark_sprites(const ab_msx_frame *f, const ab_registry *reg,
                        unsigned char *suppress,
                        const ab_sub_rule **out_rule, ab_msx_bounds *out_bounds) {
  if (!f || !reg || !suppress || !f->vram) return 0;

  const ab_msx_state *st = &f->st;
  const uint32_t vm = st->vram_mask;
  const int mode2 = st->sprite_mode2;
  /* The two sprite planes disagree on where the attribute table starts, on the
   * list terminator, and on how far the "early clock" bit shifts. */
  const int32_t attr_base = mode2 ? (st->spr_tab & 0x1fe00)
                                  : (st->spr_tab & (int32_t)~0x7fu);
  const int terminator = mode2 ? AB_MSX_SPR_END2 : AB_MSX_SPR_END;
  const int size = ab_msx_sprite_size(f);
  /* In 16x16 mode the hardware ignores the pattern id's low TWO bits. */
  const int pattern_mask = st->sprites_16 ? 0xfc : 0xff;
  const int line_size = st->wide ? 2 : 1;

  int marked = 0;
  int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
  const ab_sub_rule *rule = NULL;

  for (int i = 0; i < AB_MSX_SPRITES; i++) {
    const int32_t a = attr_base + i * 4;
    const int sy = mode2 ? vram_read(f, a) : f->vram[a & vm];
    if (sy == terminator) break;   /* terminator, not a skip */

    const int pid = mode2 ? vram_read(f, a + 2) : f->vram[(a + 2) & vm];
    const int tile = pid & pattern_mask;
    const ab_sub_rule *m = ab_registry_match_tile(reg, tile);
    if (!m) continue;
    rule = m;

    /* Screen position: attribute y is one LESS than the first visible line
     * (y=255 puts the top row on line 0). The early-clock bit lives in the
     * per-sprite colour byte in mode 1 and in the per-LINE colour byte in mode
     * 2; row 0's byte is the right one to ask for the sprite's origin. */
    const int cbyte = mode2
        ? vram_read(f, st->spr_tab & (int32_t)((~0x3ffu) | (uint32_t)(i * 16)))
        : f->vram[(a + 3) & vm];
    const int sx = (mode2 ? vram_read(f, a + 1) : f->vram[(a + 1) & vm])
                 - ((cbyte >> 2) & 0x20);
    const int top = sy + 1;

    for (int yy = top; yy < top + size; yy++) {
      const int fy = yy - st->display_offset + st->first_line;
      if (fy < 0 || fy >= AB_MSX_H) continue;
      unsigned char *r = suppress + (size_t)fy * st->out_w;
      for (int xx = sx; xx < sx + size; xx++) {
        const int base = line_size * (AB_MSX_BORDER + st->h_adjust) + xx * line_size;
        for (int k = 0; k < line_size; k++) {
          const int dx = base + k;
          if (dx >= 0 && dx < st->out_w) r[dx] = 1;
        }
      }
    }
    marked++;

    /* Bounds come only from anchoring patterns: shadow/filler sprites must be
     * suppressed but must not stretch the replacement art. */
    if (ab_registry_tile_anchors(m, tile)) {
      if (sx < x0) x0 = sx;
      if (top < y0) y0 = top;
      if (sx + size > x1) x1 = sx + size;
      if (top + size > y1) y1 = top + size;
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
