/*
 * ab_nes.h -- NES console profile for the Active Bezel render kit.
 *
 * The kit (ab_render.h) owns platform-blind mechanics: atlas, quad batching,
 * substitution registry, render targets. THIS file owns the NES specifics:
 * which regions mean what, how a palette index becomes RGB, and the priority
 * rules that decide whether a pixel is background or sprite.
 *
 * Everything here is transcribed from the Lua renderer that scores
 * 1229/1229 carts exact. That renderer
 * remains the reference implementation and the acceptance gate; this is the
 * same decisions in C, emitting GPU geometry instead of pixel blobs.
 *
 * FACTS THAT COST REAL TIME TO LEARN -- do not "simplify" these:
 *
 *  - fceumm emits 256x224, NOT 240, and the crop is SYMMETRIC: visible lines
 *    are PPU scanlines 8..231. Right height with no offset scores a perfect
 *    match at the wrong place.
 *  - nes_sprdrawn is fceumm's own resolved sprite layer, one byte per pixel:
 *    0x80 = transparent, otherwise the palette VALUE with bit 6 set for a
 *    behind-background sprite. It subsumes flips, bank selection, the 8-per-
 *    line cutoff, the left-8 clip, the compositing gate AND a retained buffer
 *    that sprite evaluation never recorded.
 *  - nes_bgval is the resolved background VALUE per pixel. Values carry
 *    fceumm's bit-6 transparency flag (Pal[0] |= 64 while rendering), so any
 *    lookup table MUST be indexed over the full 0..255 byte range with the
 *    mask baked in. A 64-entry table returns garbage for flagged bytes.
 *  - Emphasis and greyscale are PER SCANLINE (PPUMASK), not per frame. Baking
 *    a frame-end value tinted whole screens wrongly and scored WORSE than
 *    ignoring emphasis entirely.
 *  - The palette is SWAPPABLE: nes_palrgb is the core's ACTIVE table (VS
 *    System carts select rp2c03/rp2c04). Never bake an NTSC table in.
 */
#ifndef AB_NES_PROFILE_H
#define AB_NES_PROFILE_H

#include <stdint.h>
#include "ab_render.h"

enum {
  AB_NES_W = 256,
  AB_NES_H = 224,
  AB_NES_OVERSCAN_TOP = 8,   /* visible = PPU scanlines 8..231 */
  AB_NES_LINES = 240,
  AB_NES_SPR_TRANSPARENT = 0x80,
  AB_NES_SPR_BEHIND = 0x40
};

/* Region handles the profile needs. Resolve once in init; a missing optional
 * region is tolerated and the profile degrades rather than crashing. */
typedef struct {
  int32_t chr;        /* nes_chr        pattern tables */
  int32_t palette;    /* nes_palette    32 bytes, BG $00-0F + sprite $10-1F */
  int32_t palrgb;     /* nes_palrgb     the core's ACTIVE RGB table (64 * 3) */
  int32_t oam;        /* nes_oam        64 * 4 */
  int32_t ppureg;     /* nes_ppu_regs   4 bytes, [1] = PPUMASK */
  int32_t masklines;  /* nes_masklines  PPUMASK per scanline */
  int32_t bgval;      /* nes_bgval      resolved BG value per pixel */
  int32_t bgpix;      /* nes_bgpix      resolved BG INDEX per pixel (opacity) */
  int32_t sprdrawn;   /* nes_sprdrawn   resolved sprite layer per pixel */
} ab_nes_regions;

/* Per-frame snapshot. One bulk read per region beats 57k per-pixel calls;
 * this is the struct that makes that explicit. Buffers are caller-owned. */
typedef struct {
  unsigned char *bgval;      /* 256 * 240 */
  unsigned char *bgpix;      /* 256 * 240, may be NULL if region absent */
  unsigned char *sprdrawn;   /* 256 * 240 */
  unsigned char masklines[AB_NES_LINES];
  unsigned char palette[32];
  unsigned char palrgb[192];
  unsigned char oam[256];
  int have_masklines;
  int have_palrgb;
  int have_bgpix;
} ab_nes_frame;

/* Read this frame's state. Returns 1 on success. */
int ab_nes_frame_read(const ab_nes_regions *r, ab_nes_frame *f);

/* --- colour --------------------------------------------------------------
 * Palette index -> 0xRRGGBBAA, honouring greyscale (PPUMASK bit0 ANDs the
 * INDEX with $30, and it applies to the backdrop too) and emphasis
 * (PPUMASK bits 5-7, fceumm's ApplyDeemphasisClassic multipliers).
 *
 * `value` may carry the bit-6 flag; it is masked here so callers cannot get
 * it wrong.
 */
uint32_t ab_nes_rgba(const ab_nes_frame *f, int value, int mask);

/* Build a 256-entry LUT for one (emphasis, greyscale) state -- indexed over
 * the FULL byte range so flagged values resolve correctly. */
void ab_nes_build_lut(const ab_nes_frame *f, int mask, uint32_t out_lut[256]);

/* PPUMASK for a scanline (per-line if masklines is present). */
int ab_nes_line_mask(const ab_nes_frame *f, int ppu_line, int frame_mask);

/* --- priority ------------------------------------------------------------
 * A sprite-layer byte is drawn when it is not transparent AND, if it is a
 * behind-background sprite, the background pixel at that position is
 * transparent (fceumm CopySprites: `if (!(t & 0x40) || (P[n] & 0x40))`).
 *
 * BG transparency comes from nes_bgpix (resolved index; pattern entry in the
 * low 2 bits, zero = transparent), NOT from bgval's bit 6. bgval's flag is
 * only maintained while the background is RENDERING; on a line with BG
 * disabled it reports opaque and hides a behind-sprite the hardware draws.
 * This is the Section Z (mask $1a) / RoadBlasters (mask $10) 2px defect the
 * Lua reference fixed the same way -- and the C port re-introduced by
 * transcribing the older rule. Fall back to bgval bit 6 only when bgpix is
 * absent.
 */
static inline int ab_nes_spr_visible(unsigned char spr, unsigned char bgpix,
                                     int have_bgpix, unsigned char bgval) {
  if (spr == AB_NES_SPR_TRANSPARENT) return 0;
  if (spr & AB_NES_SPR_BEHIND) {
    if (have_bgpix) return (bgpix & 0x03) == 0;
    return (bgval & 0x40) != 0;
  }
  return 1;
}

/* --- emit ----------------------------------------------------------------
 * Draw one frame as GPU geometry. Background and sprite layers are emitted as
 * run-coalesced quads (long runs are the common case: flat colour spans), so
 * a frame is a couple of mesh calls rather than a 57k-pixel upload.
 *
 * `sub` may be NULL. When present, sprite-layer pixels belonging to a
 * registered rule are SUPPRESSED here -- the original never reaches the
 * picture, so nothing has to be erased afterwards. Suppression is driven by a
 * caller-built mask (the profile does not read OAM tile ids itself; that is
 * game policy and lives in ab_nes_mark_sprites).
 */
typedef struct {
  double ox, oy;   /* top-left of the game view in logical coords */
  double scale;
} ab_nes_view;

/* Emit the background layer. Returns quads drawn. */
/* Which background pixels an emit pass covers. The PPU draws the level
 * geometry and the empty sky as ONE layer, so a filter over the finished
 * picture can never treat them differently -- splitting here is the whole
 * point of redrawing from machine state. */
enum { AB_NES_BG_ALL = 0, AB_NES_BG_EMPTY = 1, AB_NES_BG_SOLID = 2 };

/* As below, but emits only the selected class of background pixel. */
int ab_nes_emit_background_sel(ab_batch *b, const ab_nes_frame *f,
                               const ab_nes_view *v, int frame_mask,
                               const unsigned char *suppress, int bg_select);

/* `suppress` (may be NULL): per-pixel mask, non-zero = do not emit. Lets a
 * guest carve specific background tiles OUT of the redraw entirely. */
int ab_nes_emit_background(ab_batch *b, const ab_nes_frame *f,
                           const ab_nes_view *v, int frame_mask,
                           const unsigned char *suppress);

/* Emit the sprite layer. `suppress` is an optional 256*240 byte mask; a
 * non-zero entry means "this pixel belongs to replacement art, skip it". */
int ab_nes_emit_sprites(ab_batch *b, const ab_nes_frame *f,
                        const ab_nes_view *v, int frame_mask,
                        const unsigned char *suppress);

/* --- OAM helpers ---------------------------------------------------------
 * Mark the pixels of every OAM sprite whose tile id matches a registry rule,
 * and report the live bounds of the matching sprites that ANCHOR (shadow and
 * filler tiles are suppressed but must not stretch the art -- the
 * filler-tile trap).
 *
 * Sprite height is 8 or 16 from PPUCTRL bit5. OAM y is one less than screen y.
 * Returns the number of sprites marked; bounds are only valid if > 0.
 */
typedef struct {
  int x0, y0, x1, y1;   /* screen-space, exclusive on x1/y1 */
} ab_nes_bounds;

int ab_nes_mark_sprites(const ab_nes_frame *f, const ab_registry *reg,
                        int sprite_height, unsigned char *suppress,
                        const ab_sub_rule **out_rule, ab_nes_bounds *out_bounds);

#endif /* AB_NES_PROFILE_H */
