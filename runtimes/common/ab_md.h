/*
 * ab_md.h -- Genesis/SMS/GG console profile for the Active Bezel render kit.
 *
 * Same division of labour as ab_nes.h: the kit (ab_render.h) owns the
 * platform-blind mechanics, this file owns what the gpgx resolved-layer
 * capture means. The Lua reference renderer that scores 100.000% on the
 * corpus is the oracle; this is the
 * same decisions in C.
 *
 * FACTS THAT COST REAL TIME -- do not "simplify":
 *
 *  - md_objpix is TWO bytes per pixel, little-endian: low byte = the
 *    composed line-buffer code AFTER the sprite write (the core's lut
 *    already resolved priority), high byte = 1 where a non-transparent
 *    sprite pixel exists. A presence flag is REQUIRED because every 8-bit
 *    code value is legal on this VDP -- the NES 0x80 sentinel trick does
 *    not port.
 *  - Colour comes ONLY from md_pixrgb (the core's own pixel[] table,
 *    RGB565 in this build) widened EXACTLY as the host widens:
 *    r = (r5 << 3) | (r5 >> 2), g = (g6 << 2) | (g6 >> 4),
 *    b = (b5 << 3) | (b5 >> 2). Verified byte-exact against the core
 *    framebuffer before anything was built on it. The emulator owns the
 *    palette; there is no "standard" 3-bit expansion here.
 *  - Per-line width/height come from md_linestate (16 B/line):
 *    [0]=reg0 [1]=reg1 [2]=reg7 [3]=reg12 [4]=reg17 [5]=reg18
 *    [6..7]=viewport.w LE [8]=viewport.x [9]=system_hw. Lines = 240 when
 *    reg1 bit3 (V30) else 224. H32/H40 mid-frame switches are real; read
 *    the width per line.
 *  - reg0 bit5 blanks the LEFTMOST 8 pixels to code 0x40 (backdrop). This
 *    is the ONE hardware rule applied here; everything else is already
 *    resolved into the planes.
 *  - SHADOW/HIGHLIGHT (reg12 bit3) is resolved during the core's OBJ merge,
 *    so bgpix codes are UN-shadowed. The compose base is therefore
 *    md_linepix (post-merge, S/H state included); bgpix is only the
 *    fallback for pixels whose sprite was SUPPRESSED by substitution
 *    (their shadow state is unknowable and the HD art covers them).
 *    Composing bg+obj alone rendered every S/H game one brightness band
 *    off (Space Harrier 2's whole sky, most sports menus).
 *  - Sprite SUBSTITUTION marks via the SAT (sprite attribute table) in
 *    VRAM, which gpgx stores WORD-BYTE-SWAPPED on this host -- read u16 as
 *    (vram[addr] | vram[addr^1] << 8) with the XOR, not straight LE.
 */
#ifndef AB_MD_PROFILE_H
#define AB_MD_PROFILE_H

#include <stdint.h>
#include "ab_render.h"

enum {
  AB_MD_MAX_W = 320,
  AB_MD_MAX_H = 240,
  AB_MD_LEFT_BLANK_CODE = 0x40
};

typedef struct {
  int32_t linepix;    /* md_linepix   FINAL composed code per pixel */
  int32_t bgpix;      /* md_bgpix     BG-only composed code per pixel */
  int32_t objpix;     /* md_objpix    2B/px: presence<<8 | composed code */
  int32_t pixrgb;     /* md_pixrgb    pixel[] table, 256 * 2 (RGB565) */
  int32_t linestate;  /* md_linestate 240 * 16 per-line reg snapshot */
  int32_t pixlines;   /* md_pixlines  240 * 512 per-LINE pixel[] table */
  int32_t vram;       /* video_ram    64KB, word-byte-swapped (SAT walks) */
  int32_t vdpregs;    /* genesis_vdp_regs 32B live (SAT base, sprite size) */
} ab_md_regions;

typedef struct {
  unsigned char *linepix;    /* 240 * 320 */
  unsigned char *bgpix;      /* 240 * 320 */
  unsigned char *objpix;     /* 240 * 320 * 2 */
  unsigned char linestate[AB_MD_MAX_H * 16];
  unsigned char pixrgb[512];
  unsigned char *pixlines;   /* 240 * 512, may be NULL */
  int have_pixlines;
} ab_md_frame;

int ab_md_frame_read(const ab_md_regions *r, ab_md_frame *f);

/* Frame geometry from the captured per-line state. */
int ab_md_lines(const ab_md_frame *f);              /* 224 or 240 */
int ab_md_line_width(const ab_md_frame *f, int y);  /* 256 / 320 (160 GG) */

/* RGB565 -> 0xRRGGBBAA with the host's exact widening. */
uint32_t ab_md_rgba(const ab_md_frame *f, int code);
void ab_md_build_lut(const ab_md_frame *f, uint32_t out_lut[256]);

/* The composition the corpus certified: sprite pixel if present, else BG,
 * with the reg0 left-blank override applied by the emitter (not here). */
static inline int ab_md_pixel_code(const ab_md_frame *f, int y, int x) {
  const int o = (y * AB_MD_MAX_W + x) * 2;
  if (f->objpix[o + 1]) return f->objpix[o];
  return f->bgpix[y * AB_MD_MAX_W + x];
}

typedef struct {
  double ox, oy;
  double scale;
} ab_md_view;

/* Emit the frame as run-coalesced solid quads. `suppress` is an optional
 * 320*240 byte mask: nonzero = "sprite pixel replaced by HD art, show the
 * BG pixel instead" (the draw-something-else seam). Returns quads. */
int ab_md_emit(ab_batch *b, const ab_md_frame *f, const ab_md_view *v,
               const unsigned char *suppress);

/* Walk the SAT and mark every sprite whose FIRST tile id matches a registry
 * rule: suppress its pixels (sprite-presence only) and report the union
 * bounds of anchoring sprites. `vram_read` order handles the word swap.
 * Returns sprites marked. */
typedef struct { int x0, y0, x1, y1; } ab_md_bounds;

int ab_md_mark_sprites(const ab_md_regions *r, const ab_md_frame *f,
                       const ab_registry *reg, unsigned char *suppress,
                       const ab_sub_rule **out_rule, ab_md_bounds *out_bounds);

#endif /* AB_MD_PROFILE_H */
