/*
 * ab_gb.h -- Game Boy / Game Boy Color console profile for the Active Bezel
 * render kit.
 *
 * The kit (ab_render.h) owns platform-blind mechanics: atlas, quad batching,
 * substitution registry, render targets. THIS file owns the GB specifics:
 * which regions mean what, how a captured pixel becomes RGB, and the priority
 * rules that decide whether a pixel is background or sprite.
 *
 * Everything here is transcribed from the Lua renderer that scores 1396/1396
 * loadable carts exact. That renderer
 * remains the reference implementation and the acceptance gate; this is the
 * same decisions in C, emitting GPU geometry instead of a pixel blob.
 *
 * FACTS THAT COST REAL TIME TO LEARN -- do not "simplify" these:
 *
 *  - gambatte's RGB565 packs a FIVE-bit green at <<6, not six bits at <<5
 *    (video_libretro.cpp:309: `rFinal << 11 | gFinal << 6 | bFinal`). Bit 5
 *    is therefore permanently 0 and white is 0xFFDF, NOT 0xFFFF. A renderer
 *    that emits "correct" RGB565 green is wrong on every pixel of every cart.
 *
 *  - The palette arrives ALREADY RESOLVED in gb_palline: gambatte's own
 *    video_pixel_t entries, post gbcToRgb32 / post DMG remap. Do NOT convert
 *    bgr15 here. That transform is runtime CONFIGURATION (correction on/off,
 *    fast vs "gold standard", dark filter, DMG colorization) and its default
 *    mode is float std::pow gamma that a guest cannot reproduce bit-exactly.
 *    The emulator owns the palette -- ratified as the template boundary.
 *
 *  - DMG has no bgr15 at all. dmgColorsGBC_ is memset to zero and only filled
 *    on the CGB-running-a-DMG-game path; a real DMG's colours are already
 *    RGB565 in dmgColorsRgb32_. The cgb/dmgMode flags in gb_lineregs[9] say
 *    which encoding produced the table, and the profile needs no branch
 *    because both arrive as final RGB565.
 *
 *  - CGB sprite overlap is resolved by LOWEST OAM INDEX per pixel (ppu.cpp
 *    keeps an `idtab` and gates each write on `id < idt[pos]`), NOT by merge
 *    order as on DMG. The capture is written INSIDE that guard, so gb_sprpix
 *    already carries the winner. Do NOT re-sort sprites here.
 *
 *  - 0 is a legal colour, not a "no data" sentinel. The per-pixel colour
 *    planes must be gated on the VALID bit (0x80) in gb_bgpix/gb_sprpix, or
 *    an unwritten pixel renders black. This cost 10 carts a clean 0.000%.
 *
 *  - A blanked frame (LCD off) is a whole-screen solid fill of ONE colour,
 *    parked by the capture in palline entry 0. It is flagged per line in
 *    gb_lineregs[10]. Rendering the last live picture instead is the stale-
 *    row trap.
 *
 *  - Geometry is 160x144 with NO overscan crop and no left-column clip. The
 *    whole framebuffer scores.
 */
#ifndef AB_GB_PROFILE_H
#define AB_GB_PROFILE_H

#include <stdint.h>
#include "ab_render.h"

enum {
  AB_GB_W = 160,
  AB_GB_H = 144,
  AB_GB_PIX = AB_GB_W * AB_GB_H,

  /* gb_lineregs: 16 bytes per scanline. */
  AB_GB_LR_STRIDE = 16,
  AB_GB_LR_LCDC   = 0,
  AB_GB_LR_SCX    = 1,
  AB_GB_LR_SCY    = 2,
  AB_GB_LR_WX     = 3,
  AB_GB_LR_WY     = 4,
  AB_GB_LR_WINY   = 8,
  AB_GB_LR_FLAGS  = 9,
  AB_GB_LR_BLANK  = 10,
  AB_GB_LR_F_CGB      = 0x01,
  AB_GB_LR_F_DMGMODE  = 0x02,

  /* gb_palline: 32 BG + 32 OBJ entries, u16 LE, already final RGB565. */
  AB_GB_PAL_STRIDE = 128,

  /* gb_bgpix / gb_sprpix bit layout. */
  AB_GB_PIX_ENTRY = 0x03,
  AB_GB_PIX_PAL   = 0x1C,   /* >> 2 */
  AB_GB_PIX_WIN   = 0x20,   /* bgpix only: window(1) vs background(0) */
  AB_GB_PIX_PRIO  = 0x40,   /* bgpix only: CGB BG tile-attr priority */
  AB_GB_PIX_SPRIO = 0x20,   /* sprpix only: sprite attr bgpriority */
  AB_GB_PIX_VALID = 0x80
};

/* Region handles the profile needs. Resolve once in bind; an absent optional
 * region degrades rather than crashing. */
typedef struct {
  int32_t lineregs;   /* gb_lineregs   144 * 16 */
  int32_t bgpix;      /* gb_bgpix      resolved BG/window inputs per pixel */
  int32_t sprpix;     /* gb_sprpix     resolved sprite inputs per pixel */
  int32_t palline;    /* gb_palline    resolved palettes per line */
  int32_t bgcol15;    /* gb_bgcol15    resolved BG colour per pixel (u16) */
  int32_t sprcol15;   /* gb_sprcol15   resolved OBJ colour per pixel (u16) */
  int32_t oam;        /* gb_oam        40 * 4, for sprite marking */
} ab_gb_regions;

/* Per-frame snapshot. One bulk read per region; the per-byte path is not
 * something this profile should ever exercise. Buffers are caller-owned. */
typedef struct {
  unsigned char *bgpix;      /* AB_GB_PIX */
  unsigned char *sprpix;     /* AB_GB_PIX */
  unsigned char *bgcol15;    /* AB_GB_PIX * 2, u16 LE */
  unsigned char *sprcol15;   /* AB_GB_PIX * 2, u16 LE */
  unsigned char lineregs[AB_GB_H * AB_GB_LR_STRIDE];
  unsigned char palline[AB_GB_H * AB_GB_PAL_STRIDE];
  unsigned char oam[160];
  int have_percolour;   /* bgcol15 + sprcol15 present */
  int have_oam;
} ab_gb_frame;

/* Read this frame's state. Returns 1 on success. */
int ab_gb_frame_read(const ab_gb_regions *r, ab_gb_frame *f);

/* --- per-line state ------------------------------------------------------ */
static inline int ab_gb_line_flags(const ab_gb_frame *f, int y) {
  return f->lineregs[(size_t)y * AB_GB_LR_STRIDE + AB_GB_LR_FLAGS];
}
/* True CGB rendering: the cgb flag set AND not a DMG game on the CGB core. */
static inline int ab_gb_line_cgb(const ab_gb_frame *f, int y) {
  const int fl = ab_gb_line_flags(f, y);
  return (fl & AB_GB_LR_F_CGB) != 0 && (fl & AB_GB_LR_F_DMGMODE) == 0;
}
static inline int ab_gb_line_blank(const ab_gb_frame *f, int y) {
  return f->lineregs[(size_t)y * AB_GB_LR_STRIDE + AB_GB_LR_BLANK] != 0;
}
/* LCDC bit0. On CGB this is the OBJ-over-BG master priority enable; on DMG it
 * means "BG off", which the core already applied at the emit site by forcing
 * the tile entry to 0. Same bit, two meanings -- the classic GB trap. */
static inline int ab_gb_line_master_prio(const ab_gb_frame *f, int y) {
  return (f->lineregs[(size_t)y * AB_GB_LR_STRIDE + AB_GB_LR_LCDC] & 0x01) != 0;
}

/* --- colour --------------------------------------------------------------
 * Palette entries are ALREADY final RGB565 (see the header note); the only
 * work here is widening to RGBA8888 by replicating high bits.
 */
uint16_t ab_gb_pal_entry(const ab_gb_frame *f, int line, int obj, int idx);
uint32_t ab_gb_rgba565(uint16_t c565);

/* --- priority ------------------------------------------------------------
 * Whether the sprite pixel wins at this position. Transcribed from
 * ppu.cpp:398 (DMG) / :593 (CGB) / :779 (cycle-stepped):
 *   DMG: show unless the sprite's bgpriority is set AND the BG entry != 0.
 *   CGB: additionally the BG TILE's attr bit7 forces it behind; LCDC bit0
 *        clear disables the whole rule (sprites win everywhere).
 * Sprite entry 0 is transparent and is never captured, so entry == 0 means
 * "no sprite here".
 */
static inline int ab_gb_spr_visible(unsigned char bp, unsigned char sp,
                                    int cgb, int master_prio) {
  if ((sp & AB_GB_PIX_VALID) == 0) return 0;
  if ((sp & AB_GB_PIX_ENTRY) == 0) return 0;
  if (!master_prio) return 1;
  if (cgb)
    return ((sp & AB_GB_PIX_SPRIO) == 0 && (bp & AB_GB_PIX_PRIO) == 0)
           || (bp & AB_GB_PIX_ENTRY) == 0;
  return (sp & AB_GB_PIX_SPRIO) == 0 || (bp & AB_GB_PIX_ENTRY) == 0;
}

/* --- emit ---------------------------------------------------------------- */
typedef struct {
  double ox, oy;   /* top-left of the game view in logical coords */
  double scale;
} ab_gb_view;

/* Emit the background/window layer as run-coalesced solid quads. */
int ab_gb_emit_background(ab_batch *b, const ab_gb_frame *f,
                          const ab_gb_view *v);

/* Emit the sprite layer. `suppress` is an optional AB_GB_PIX byte mask; a
 * non-zero entry means "this pixel belongs to replacement art, skip it". */
int ab_gb_emit_sprites(ab_batch *b, const ab_gb_frame *f,
                       const ab_gb_view *v, const unsigned char *suppress);

/* --- OAM helpers ---------------------------------------------------------
 * GB OAM is 40 entries of 4 bytes: y, x, tile, attr. Screen position is
 * y-16 and x-8 (an object at the top-left corner has OAM y=16, x=8), which
 * is NOT the NES convention -- do not copy that offset.
 *
 * Sprite height is 8 or 16 from LCDC bit 2. In 8x16 mode the tile id's low
 * bit is ignored by the hardware.
 */
typedef struct {
  int x0, y0, x1, y1;   /* screen-space, exclusive on x1/y1 */
} ab_gb_bounds;

int ab_gb_mark_sprites(const ab_gb_frame *f, const ab_registry *reg,
                       int sprite_height, unsigned char *suppress,
                       const ab_sub_rule **out_rule, ab_gb_bounds *out_bounds);

/* Sprite height for a line, from LCDC bit 2. */
static inline int ab_gb_sprite_height(const ab_gb_frame *f, int y) {
  return (f->lineregs[(size_t)y * AB_GB_LR_STRIDE + AB_GB_LR_LCDC] & 0x04) ? 16 : 8;
}

#endif /* AB_GB_PROFILE_H */
