/*
 * ab_msx.h -- MSX / MSX2 console profile for the Active Bezel render kit.
 *
 * The kit (ab_render.h) owns platform-blind mechanics: atlas, quad batching,
 * substitution registry, render targets. THIS file owns the MSX specifics:
 * which regions mean what, how VDP state becomes a picture, and the priority
 * rule that decides whether a pixel is background or sprite.
 *
 * WHAT MAKES MSX DIFFERENT FROM EVERY OTHER PROFILE HERE
 *   NES/GB/MD/SNES expose RESOLVED per-pixel planes that the core already
 *   composited -- those profiles read a decision the emulator made. bluemsx
 *   exposes only RAW VDP STATE (VRAM + registers). This profile therefore
 *   RE-RUNS the VDP's line renderer: name table -> pattern -> colour, plus the
 *   sprite line buffer, per scanline. Every rule below is transcribed from
 *   bluemsx Src/VideoChips/Common.h (RefreshLine1/2/3) and SpriteLine.h
 *   (spritesLine), which is what actually runs -- NOT from a wiki. Where the
 *   two disagree, the core wins.
 *
 * SCOPE (measured, not claimed)
 *   COVERED: SCREEN 0 (Text 1, 40 columns), the TMS9918 graphics modes
 *   SCREEN 1 (Graphic I), SCREEN 2 (Graphic II), SCREEN 3 (Multicolour), and
 *   the V9938 bitmap modes SCREEN 4, 5, 6, 7 and 8. Sprite mode 1 (the TMS
 *   plane, 4 per line) AND sprite mode 2 (the V9938 plane: 8 per line,
 *   per-line colour bytes, CC OR-blending). Scored against the core's own
 *   framebuffer over a 816-ROM corpus -- see ab_msx.c for the exact numbers.
 *   NOT COVERED: TEXT80 (SCREEN 0 width 80, bluemsx mode 13), and the YJK
 *   modes SCREEN 10/12 (R25 bit3 set), which decode Y/J/K into RGB through a
 *   precomputed yjkColor table rather than the palette. Those report
 *   AB_MSX_MODE_UNSUPPORTED so a bezel can degrade instead of drawing
 *   confident nonsense. The mode-select combinations "SCREEN 0+2" and
 *   "SCREEN 0+3" also report UNSUPPORTED, matching the core: on a V9938/V9958
 *   (which is what romdev runs) those select RefreshLineBlank, not a picture.
 *
 * FACTS THAT COST REAL TIME TO LEARN -- do not "simplify" these:
 *
 *  - RETENTION HAS TWO SOURCES, AND ONLY ONE IS LIVE-SAFE. Rows at/beyond
 *    the frame-end cut are fossil framebuffer memory no state-only renderer
 *    can produce. (a) prev_rows -- the caller's own prior composite -- is
 *    exact ONLY for a caller that composes EVERY frame (the C harness).
 *    Bezel ticks fire per COMPOSE, not per frame, so live there is either no
 *    prior (first compose: retention silently inert) or a STALE one, which
 *    CORRUPTS active rows below the cut: one wide-mode frame, cut=216 with the
 *    212-line display running to line 226, lost rows 216..226 to a stale
 *    composite -- 6512 wrong pixels, 95.012%. (b) msx_fb_tail -- the core's
 *    own fossil-row snapshot, captured at retro_run end from its single
 *    persistent framebuffer -- always describes the frame being rendered and
 *    is the ONLY correct live source. It IS core-resolved pixels: a narrow,
 *    flagged deviation, cut-gated and control-covered (ab_msx_test_no_fbtail,
 *    control 21), same class as PCE linepix. Never arm prev_rows in a
 *    per-compose consumer.
 *
 *  - msx_fb_tail MUST AGREE WITH msx_vram_deltas ON THE CUT. Both are
 *    stamped from the same struct at retro_run end; a mismatch means the two
 *    region reads straddled a frame boundary and the snapshot's pixel rows
 *    belong to a different frame than the state describes. frame_read
 *    rejects the snapshot then (have_fbtail=0) instead of trusting it.
 *
 *  - THE EXPOSED msx_palette IS NOT ALWAYS THE PALETTE THAT RENDERS. At VDP
 *    reset the core fills vdp->palette[] (what draws) from a HARDCODED table
 *    (msx1Palette/msx2Palette in VDP.c), while paletteReg (what msx_palette
 *    reports) is memcpy'd from a DIFFERENT constant, defaultPaletteRegs. They
 *    disagree. Only once the BIOS writes the palette port do the two agree --
 *    measured at ~frame 13 on this core; frames 0..10 render from the reset
 *    table and CANNOT be reconstructed from the exposed registers at all.
 *    Every real game frame is post-BIOS, so reading msx_palette is right; a
 *    bezel that draws during the first ~13 frames of boot will be wrong and
 *    there is no exposed flag to detect it. Do not "fix" this by baking the
 *    TMS9918 table in -- that breaks every post-BIOS frame instead.
 *
 *  - MSX1 games do NOT get the MSX1 palette. romdev runs machine "MSX2+"
 *    (V9958), so a TMS9918-era cart in SCREEN 2 renders through the MSX2
 *    default palette. The famous TMS9918 colours (62,184,73 green ...) never
 *    appear. Deriving colour from paletteReg gets this right automatically;
 *    hardcoding "the MSX1 palette" is wrong on every pixel of every cart.
 *
 *  - COLOUR 0 IS THE BACKDROP, NOT BLACK. When transparency is on (R8 bit5
 *    clear, non-bitmap mode) the core points palette[0] at palette[BGColor]
 *    (vdp_updateOutputMode). A frame with R7=0xe4 renders its colour-0 pixels
 *    BLUE, not black. Skipping this scored 27% on an otherwise perfect frame.
 *
 *  - THE SPRITE PLANE IS ONE SCANLINE LATE, BY CONSTRUCTION. RefreshLineN
 *    starts a line with getSpritesLine(Y), which returns lineBufs[(Y&1)^1] --
 *    the buffer spritesLine() filled at the END of line Y-1. So line Y
 *    composites the sprites EVALUATED FOR LINE Y-1. This is emulator double-
 *    buffering, not hardware, but it is what the framebuffer contains, so a
 *    reconstruction must reproduce it. Costs ~0.1% of pixels if you skip it --
 *    small enough to look like noise and never get diagnosed.
 *
 *  - SCREEN 2's THREE-THIRDS ADDRESSING IS THE CLASSIC TRAP. The pattern and
 *    colour lookups share one index built as
 *        base  = (-1 << 13) | ((y & 0xc0) << 5) | (y & 7)
 *        index = base | (name * 8)
 *    then masked SEPARATELY by chrGenBase and colTabBase. (y & 0xc0) << 5 is
 *    what splits the screen into three independent 256-tile banks. SCREEN 1
 *    uses a completely different colour lookup -- colTabBase & (name/8 | ~63),
 *    one colour byte per EIGHT tiles -- and no thirds at all.
 *
 *  - THE MODE-DEPENDENT ADDRESS MASKS ARE NOT DECORATIVE. Each base is built
 *    as (reg << shift) | ~(-1 << shift), i.e. the low bits are forced to ONES,
 *    and then used as an AND-mask against the index. It reads like an OR-base
 *    but behaves as a mask: chrGenBase 0x3fff masks nothing, so a "wrong
 *    pattern base" corruption can be a genuine no-op. Reproduce the &, do not
 *    "simplify" it to a +.
 *
 *  - SPRITE ATTRIBUTE y == 208 TERMINATES THE LIST. Not "is offscreen" -- the
 *    scan BREAKS, so sprites after it never draw however valid they look. (The
 *    V9938 mode-2 sentinel is 216, a different value; this profile is mode 1.)
 *
 *  - SPRITES ARE DRAWN LOWEST-INDEX-LAST. spritesLine walks the visible list
 *    in REVERSE (while (visibleCnt--)), so sprite 0 overwrites sprite 31. Draw
 *    them in list order and every overlap is inverted.
 *
 *  - THE 4-SPRITE-PER-LINE LIMIT IS REAL AND MUST BE APPLIED. TMS9918 mode
 *    stops at 4 per line (V9938 mode-2 stops at 8). The 5th sprite sets status
 *    bit 0x40 + its index in S#0's low 5 bits; collision sets bit 0x20. Status
 *    is READ-ONLY here -- reconstructing it would fight the core.
 *
 *  - A COLOUR-0 SPRITE IS INVISIBLE BUT STILL COLLIDES. Unless R8 bit5
 *    (colour-0-solid) is set, colour 0 sprites write no pixels while still
 *    marking the collision buffer. Rendering them paints black blobs.
 *
 *  - GEOMETRY: the framebuffer is 272x240 = 8px border + 256 display + 8px
 *    border, 240 lines of which only 192 (or 212 with R9 bit7) are display.
 *    The active area starts at line firstLine = displayOffest + (212?14:24) +
 *    VAdjust, where displayOffest is 27 on PAL and 0 on NTSC. Everything
 *    outside is backdrop. Assuming "256x192 at (8,24)" happens to be right for
 *    NTSC/192 and silently wrong for PAL or 212-line.
 *
 *  - SCREEN 6 AND 7 ARE 544 PIXELS WIDE, NOT 272. They are the 512-dot modes,
 *    and the libretro shim's frameBufferSetDoubleWidth sets
 *    image_buffer_current_width = base*2 for the WHOLE image buffer, so the
 *    frontend reports width 544 and does NO downsampling. The border doubles
 *    with it: RefreshBorder emits lineSize*(BORDER_WIDTH+HAdjust) samples with
 *    lineSize 2. Reconstructing these at 272 and scoring against the 544-wide
 *    truth compares misaligned data and reports ~30% on a renderer that is in
 *    fact exact -- it looks like a deep pixel bug and is only a geometry bug.
 *
 *  - SCREEN 0's HORIZONTAL ADJUST IS VDP-VERSION DEPENDENT. RefreshLine0 passes
 *    vdp->hAdjustSc0 to RefreshBorder as borderExtra, and vdpCreate sets that
 *    to -2 on the TMS9918 variants but +1 on the V9938/V9958. romdev runs
 *    machine MSX2+ (a V9958), so +1 applies; taking the TMS value shifts every
 *    character three pixels left and costs the entire text area. SCREEN 0 also
 *    forces cells 0 and 31 to BGColor, draws SIX-pixel characters two at a time
 *    from an 8-bit pattern (so cell and pattern boundaries do not align), reads
 *    its name table at 0xc00 + 40*(y/8) + x masked by (-1 << 12), and takes
 *    hScroll MODULO 6 rather than masking it.
 *
 *  - MODES 7..12 ADDRESS VRAM INTERLEAVED. MAP_VRAM rewrites the address as
 *    (addr >> 1) | ((addr & 1) << 16) whenever screenMode is 7..12, i.e. even
 *    bytes live in the low 64K and odd bytes in the high 64K. SCREEN 7/8's line
 *    renderers spell the same thing out by hand with vdp->vram128 (0x10000 when
 *    the VDP has >=128K). Skipping the interleave turns every SCREEN 7/8 pixel
 *    into noise rather than producing a subtle error.
 *
 *  - SCREEN 8 DOES NOT USE THE PALETTE REGISTERS AT ALL. Background pixels go
 *    through paletteFixed[256] (a GRB332 table where BLUE is 2 bits with an
 *    (i&3)==3 -> 7 special case, NOT a linear ramp) and sprite pixels through a
 *    separate fixed paletteSprite8[16]. Its backdrop is paletteFixed[R7], the
 *    whole register, not the low nibble.
 *
 *  - THE V9938 SPRITE PLANE (MODE 2) DIFFERS FROM MODE 1 IN SIX WAYS, all of
 *    which matter: the list terminator is 216 (not 208); the per-line limit is
 *    8 (not 4); the colour is a PER-LINE byte read from
 *    sprTabBase & ((-1 << 10) | (sprite * 16 + spriteLine)), not a per-sprite
 *    attribute; the written value is ((colour & 0x0f) << 1) | solidColor, so
 *    the line renderers un-shift it with col >> 1; colour bit 0x40 marks a CC
 *    sprite that ORs into the line instead of overwriting it, and is skipped
 *    outright when it is the first visible sprite; and horizontalPos is
 *    x + 24 (plus 8 more for 16x16), not x + 32.
 *
 *  - ccColorMask IS A STATIC THAT SURVIVES ACROSS FRAMES. colorSpritesLine
 *    keeps ccColorMask/ccColorCheckMask as function statics: at each frame
 *    start ccColorMask takes the PREVIOUS frame's ccColorCheckMask and
 *    ccColorCheckMask resets to 0xf0, and ccColorCheckMask becomes 0xff as soon
 *    as any non-CC sprite with a visible colour is seen. Any frame of a game
 *    that draws ordinary sprites therefore runs with ccColorMask == 0xff.
 *    Initialising it to 0xf0 masks the low nibble off every CC sprite, so CC
 *    sprites OR in nothing and their colour silently never appears.
 *
 *  - A C-BIOS BIOS SCREEN IS NOT A PASSING FRAME. romdev runs the open C-BIOS
 *    machine. Mapper selection comes from blueMSX's ROM database
 *    (<systemDir>/Databases/msxromdb.xml, keyed by <hash algo="sha1">); once
 *    that ships, mapped commercial carts boot normally. What C-BIOS still
 *    CANNOT do is run a BASIC ROM -- it has no BASIC interpreter -- so those
 *    print "Cannot execute a BASIC ROM" forever. In a 250-ROM sample 10 ROMs
 *    are BASIC and 22 render a flat screen. In every case loadMedia reports
 *    success and the core renders a clean frame, so the failure is INVISIBLE
 *    to a harness that does not read the pixels. Those frames are TRIVIAL to
 *    reproduce (one or two colours) and scoring them inflates any corpus-wide
 *    percentage; the first pass of this profile scored 630 such frames at a
 *    perfect 100% and they were 30% of the denominator. Any harness must
 *    classify frames by what the GAME drew -- read the SCREEN 0/1 name table,
 *    where C-BIOS's tile indices are ASCII, and look for the message -- and
 *    exclude them. Counting distinct colours is NOT a boot check: an error
 *    screen has two, and so do plenty of real title screens. Test FLATNESS
 *    FIRST, before any name-table reading, so a one-colour frame can never be
 *    rescued into a pass. And give the ROM a generous boot budget before
 *    calling it failed: at 1800 frames three ROMs in a 250-ROM sample were
 *    still on a blank screen that resolved into a real game by 5400.
 *
 *  - THE CORE FREE-RUNS BETWEEN TOOL CALLS, SO A MULTI-CALL CAPTURE CAN BE
 *    SELF-INCONSISTENT. Reading VRAM and then taking a screenshot are two
 *    separate calls; on a title that is still animating they come from
 *    DIFFERENT frames and no renderer can reconcile them. Measured: two
 *    back-to-back VRAM reads with NO stepping already differ on such a ROM,
 *    while a genuinely idle screen returns byte-identical VRAM ten times
 *    running. This reads exactly like a renderer bug -- Zaxxon scored 89.6%
 *    mid-scroll and 100.0000% once its VRAM was verified stable. Gate captures
 *    on THREE identical consecutive VRAM reads; two can match by luck on a
 *    game that rewrites VRAM on a cycle. Gate on VRAM, not on the picture: the
 *    screen can look static while VRAM churns.
 *
 *  - THE EMITTER'S SAMPLE WIDTH IS A POLICY, AND THE DEFAULT MUST BE "ONE
 *    SAMPLE = ONE `scale`". ab_msx_emit used to unconditionally squeeze every
 *    mode into AB_MSX_W * scale, so a 544-wide SCREEN 6/7 frame drew at HALF
 *    the requested scale. A bezel that sized its layout from the real width
 *    (ab.game_width() -> 544) and asked for scale 3 reserved 1632 logical
 *    pixels and got 816; every scored column past the first then sampled the
 *    wrong place. Measured: 0 of 6 wide-mode carts passed, scoring 24%-94%
 *    depending on how flat the rows happened to be, while narrow modes were
 *    untouched because out_w == AB_MSX_W makes the ratio 1. ab_msx_view.
 *    fit_width restores the squeeze for a fixed-size window; it must stay OFF
 *    for anything compared against the core's framebuffer.
 *
 *    THIS IS THE THIRD WIDTH-SOURCE MISMATCH IN THIS KIT -- ab_pce_view.
 *    fb_width, ab_pce_emit calling render_line instead of render_fb_row, and
 *    this. The pattern is always the same: the consumer knows the real width,
 *    the API quietly uses a different one, nothing errors, and the score just
 *    sits lower than it should. When a whole geometry class fails at a 0%%
 *    rate, suspect the width source before the pixels.
 *
 *  - A PROFILE THAT IS CORRECT IS NOT A BEZEL THAT IS CORRECT. The C harness
 *    feeds captured buffers straight into this file; a real bezel goes
 *    Lua -> ab_msx_lua.c -> ab_region_find -> draw. Both layers silently
 *    dropped msx_vdp_reglines: the Lua binding never resolved it, AND
 *    active-bezels/src/Regions.js listed regions only up to 0x1c5 so the name
 *    could not resolve even once the binding asked. Because the no-reglines
 *    path is byte-identical to the pre-per-line renderer, nothing errored --
 *    the bezel just rendered the old, worse picture. Measured through the real
 *    host: one cart read 43.692% before, 100.000% after. ALWAYS score through a
 *    bezel; ctest is a component test, not the acceptance gate.
 *
 *  - FOLLOW THE REGISTERS PER SCANLINE, NOT PER FRAME. The core records what
 *    each line actually rendered with into msx_vdp_reglines (256 records of
 *    {regs[64], palette[16], status2, valid, line, firstLine, activeLines,
 *    displayOffest}, written at the top of each line in vdp_sync). A frame the
 *    game changes partway down the screen holds TWO states while a vblank
 *    snapshot holds one. Measured: one cart scored 43.69% from a snapshot and
 *    99.82% from the per-line records; another went 82.64% -> 99.90%.
 *    The PALETTE matters as much as the registers -- the raster-split cart varies
 *    only paletteReg and R16 across its 1012 changed line-entries, not the mode
 *    bits. `valid` is load-bearing: an unrendered line is all zeroes and 0 is a
 *    LEGAL register value, so trusting an unwritten record renders mode 1 with
 *    every table base at 0 instead of falling back. The profile degrades to the
 *    frame snapshot when the region is absent, which is right for the majority
 *    of frames and the only option on an older core.
 *
 *  - PER-LINE REGISTERS DO NOT FIX MID-FRAME *VRAM* MUTATION. A game that
 *    rewrites tile or name-table bytes while the beam is running produces a
 *    picture drawn from two VRAM states, and the register record cannot
 *    describe that. Measured on the residual failures: 13 of 16 show NO
 *    per-line register or palette variation at all, yet still miss -- Car
 *    Fighter changes 158 VRAM bytes every frame with completely constant
 *    registers. Capturing that would need a per-line VRAM snapshot, which is a
 *    far larger region than a 256x106 record.
 *
 *  - A MID-FRAME RASTER SPLIT CANNOT BE RECONSTRUCTED FROM A SNAPSHOT ALONE.
 *    Some games rewrite VDP state partway down the screen (a status bar in a
 *    different mode, a mid-screen palette or scroll change). The framebuffer
 *    then holds TWO states while a once-per-frame snapshot holds one. The tell
 *    is a CONTIGUOUS band of wrong rows with 100.0000% everywhere outside it.
 *    A stable split reproduces itself every frame, so a frame-identity filter
 *    does NOT catch it -- but the per-line records above DO, and that cart
 *    4 is now pixel-exact on all three dumps.
 *
 *    CORRECTION worth keeping: it was first diagnosed as a
 *    mid-frame MODE change, because rows 30..130 showed colours that only
 *    SCREEN 8's fixed palette produces while the vblank registers said
 *    SCREEN 5. The per-line records disprove that: the mode selector is 0x03
 *    (SCREEN 5) on EVERY line, and what actually changes is paletteReg on 92
 *    lines. The "SCREEN 8 colours" were SCREEN 5 pixels resolving through a
 *    palette the snapshot had already missed. Do not infer a mode change from
 *    colours alone -- read the recorded mode.
 *
 *  - UNVERIFIED: THE ODD-PAGE RULE IS IMPLEMENTED BUT UNTESTED. SCREEN 5/6/7/8
 *    mask their char table with ~vdpIsOddPage(vdp) << 7, where vdpIsOddPage
 *    needs BOTH status[2] bit1 and R9 bit2 (interlace). Across the whole
 *    816-ROM corpus NOT ONE frame has R9 bit2 set, so odd_page is always 0 and
 *    a control that forces it to 0 does not move the score by a single pixel.
 *    The code follows the core, but nothing here proves it: treat interlaced
 *    page flipping as untested rather than as a passing rule.
 *
 *  - vdpStatus[2] BIT 6 IS A LIVE FLAG AND MUST NOT BE HONOURED FROM A
 *    SNAPSHOT. colorSpritesLine bails when (vdpStatus[2] & 0x40) is set, but
 *    that bit means "not currently displaying": it is CLEAR while the core
 *    renders active scanlines and SET during vblank. A between-frames snapshot
 *    always reads it SET, so a reconstruction that honours it disables sprites
 *    on every single frame. Test only the persistent enables (R1 bit6 screen
 *    on, R8 bit1 sprites off). vdpStatus[2] bit 1 IS safe to use -- it feeds
 *    vdpIsOddPage, which SCREEN 5/6/7/8 need for their page flipping.
 *
 *  - THE CORE IS RGB565 WITH A TRUE 6-BIT GREEN: (R>>3)<<11 | (G>>2)<<5 |
 *    B>>3 (FrameBuffer.h, VIDEO_COLOR_TYPE_RGB565). This is NOT gambatte's
 *    5-bit-green-at-<<6 quirk -- do not copy the GB profile's widening note.
 *    Palette register -> RGB is per channel with INTEGER truncation:
 *    R = (v & 0x70) * 255 / 112, G = ((v >> 8) & 7) * 255 / 7,
 *    B = (v & 0x07) * 255 / 7. Round instead of truncate and colours drift by
 *    one LSB on about a third of entries.
 */
#ifndef AB_MSX_PROFILE_H
#define AB_MSX_PROFILE_H

#include <stdint.h>
#include "ab_render.h"

enum {
  /* Framebuffer geometry. BORDER_WIDTH 8 + DISPLAY_WIDTH 256 + 8 = 272. */
  AB_MSX_BORDER  = 8,
  AB_MSX_DISPLAY = 256,
  AB_MSX_W       = 2 * AB_MSX_BORDER + AB_MSX_DISPLAY,   /* 272 */
  AB_MSX_H       = 240,
  AB_MSX_PIX     = AB_MSX_W * AB_MSX_H,

  /* SCREEN 6/7 present at DOUBLE width -- the libretro shim widens the whole
   * image buffer, so those frames are 544x240 and are NOT downsampled. Buffers
   * that must hold any mode are sized AB_MSX_MAXW / AB_MSX_MAXPIX. */
  AB_MSX_MAXW   = 2 * AB_MSX_W,                          /* 544 */
  AB_MSX_MAXPIX = AB_MSX_MAXW * AB_MSX_H,

  /* VRAM the profile reads. The V9938/V9958 romdev runs exposes 128KB, and the
   * bitmap modes genuinely use all of it (SCREEN 7/8 address it interleaved
   * across the 64K boundary), so 32KB is NOT enough any more. */
  AB_MSX_VRAM_SIZE = 0x20000,

  AB_MSX_REGS   = 64,
  AB_MSX_STATUS = 16,
  AB_MSX_PALREG = 16,   /* 16 u16 LE palette registers */

  /* Sprite attribute table: 32 entries x 4 bytes {y, x, pattern, colour}. */
  AB_MSX_SPRITES      = 32,
  AB_MSX_SPR_PERLINE  = 4,     /* TMS9918 mode-1 per-line limit */
  AB_MSX_SPR_PERLINE2 = 8,     /* V9938 mode-2 per-line limit */
  AB_MSX_SPR_END      = 208,   /* mode-1 attribute y that TERMINATES the scan */
  AB_MSX_SPR_END2     = 216,   /* mode-2 uses a DIFFERENT terminator */

  /* Sprite line buffer: 384 wide with screen x 0 at index 32, so a sprite
   * hanging off either edge (the -32 "early clock" bit) needs no clamping. */
  AB_MSX_SPRBUF     = 384,
  AB_MSX_SPRBUF_ORG = 32
};

/* Screen modes this profile understands. The value is bluemsx's own
 * screenMode number, so it can be compared against core sources directly. */
enum {
  AB_MSX_MODE_UNSUPPORTED = -1,
  AB_MSX_MODE_SCREEN0 = 0,   /* Text 1       -- 40 cols of 6-pixel characters */
  AB_MSX_MODE_SCREEN1 = 1,   /* Graphic I    -- colour per 8 tiles */
  AB_MSX_MODE_SCREEN2 = 2,   /* Graphic II   -- colour per tile ROW, 3 thirds */
  AB_MSX_MODE_SCREEN3 = 3,   /* Multicolour  -- 4x4 blocks, no pattern bits */
  AB_MSX_MODE_SCREEN4 = 4,   /* Graphic III  -- SCREEN 2 tiles + mode-2 sprites */
  AB_MSX_MODE_SCREEN5 = 5,   /* Graphic IV   -- 256x212 4bpp linear bitmap */
  AB_MSX_MODE_SCREEN6 = 6,   /* Graphic V    -- 512x212 2bpp, presents at 544 */
  AB_MSX_MODE_SCREEN7 = 7,   /* Graphic VI   -- 512x212 4bpp, presents at 544 */
  AB_MSX_MODE_SCREEN8 = 8    /* Graphic VII  -- 256x212 direct GRB332 */
};

/* Region handles the profile needs. Resolve once in bind; an absent optional
 * region degrades rather than crashing. */
typedef struct {
  int32_t vram;      /* msx_vram        VDP VRAM */
  int32_t regs;      /* msx_vdp_regs    64 VDP registers */
  int32_t status;    /* msx_vdp_status  16 status registers (READ-ONLY) */
  int32_t palette;   /* msx_palette     16 u16 LE V9938 palette registers */
  /* msx_vdp_reglines -- OPTIONAL per-scanline record. Set to -1 when the core
   * does not expose it and the profile falls back to the frame snapshot. */
  int32_t reglines;
  /* msx_vram_deltas -- OPTIONAL per-frame VRAM write log; -1 when absent. */
  int32_t vdeltas;
  /* msx_fb_tail -- OPTIONAL core fossil-row snapshot; -1 when absent. */
  int32_t fbtail;
} ab_msx_regions;

/* One per-scanline record, matching the core's struct romdev_msx_regline byte
 * for byte. The core writes this at the TOP of each line it renders, so it is
 * the state that line actually used -- not a value re-derived afterwards. */
enum { AB_MSX_REGLINES = 256, AB_MSX_REGLINE_STRIDE = 106 };

/* msx_vram_deltas -- the per-frame VRAM write log. Header {count u16,
 * truncated u8, pad u8} then AB_MSX_VDELTA_CAP entries of
 * {line u16, oldv u8, newv u8, addr u32}.
 *
 * WHY BOTH VALUES: the consumer holds the END-of-frame VRAM snapshot. To know
 * what line Y rendered from it must UNDO the whole log in reverse (restore
 * oldv) to reach the frame-START state, then REDO entries with line < Y in
 * forward order (apply newv) as it walks down the frame. A new-value-only log
 * cannot reconstruct any earlier state.
 *
 * `truncated` means the log overflowed: a PARTIAL undo would rebuild a
 * frame-start state that never existed, so the consumer must ignore the log
 * entirely and fall back to the snapshot -- visible degradation, never a
 * silently wrong picture. */
enum {
  AB_MSX_VDELTA_CAP   = 8192,
  /* header: {count u16, truncated u8, pad u8, cutLine u16, cutOffset s16}.
   * cutLine/cutOffset are the FRAME-END CUT: the presented framebuffer's
   * pixels at and beyond this position were never re-rendered by this frame --
   * they are retained from an earlier one (the fossil tail, verified
   * pixel-identical to the prior frame). 0xFFFF = unknown (first frame). */
  AB_MSX_VDELTA_HDR   = 8,
  /* {line u16, dot u16, addr u32 (kind in bits 31:30), oldv u16, newv u16}.
   * `dot` is vdp_sync's lineOffset -- one unit = one 8-pixel block of the
   * display area, which is the granularity the core ITSELF renders lines in,
   * so a mid-line split stitched at dot*8 is exact, not approximate.
   * 0xFFFF = whole-line (no split). Values are u16 because palette registers
   * are 9-bit. */
  AB_MSX_VDELTA_ENTRY = 12,
  AB_MSX_VDELTAS_SIZE = AB_MSX_VDELTA_HDR + AB_MSX_VDELTA_CAP * AB_MSX_VDELTA_ENTRY,
  AB_MSX_VDELTA_DOT_NONE = 0xFFFF,
  AB_MSX_CUT_NONE = 0xFFFF,

  /* msx_fb_tail -- core fossil-row snapshot. Header {cutLine u16,
   * cutOffset s16, rows u16, width u16, firstRow u16, pad u16} then
   * AB_MSX_FBTAIL_ROWS rows of AB_MSX_FBTAIL_W u16 RGB565 pixels (each row
   * uses `width` of them). The CORE captures it at the end of retro_run,
   * from its single persistent framebuffer, so rows at/beyond the recorded
   * cut are exactly the fossil content the presentation shows. */
  AB_MSX_FBTAIL_ROWS = 32,
  AB_MSX_FBTAIL_W    = 544,
  AB_MSX_FBTAIL_HDR  = 12,
  AB_MSX_FBTAIL_SIZE = AB_MSX_FBTAIL_HDR + AB_MSX_FBTAIL_ROWS * AB_MSX_FBTAIL_W * 2
};
enum {
  AB_MSX_VDELTA_KIND_VRAM = 0,
  AB_MSX_VDELTA_KIND_REG  = 1,
  AB_MSX_VDELTA_KIND_PAL  = 2
};
#pragma pack(push, 1)
typedef struct {
  unsigned char  regs[AB_MSX_REGS];      /* 64 VDP control registers */
  uint16_t       palette[AB_MSX_PALREG]; /* paletteReg[] for THIS line */
  unsigned char  status2;                /* vdpStatus[2]: bit1 -> vdpIsOddPage */
  unsigned char  valid;                  /* 0 = line did not render this frame */
  uint16_t       line;                   /* the VDP scanline, echoed */
  uint16_t       first_line;             /* resolved display start */
  uint16_t       active_lines;           /* 192 or 212, resolved */
  unsigned char  display_offset;         /* 27 PAL / 0 NTSC */
  unsigned char  pad;
} ab_msx_regline;
#pragma pack(pop)

/* Everything decoded out of the registers once per frame. Kept separate from
 * the raw bytes because every draw call needs it and re-deriving the masks
 * per scanline is exactly the cost this kit exists to avoid. */
typedef struct {
  int mode;             /* AB_MSX_MODE_* */

  /* Table bases. These are AND-MASKS, not additive bases -- see the header
   * note. Built exactly as onScrModeChange does. */
  int32_t chr_tab;      /* name table */
  int32_t chr_gen;      /* pattern generator */
  int32_t col_tab;      /* colour table */
  int32_t spr_tab;      /* sprite attribute table */
  int32_t spr_gen;      /* sprite pattern generator */

  int fg, bg;           /* R7 high/low nibble */
  int screen_on;        /* R1 bit6 */
  int sprites_16;       /* R1 bit1 -- 16x16 instead of 8x8 */
  int sprites_big;      /* R1 bit0 -- 2x magnification */
  int sprites_off;      /* R8 bit1 */
  int color0_solid;     /* R8 bit5 -- colour 0 draws instead of showing backdrop */
  int vscroll;          /* R23 */

  int first_line;       /* first VDP scanline of the active display */
  int active_lines;     /* 192, or 212 when R9 bit7 */
  int display_offset;   /* 27 on PAL, 0 on NTSC */
  int h_adjust;         /* R18 low nibble, sign-extended and negated */

  /* --- V9938 additions --------------------------------------------------- */
  int sprite_mode2;     /* 1 for SCREEN 4..8: the 8-per-line colour plane */
  int wide;             /* 1 for SCREEN 6/7: the frame presents at AB_MSX_MAXW */
  int out_w;            /* AB_MSX_W, or AB_MSX_MAXW when `wide` */
  int hscroll;          /* R26/R27 composed, masked by the 512 bit */
  int hscroll512;       /* R25 bit0 & R2 bit5 -- two-page horizontal scroll */
  int edge_masked;      /* R25 bit1 -- leftmost 8 pixels forced to backdrop */
  int odd_page;         /* vdpIsOddPage: status[2] bit1 + R9 bit2 */
  int hadjust_sc0;      /* SCREEN 0 border extra: +1 on V9938+, -2 on TMS9918 */
  int yjk;              /* R25 bit3 -- SCREEN 10/12; unsupported, but detected */

  uint32_t vram_mask;   /* (vram_pages << 14) - 1 */
  uint32_t vram_acc_mask; /* MAP_VRAM's access mask, from R8 bit3 */
  int      vram_pages;    /* VRAM size >> 14 */
  int32_t  vram128;       /* 0x10000 when the VDP has >= 128K, else 0 */
} ab_msx_state;

/* Per-frame snapshot. One bulk read per region; the per-byte path is not
 * something this profile should ever exercise. Buffer is caller-owned. */
typedef struct {
  unsigned char *vram;                          /* AB_MSX_VRAM_SIZE */
  unsigned char regs[AB_MSX_REGS];
  unsigned char status[AB_MSX_STATUS];
  unsigned char palreg[AB_MSX_PALREG * 2];
  uint16_t      palette[AB_MSX_PALREG];         /* resolved RGB565 */
  ab_msx_state  st;
  int have_status;

  /* Per-scanline records. `have_reglines` is 0 when the core does not expose
   * the region, and the renderer then uses the frame snapshot for every row --
   * which is correct for the (large) majority of frames that never change VDP
   * state mid-screen, and is the ONLY option on an older core. */
  ab_msx_regline *reglines;   /* caller-owned, AB_MSX_REGLINES entries */
  int have_reglines;

  /* VRAM delta log (raw region bytes, AB_MSX_VDELTAS_SIZE) and the mutable
   * working copy the per-line replay renders from (AB_MSX_VRAM_SIZE). Both
   * caller-owned; either NULL disables the replay and the profile renders
   * every row from the end-of-frame snapshot exactly as before. */
  unsigned char *vdeltas;
  unsigned char *vram_work;
  int have_vdeltas;

  /* FRAME-END CUT, parsed from the log header. */
  int cut_line;      /* VDP line where the frame's rendering stopped */
  int cut_offset;    /* block (lineOffset unit) within that line */
  int have_cut;

  /* RETENTION of the never-re-rendered tail.
   *
   * THE DISTINCTION THAT KEEPS THIS LEGAL: this is NOT the banned final-plane
   * shortcut. That read the CORE's resolved output, which can hide renderer
   * bugs. `prev_rows` is the CALLER'S OWN prior composite -- every pixel in it
   * came from THIS renderer on an earlier frame, so a renderer bug still
   * diverges visibly and the captured-buffer gate still catches it. The copy
   * is gated STRICTLY by the recorded cut; it is never a general fallback.
   *
   * Caller-owned buffer of out_w-max * H u16 (AB_MSX_MAXW * AB_MSX_H).
   * walk_rows substitutes prev_rows content for the stale region when
   * retain_valid, and refreshes prev_rows with every row it emits, so a
   * caller that draws every frame converges to the core exactly (the fossil
   * was laid while both were rendering the same frames). retain_valid = 0 on
   * the first frame (nothing to retain yet). */
  uint16_t *prev_rows;
  int retain_valid;

  /* CORE FOSSIL SNAPSHOT (msx_fb_tail raw bytes, AB_MSX_FBTAIL_SIZE),
   * caller-owned; NULL disables it. This is the PREFERRED retention source,
   * and it IS core-resolved pixels -- a deliberate, narrow deviation from the
   * prev_rows rule above, forced by the live compose model: bezel ticks fire
   * per COMPOSE, not per frame, so "our own prior composite" does not exist
   * on a first compose and is STALE on a later one. A stale composite
   * substituted at the cut corrupts active rows (the wide-frame
   * live regression: cut=216 with the 212-line display running to line 226 --
   * rows 216..226 swallowed by a minutes-old compose, 6512 wrong pixels).
   * The core snapshot is always current. It is cut-gated framebuffer MEMORY,
   * the same class as the PCE core's linepix region: used only for pixels no
   * state-only renderer can produce, with a must-fail control
   * (ab_msx_test_no_fbtail) proving exactly which pixels come from it.
   * have_fbtail additionally requires the snapshot's recorded cut to MATCH
   * the vdeltas cut -- a mismatch means the two regions describe different
   * frames and the snapshot must not be trusted. */
  unsigned char *fbtail;
  int have_fbtail;
} ab_msx_frame;

/* Read this frame's state and decode the registers. Returns 1 on success, 0 if
 * a required region is missing. A frame whose mode this profile does not cover
 * still returns 1, with st.mode == AB_MSX_MODE_UNSUPPORTED. */
int ab_msx_frame_read(const ab_msx_regions *r, ab_msx_frame *f);

/* Decode registers into `out`. Split out so a caller holding its own register
 * bytes (a test, a replay) can decode without a live region.
 *
 * `status` may be NULL; it is needed only for vdpIsOddPage (status[2] bit 1),
 * which SCREEN 5/6/7/8 use for page flipping -- pass NULL and those modes
 * behave as if the even page is selected. `vram_size` is the size of the VRAM
 * the caller will supply, and drives vramMask/vram128; pass 0 for the default
 * AB_MSX_VRAM_SIZE. */
void ab_msx_decode(const unsigned char *regs, const unsigned char *status,
                   uint32_t vram_size, ab_msx_state *out);

/* --- colour --------------------------------------------------------------
 * Palette register (u16, 0x0GRB nibble-ish packing) -> RGB565, via the core's
 * exact integer path. See the header note on truncation.
 */
uint16_t ab_msx_palreg_to_565(uint16_t palreg);

/* Build all 16 entries, applying the backdrop rule for entry 0. */
void ab_msx_build_palette(const unsigned char *palreg_bytes,
                          const ab_msx_state *st, uint16_t out[AB_MSX_PALREG]);

/* RGB565 -> 0xRRGGBBAA, the host's widening. */
uint32_t ab_msx_rgba565(uint16_t c565);

/* --- sprites -------------------------------------------------------------
 * Fill `buf` (AB_MSX_SPRBUF bytes, screen x 0 at AB_MSX_SPRBUF_ORG) with the
 * winning sprite pixel, 0 = no sprite. `vdp_line` is the VDP scanline, NOT the
 * framebuffer row. Dispatches on st.sprite_mode2 to the TMS (mode 1) or V9938
 * (mode 2) plane.
 *
 * WHAT THE BYTE MEANS DIFFERS BY MODE, and getting it wrong is silent:
 *   mode 1 -- the value IS the palette index (1..15).
 *   mode 2 -- the value is ((colour & 0x0f) << 1) | color0Solid, exactly as the
 *             core writes it, because the line renderers un-shift it with
 *             `col >> 1`. Do not pre-shift it here.
 *
 * `cc_mask` carries colorSpritesLine's cross-frame ccColorMask/ccColorCheckMask
 * statics: cc_mask[0] is ccColorMask (start a steady-state frame at 0xff, see
 * the header note) and cc_mask[1] is ccColorCheckMask. Pass NULL in mode 1.
 *
 * Returns 1 if any sprite pixel was written, 0 for an empty line. The caller
 * is responsible for the one-line delay (see the header note): the buffer this
 * fills for line Y is what line Y+1 must composite.
 */
int ab_msx_sprite_line(const ab_msx_frame *f, int vdp_line, unsigned char *buf,
                       unsigned char *cc_mask);

/* --- emit ---------------------------------------------------------------- */
typedef struct {
  double ox, oy;   /* top-left of the game view in logical coords */
  double scale;
  /* 0 (default): one sample = `scale` logical pixels, so the frame occupies
   * out_w * scale and a 544-wide SCREEN 6/7 frame is physically wider than a
   * 272-wide one. This is what a caller that derived `scale` from the REAL
   * frame width expects, and it is what a pixel-exact scorer requires.
   *
   * 1: squeeze every mode into the same AB_MSX_W * scale footprint (512-wide
   * modes draw at half scale). Useful for a fixed-size bezel window; wrong for
   * anything comparing against the core's framebuffer. */
  int fit_width;
} ab_msx_view;

/* Emit the whole picture (border + background + sprites) as run-coalesced
 * solid quads. MSX has no separate "background layer" worth splitting: the
 * sprite plane is composited into the same scanline walk the VDP does, so one
 * pass is both faster and exactly what the hardware produces.
 *
 * `suppress` is an optional byte mask of st.out_w * AB_MSX_H entries (so
 * AB_MSX_MAXPIX is always big enough); a non-zero entry means "this pixel
 * belongs to replacement art, skip the SPRITE there and show background".
 * Pass NULL for none. Returns quads drawn.
 */
int ab_msx_emit(ab_batch *b, const ab_msx_frame *f, const ab_msx_view *v,
                const unsigned char *suppress);

/* Render the frame into a caller-owned RGBA8888 buffer. The frame is
 * st.out_w x AB_MSX_H, which is 272x240 for most modes and 544x240 for
 * SCREEN 6/7 -- size the buffer AB_MSX_MAXPIX to be safe for any mode.
 * This is the ACCEPTANCE-GATE path -- it is what gets scored byte-for-byte
 * against the core's framebuffer. The quad emitter above must agree with it. */
int ab_msx_render_rgba(const ab_msx_frame *f, uint32_t *out);

/* --- sprite attribute helpers -------------------------------------------
 * Mark the pixels of every sprite whose PATTERN id matches a registry rule,
 * and report the bounds of the matching sprites that ANCHOR (shadow/filler
 * patterns are suppressed but must not stretch the replacement art).
 *
 * Sprite size is 8 or 16 from R1 bit1, doubled again by R1 bit0. In 16x16 mode
 * the hardware ignores the pattern id's low TWO bits (mask 0xfc) -- not one bit
 * like the GB, and not zero like the NES.
 */
typedef struct {
  int x0, y0, x1, y1;   /* screen-space, exclusive on x1/y1 */
} ab_msx_bounds;

/* --- test-only hooks -----------------------------------------------------
 * Each disables ONE documented rule so the acceptance suite can prove that
 * rule is load-bearing: a control that does NOT drop the score means the rule
 * is untested, not that it is right. Never set any of these in production.
 */
extern int ab_msx_test_no_sprite_delay; /* composite THIS line's sprites */
extern int ab_msx_test_no_interleave;   /* skip MAP_VRAM's mode 7..12 rewrite */
extern int ab_msx_test_no_sprites;      /* composite no sprite plane at all */
extern int ab_msx_test_ccmask_f0;       /* start ccColorMask at 0xf0, not 0xff */
extern int ab_msx_test_spr4_limit;      /* mode-1 4-per-line cap in mode 2 */
extern int ab_msx_test_sprend_208;      /* mode-1 terminator in mode 2 */
extern int ab_msx_test_no_reglines;     /* ignore the per-scanline records */
extern int ab_msx_test_reglines_novalid;/* trust reglines without the valid bit */
extern int ab_msx_test_no_vdeltas;      /* ignore the VRAM delta log */
extern int ab_msx_test_no_subline;      /* per-line application only: no splits */
extern int ab_msx_test_no_retention;    /* never substitute prior rows at the cut */
extern int ab_msx_test_no_fbtail;       /* ignore the core fossil snapshot only */
/* Applied to the state each ROW renders with -- corrupting the frame state is
 * not enough once per-row resolution overwrites it. */
extern int ab_msx_test_corrupt_chrtab, ab_msx_test_corrupt_coltab;
extern int ab_msx_test_corrupt_sprgen, ab_msx_test_corrupt_hscroll;
extern int ab_msx_test_corrupt_edge, ab_msx_test_corrupt_sc0adjust;
extern int ab_msx_test_corrupt_sprmode1, ab_msx_test_corrupt_bgtransparent;
extern int ab_msx_test_corrupt_palette;

int ab_msx_mark_sprites(const ab_msx_frame *f, const ab_registry *reg,
                        unsigned char *suppress,
                        const ab_sub_rule **out_rule, ab_msx_bounds *out_bounds);

/* Sprite cell size in pixels, magnification included. */
static inline int ab_msx_sprite_size(const ab_msx_frame *f) {
  return (f->st.sprites_16 ? 16 : 8) * (f->st.sprites_big ? 2 : 1);
}

/* Is this frame something the profile can draw? */
static inline int ab_msx_supported(const ab_msx_frame *f) {
  return f->st.mode != AB_MSX_MODE_UNSUPPORTED;
}

#endif /* AB_MSX_PROFILE_H */
