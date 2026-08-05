/*
 * ab_pce.h -- PC Engine / TurboGrafx-16 console profile for the Active Bezel
 * render kit.
 *
 * Same division of labour as ab_nes.h / ab_gb.h: the kit (ab_render.h) owns
 * the platform-blind mechanics, this file owns the PCE specifics.
 *
 * BUT THE SHAPE OF THE JOB IS DIFFERENT, AND THAT IS THE HEADLINE:
 *
 *   Every other profile READS RESOLVED PLANES. fceumm hands us nes_sprdrawn,
 *   gambatte hands us gb_bgpix/gb_sprpix, gpgx hands us md_linepix -- the
 *   emulator already decided every pixel and we only widen colour.
 *
 *   geargrafx exposes NO resolved planes. The six pce_* regions are RAW CHIP
 *   STATE: VDC VRAM, the sprite attribute table, the 20 VDC registers and the
 *   VCE colour table. So this profile RE-RUNS THE VDC's line renderer --
 *   BAT fetch, tile decode, sprite fetch, priority merge -- for every scanline
 *   of every frame. It is a reimplementation, not a widening pass.
 *
 *   Consequence: every decision below is transcribed line-by-line from
 *   geargrafx's own huc6270.cpp (RenderBackground / RenderSprites /
 *   FetchSprites) and huc6260.cpp (InitPalettes). Where a wiki and the core
 *   disagree, the core wins -- it is what actually runs. Verified at
 *   99.90% / 99.71% exact against the core's own framebuffer on two
 *   independent Blazing Lazers frames (see the "residual" note at the end).
 *
 * FACTS THAT COST REAL TIME TO LEARN -- do not "simplify" these:
 *
 *  - THE CORE RUNS RGB565, NOT RGBA8888. geargrafx builds THREE palettes
 *    (huc6260.cpp InitPalettes) and the RGBA8888 one expands each 3-bit
 *    component as `c * 255 / 7`. That is the OBVIOUS transform and it is
 *    WRONG here: this build renders through m_rgb565_palette, which
 *    quantises to 5/6/5 FIRST (`r*31/7`, `g*63/7`, `b*31/7`) and the host
 *    then widens back to 8 bits. The two differ on most colours -- e.g. the
 *    green channel takes 0/36/73/109/146/182/219/255, values that simply do
 *    not exist under *255/7. Using the "correct" RGBA8888 expansion scored
 *    73% and every ship pixel was wrong. Cost: a long detour. The decisive
 *    test is cheap -- histogram the framebuffer's channel values and check
 *    whether green lands on the 6-bit ladder.
 *
 *  - THE VCE ENTRY IS 9-BIT **GGGRRRBBB** -- green in the HIGH bits, then
 *    red, then blue. Not RGB order. Swapping G and R still renders a
 *    plausible-looking picture (it scored 65%), so this will NOT announce
 *    itself as obviously broken; it just looks slightly wrong forever.
 *
 *  - pce_vdc_regs IS THE LIVE REGISTER FILE, NOT THE LATCHED COPY.
 *    GetState()->R points at m_register[20] -- what the CPU last wrote. The
 *    VDC renders from m_latched_cr / m_latched_mwr / m_latched_bxr, latched
 *    at HSYNC / VSW. A game that disables sprites in vblank (Blazing Lazers
 *    writes CR=0x8E) leaves the live CR reading "sprites off" for a frame
 *    that plainly HAS sprites. Trust the live registers for GEOMETRY, but
 *    treat the CR enable bits as a hint the caller may override -- see
 *    ab_pce_view.force_sprites / force_bg.
 *
 *  - ab_pce_emit MUST GO THROUGH ab_pce_render_fb_row, NOT ab_pce_render_line.
 *    render_line produces only the VDC's ACTIVE WINDOW and knows nothing about
 *    where that window sits inside the framebuffer row; render_fb_row applies
 *    the x-offset (or the recorded xofflines/srclines placement) and fills the
 *    border. An earlier emit called render_line directly, so every game whose
 *    VDC window is wider than the VCE line -- the 341-wide set -- rendered 8px
 *    out of place: seven carts in the corpus read 45.8%..97.3%, while every
 *    256-wide game was exact.
 *
 *    THIS HID BEHIND THE TEST HARNESS FOR A LONG TIME. The C harness scores
 *    through render_fb_row and passes the true framebuffer width via an env
 *    var, so it read 100.000% on the very games a real bezel rendered at 71%.
 *    A harness that takes a different code path than the shipping consumer is
 *    not testing the shipping consumer. The bezel sweep is the acceptance
 *    gate; ctest is a component test.
 *
 *  - ab_pce_view.fb_width IS THE CORE'S LINE WIDTH, NOT THE VDC WINDOW, AND IT
 *    IS INDEPENDENTLY LOAD-BEARING. With emit fixed to use render_fb_row,
 *    withholding fb_width still leaves two carts at 93.84% and
 *    99.08% (both exact once it is supplied), because the
 *    fallback re-derives the width from the VDC registers and the two clock
 *    domains disagree. One cart happens to be exact either way -- so testing the
 *    parameter on that cart alone reports a working control as dead. Measure a
 *    control on a game the parameter actually moves.
 *
 *  - THE FRAMEBUFFER-ROW -> RASTER-LINE OFFSET IS **NOT A CONSTANT**, AND
 *    BELIEVING IT WAS COST THE WHOLE CORPUS. An earlier revision of this file
 *    said "framebuffer row y is VDC raster line y + 8", measured on two
 *    Blazing-Lazers screens where it peaked sharply at +8. It is +8 for those
 *    screens and for many games. Sweeping the bias per game across the
 *    80-title library found frames that are EXACTLY 100% at +0 for some
 *    carts, at +8 for others and at +17 for at least one. Two measurements
 *    agreeing is not a hardware invariant; it is two games that happened to
 *    program the same VDS.
 *
 *    The offset is set by where the VDC's display window (VDW) starts
 *    relative to the VCE's first copied scanline, and the two clocks are not
 *    locked: HuC6270 gates m_active_line on (m_vpos >= 14 && m_vpos < 256)
 *    while VDW begins after VSW+1 + VDS+2 lines. When VDW starts LATER than
 *    vpos 14 the picture is CLIPPED, not shifted, so even the register
 *    arithmetic (VSW+1 + VDS+2 - 14) does not describe it -- verified: it
 *    predicts +14 for three games measured at exactly +0.
 *
 *    So the mapping is not computed at all any more, it is RECORDED. The
 *    per-line record carries the m_vpos each raster line rendered on, and
 *    framebuffer row R is the line whose vpos == R + ab_pce_vpos_origin(H).
 *    That origin is ITSELF not a constant -- it is m_scanline_start + 14, and
 *    m_scanline_start is a core option (11 at 224p, 2 at 240p, else 0), so it
 *    is derived from the frame height. See ab_pce_regline_for_row.
 *    AB_PCE_RASTER_BIAS survives only as the fallback for a core too old to
 *    record vpos.
 *
 *    Corollary that matters as much as the mapping: A ROW NO RECORDED LINE
 *    CLAIMS IS BLANK. The VDC's window is often shorter than the VCE's
 *    visible area, and those rows get the VDC's idle 0x100 pixel. The old
 *    constant-bias code silently rendered them with the frame-end registers,
 *    inventing picture where the hardware emitted border.
 *
 *  - THE VCE COLOUR TABLE MUTATES MID-FRAME TOO, AND IT LOOKS NOTHING LIKE A
 *    GEOMETRY BUG. m_color_table is written by the CPU through VCE ports 4/5
 *    at any time, and HuC6260::Clock resolves EVERY emitted pixel through it
 *    live -- so pce_vce_palette (a frame-end read) describes only the last
 *    palette of a frame that showed several. One title screen scored
 *    43.9% this way with the geometry already pixel-perfect: the failure
 *    presented as two blues swapped across most of the screen, which reads
 *    like a palette-decode bug, not like a timing one. The tell is that the
 *    SHAPES are all correct.
 *    The fix is pce_vce_pallines (region id 0x1A7), recorded at end-of-line
 *    and keyed by the SAME m_vpos the regline record carries, so one row index
 *    drives both. OPTIONAL, exactly like reglines: absent means fall back to
 *    the frame-end table, which is the old behaviour.
 *
 *  - A BACKGROUND PIXEL WHOSE COLOUR ENTRY IS 0 COLLAPSES TO PALETTE INDEX
 *    0, not to "sub-palette N, entry 0". HuC6270::Clock does
 *    `if ((pixel & 0x0F) == 0) pixel = 0;` AFTER the line buffer holds
 *    (colour_table << 4) | entry. Entry 0 of every sub-palette is the shared
 *    backdrop. Skipping this paints the backdrop in 16 different colours.
 *
 *  - TILES ARE 4BPP IN TWO WORD PLANES, EIGHT WORDS APART. For tile row
 *    `ty`, word (tile<<4)+ty holds planes 0/1 (low byte = plane 0, high byte
 *    = plane 1) and word (tile<<4)+ty+8 holds planes 2/3. Bit 7-(x&7) of
 *    each. That +8 word stride inside a 16-word tile is the interleave.
 *
 *  - SPRITE PATTERNS ARE A DIFFERENT LAYOUT FROM TILES: planes are 16 WORDS
 *    apart (line+0, +16, +32, +48) inside a 64-word cell, and a 32-wide
 *    sprite is TWO cells 64 words apart. Do not reuse the BG decoder.
 *
 *  - SPRITE X IS BIASED BY 0x20 AND Y BY 64. Screen x = sat[1] - 0x20,
 *    screen y = (sat[0] & 0x3FF) - 64. Different bias per axis; neither is
 *    the NES or GB convention.
 *
 *  - 32-WIDE SPRITES ARE FETCHED AS TWO 16-WIDE HALVES AND EACH HALF COUNTS
 *    AGAINST THE 16-PER-LINE LIMIT SEPARATELY. X-flip swaps which cell feeds
 *    which half (the +64 word offset moves between them), it does not just
 *    mirror pixels. Transcribed from FetchSprites' width==16 vs else branch.
 *
 *  - SPRITES ARE MERGED BACK-TO-FRONT (index 63 down to 0) into a SEPARATE
 *    line buffer, and only entries carrying the 0x200 "wrote in front" flag
 *    overwrite the background. A behind-background sprite (flags bit 7 clear)
 *    over an OPAQUE background pixel keeps its pixel in the sprite buffer but
 *    WITHOUT 0x200, so it loses -- while still occluding lower-numbered
 *    sprites. That two-buffer dance is load-bearing; a single-pass
 *    "if visible then draw" gets sprite-vs-sprite-vs-BG stacking wrong.
 *
 *  - MWR SELECTS THE BAT SIZE: (MWR >> 4) & 7 indexes {32,64,128,128} tiles
 *    wide and {32,32,32,32,64,64,64,64} tall. The BAT is a flat row-major
 *    array of u16 at VRAM word 0, and scrolling WRAPS on the BAT's pixel
 *    dimensions (mask, not modulo -- they are powers of two).
 *
 *  - MID-FRAME RASTER SPLITS ARE THE NORM, NOT THE EXCEPTION, AND THE
 *    END-OF-FRAME REGISTERS CANNOT DESCRIBE THEM. pce_vdc_regs is what the
 *    CPU last wrote; a game doing parallax rewrites BXR/BYR on an RCR
 *    interrupt EVERY SCANLINE, so a reconstruction that reads one snapshot
 *    gives every line the LAST scroll of the frame. One parallax platformer scored
 *    31.7% that way -- the top rows pixel-exact (they are the rows written
 *    last is a coincidence of that title; in general SOME band matches) and
 *    everything past the first split off by whole tiles, with visible band
 *    jumps. There is no arithmetic that recovers it: the values are simply
 *    gone by the time the frame ends.
 *    The fix is pce_vdc_reglines (region id 0x1A6), which the patched
 *    geargrafx records at the top of HuC6270::RenderLine -- the LATCHED
 *    bxr/cr/mwr/hdw plus m_bg_offset_y (the effective BYR for that line).
 *    Same class of region as the NES core's PALLINES/MASKLINES/NTMAPLINES.
 *    It is OPTIONAL: an older core or a host that does not expose it falls
 *    back to the frame-end registers, which is exactly the old behaviour and
 *    still 100% on games that do not split.
 *
 *  - THE PER-LINE RECORD IS INDEXED BY VDC RASTER LINE, NOT FRAMEBUFFER ROW.
 *    m_raster_line resets to 0 at VDW start. Look a row up with
 *    ab_pce_regline_for_row, which matches on the recorded vpos; indexing the
 *    table with a biased row is the old, wrong approach (see the offset note
 *    above). `valid` marks lines the VDC actually rendered.
 *
 *  - THE FRAMEBUFFER IS WIDER THAN THE PICTURE, AND THE WIDTH COMES FROM THE
 *    **VCE**, NOT FROM HDW. The VCE emits k_huc6260_line_width[overscan]
 *    [speed] = {256, 341, 512, 512} pixels per row by clocking the VDC, and
 *    the VDC returns real picture only inside its HDW window -- every other
 *    clock returns 0x100. Deriving the width from HDR produced 328 where the
 *    core produced 341 and 240 where it produced 256, on 7 of the 80 games;
 *    those reconstructions were a different SIZE from the core's frame and
 *    could not be scored at all. See the geometry section for the offset that
 *    places the picture inside the row.
 *
 *  - ...AND THE HORIZONTAL PLACEMENT INSIDE THAT ROW IS ALSO RECORDED, NOT
 *    COMPUTED, FOR THE SAME REASON THE VERTICAL ONE IS. ab_pce_x_offset's
 *    arithmetic ((HSW+1 + HDS+1)*8 - screen_start_x) is transcribed from the
 *    core and is right for most of the library -- but the VDC reaches its HDW
 *    window through a clock chain (NextHorizontalState, whose HSW term arrives
 *    via m_clocks_to_next_h_state, NOT as a clean (HSW+1)*8) while the VCE
 *    copies from m_screen_start_x in its own domain. One cart lands 8px off: the
 *    formula predicts 0, the truth is 8, and it sat at a flat 73.00% on every
 *    frame until corrected. pce_vce_xofflines (0x1A9) records the framebuffer
 *    x of the first real picture pixel per scanline; 0xFFFF means the VDC
 *    emitted no picture on that line at all. The formula remains as the
 *    fallback for a core without the region.
 *
 *  - VRAM IS DMA'd **DURING DISPLAY**, SO A FRAME-END VRAM SNAPSHOT CANNOT
 *    RECONSTRUCT EVERY LINE -- AND THE ANSWER IS TO STOP RECONSTRUCTING.
 *    HuC6270::VRAMTransfer runs from Clock(), i.e. while the picture is being
 *    emitted, so a game streaming new sprite/tile patterns shows one pattern
 *    on line 40 and a different one on line 120 out of the same tile index.
 *    pce_vdc_vram holds only the final state. The bad pixels cluster on a
 *    handful of animated patterns (two pattern-animation-heavy carts) and no amount
 *    of care in the line renderer can recover the bytes -- they are gone.
 *    VRAM cannot be captured per line either (263 x 64KB).
 *    It does not need to be: pce_vdc_linepix (0x1A8) records the VDC's own
 *    m_line_buffer per raster line, AFTER RenderBackground and the sprite
 *    merge. Those are the final palette indices the VDC hands the VCE, so
 *    when the region is present this profile stops re-running the VDC
 *    entirely and just reads them. Same rule the NES profile settled on with
 *    nes_bgpix/nes_linepix: read what the emulator resolved, do not re-derive
 *    it. The reconstruction is still carried, because force_bg/force_sprites
 *    need a layer split the merged buffer cannot provide.
 *    ONE SUBTLETY: the recorded buffer has NOT had the entry-0 collapse
 *    applied -- the core does that later, in Clock() -- so the consumer must
 *    still apply it.
 *
 *  - BURST MODE IS **LITERAL BLACK**, NOT THE BACKDROP, AND IT MUST BE
 *    CHECKED BEFORE THE RESOLVED PLANE. With both layers disabled the VDC
 *    returns HUC6270_PIXEL_BLACK (0x800) from Clock() without consulting the
 *    line buffer, and the VCE writes black for that code while BYPASSING the
 *    colour table -- unlike the 0x100 idle pixel, which IS resolved through
 *    the palette. Two consequences, both load-bearing:
 *      1. Treating a burst line as index 0x100 paints it in the backdrop
 *         colour. Parasol Stars' screen transition came out flat BLUE against
 *         the hardware's flat BLACK: 0.0000% on a frame that is a single
 *         colour either way, which is exactly the kind of frame it is
 *         tempting to dismiss as "blank, therefore fine".
 *      2. RenderLine SKIPS both renderers in burst, so pce_vdc_linepix still
 *         holds the last non-burst line. The burst check must therefore come
 *         BEFORE the resolved-plane read, or stale picture gets painted over
 *         a blanked screen.
 *
 *  - A vpos CAN BE CLAIMED BY MORE THAN ONE RASTER LINE, AND THE LAST WRITER
 *    IS THE RIGHT ONE. m_raster_line resets whenever a display window starts,
 *    so a game that restarts VDW mid-frame leaves the previous window's
 *    low-numbered records in the table carrying the SAME vpos as this frame's
 *    real lines. Both are `valid`. One shooter does this at raster 133 and a
 *    first-match lookup painted a six-row band with the wrong scroll --
 *    98.3102% on every frame, always the same 969 pixels. ab_pce_regline_for_row
 *    therefore scans the table BACKWARDS.
 *
 *  - THE SATB TIMING RESIDUAL WAS A MISDIAGNOSIS. An earlier revision of this
 *    file blamed the last ~0.1-1.8% on the SATB being DMA-refreshed during
 *    display, so that a post-frame snapshot catches one phase of an animation
 *    the frame showed several of. The evidence looked right (the residual
 *    moved when the sprite layer was disabled) but the cause was the constant
 *    raster bias putting whole rows on the wrong scanline; sprites just made
 *    the error visible. With the recorded vpos mapping those frames went to
 *    exactly 100%. The genuine mid-frame-mutation problem is VRAM, not the
 *    SATB, and it is solved by reading the resolved line buffer -- see
 *    pce_vdc_linepix. Verified: the SATB always matched its own VRAM source
 *    (256/256 words) on every frame that was still failing.
 *
 *  - HISTORICAL NOTE ON THE OLD RESIDUAL: games animating
 *    sprite patterns MID-FRAME. The SATB is DMA-refreshed during display, so
 *    a single post-frame snapshot of pce_vdc_satb + VRAM catches ONE phase of
 *    a twinkling-star pattern while the displayed frame contains several. On
 *    Blazing Lazers every residual pixel traced to sprites sharing one
 *    animated pattern word (sat2 == 0x0062); excluding that pattern scores
 *    99.97% / 99.95%. This is a capture-timing limit of the exposed regions,
 *    NOT a renderer defect -- do not chase it in this file.
 */
#ifndef AB_PCE_PROFILE_H
#define AB_PCE_PROFILE_H

#include <stdint.h>
#include "ab_render.h"

enum {
  /* The VDC's display width is programmable; 256 is the common case and the
   * allocation ceiling here. HDW can reach 1024 but no cart ships that. */
  AB_PCE_MAX_W = 1120,
  AB_PCE_MAX_H = 242,

  AB_PCE_VRAM_WORDS = 0x8000,
  AB_PCE_SAT_WORDS  = 0x100,
  AB_PCE_REGS       = 20,
  AB_PCE_PAL_ENTRIES = 512,

  /* pce_vdc_reglines: 263 records of 16 bytes. 263 = HUC6270_LINES, the full
   * NTSC line count -- the display window is shorter but m_raster_line is
   * bounded only by the programmed VDW. */
  AB_PCE_REGLINES     = 263,
  AB_PCE_REGLINE_SIZE = 16,

  /* pce_vce_pallines: one full 512-entry colour table per VCE scanline. */
  AB_PCE_PALLINES      = 263,
  AB_PCE_PALLINE_SIZE  = 512 * 2,

  /* pce_paldeltas geometry -- must match struct romdev_pce_paldelta_region:
   * header {count u16, truncated u8, pad u8} + entries {vpos u16, hpos u16,
   * dot u16, index u16, oldv u16, newv u16}. */
  AB_PCE_PALDELTA_HDR   = 4,
  AB_PCE_PALDELTA_ENTRY = 12,
  AB_PCE_PALDELTA_CAP   = 8192,
  AB_PCE_PALDELTAS_SIZE = AB_PCE_PALDELTA_HDR + AB_PCE_PALDELTA_CAP * AB_PCE_PALDELTA_ENTRY,

  /* pce_vdc_linepix: the VDC's resolved line buffer, per raster line. */
  AB_PCE_LINEPIX_LINES = 263,
  AB_PCE_LINEPIX_WIDTH = 1024,
  AB_PCE_LINEPIX_SIZE  = 1024 * 2,

  /* pce_vce_xofflines: one u16 per VCE scanline. 0xFFFF = no picture. */
  AB_PCE_XOFFLINES     = 263,
  AB_PCE_XOFF_NONE     = 0xFFFF,

  AB_PCE_SPRITES = 64,
  /* Per-line sprite cap the VDC enforces. A 32-wide sprite eats TWO slots. */
  AB_PCE_SPR_PER_LINE = 16,

  /* Framebuffer row -> VDC raster line, used ONLY on the fallback path (a core
   * without pce_vdc_reglines, or a row no recorded line claims). It is a
   * measured typical value, NOT a hardware invariant: sweeping it across the
   * 80-game corpus found games that are pixel-exact at 0, at 8 and at 17. When
   * the region is present, ab_pce_regline_for_row gives the exact mapping and
   * this constant is not consulted. See the FACTS block. */
  AB_PCE_RASTER_BIAS = 8,

  /* Framebuffer row 0 is VCE scanline m_screen_start_y, and that is
   *   m_screen_start_y = m_scanline_start + HUC6270_LINES_TOP_BLANKING
   * with LINES_TOP_BLANKING = 14 (huc6260_inline.h SetScanlineStart).
   *
   * m_scanline_start is a CORE OPTION, not a constant: geargrafx's libretro
   * glue sets it from geargrafx_scanline_count -- 11 for "224p", 2 for
   * "240p", otherwise the raw geargrafx_scanline_start (default 0). So the
   * origin is 25, 16 or 14 depending on how the frontend is configured, and
   * the frame HEIGHT is what tells them apart: 224 rows means 224p means
   * origin 25. Assuming a bare 14 put every row of a 224-line frame 11 rows
   * out and blanked the top of the picture. */
  AB_PCE_LINES_TOP_BLANKING = 14,

  /* VDC register indices (huc6270_defines.h). */
  AB_PCE_REG_CR   = 0x05,
  AB_PCE_REG_BXR  = 0x07,
  AB_PCE_REG_BYR  = 0x08,
  AB_PCE_REG_MWR  = 0x09,
  /* HSR: HSW in bits 0-4, HDS in bits 8-14. Together they place the VDC's
   * display window inside the VCE's line -- see ab_pce_x_offset. */
  AB_PCE_REG_HSR  = 0x0A,
  AB_PCE_REG_HDR  = 0x0B,
  AB_PCE_REG_VDR  = 0x0D,

  /* CR bits that gate the two layers. */
  AB_PCE_CR_BG_ON  = 0x0080,
  AB_PCE_CR_SPR_ON = 0x0040,

  /* Sprite attribute flag bits (SATB word 3). */
  AB_PCE_SPR_PRIORITY = 0x0080,   /* set = in front of background */
  AB_PCE_SPR_XFLIP    = 0x0800,
  AB_PCE_SPR_YFLIP    = 0x8000,

  /* Line-buffer tag bits, mirroring the core's own encoding. */
  AB_PCE_LB_SPRITE = 0x100,       /* this pixel came from the sprite layer */
  AB_PCE_LB_INFRONT = 0x200,      /* ... and it won against the background */

  /* HUC6270_PIXEL_BLACK. Burst mode (both layers disabled) makes the VDC
   * return this instead of a palette index, and HuC6260::Clock writes LITERAL
   * BLACK for it rather than looking it up -- so a blanked frame is black
   * regardless of what the backdrop entry holds. Modelling burst as "index
   * 0x100" instead painted Parasol Stars' screen-transition frame in the
   * backdrop colour (blue) where the hardware showed black: 0.0000% on a
   * frame that is a single flat colour either way. */
  AB_PCE_LB_BLACK = 0x800
};

/* Region handles the profile needs. Resolve once in bind. The first four are
 * REQUIRED -- unlike the resolved-plane platforms there is no degraded mode,
 * because there is no pre-rendered picture to fall back on. `reglines` is
 * OPTIONAL: set it to -1 (or leave it as an unresolved handle) on a core that
 * predates the region, and the profile uses the frame-end registers for every
 * line, exactly as it did before. */
typedef struct {
  int32_t vram;      /* pce_vdc_vram     0x8000 u16 LE */
  int32_t satb;      /* pce_vdc_satb     0x100  u16 LE */
  int32_t regs;      /* pce_vdc_regs     20     u16 LE (LIVE, not latched) */
  int32_t palette;   /* pce_vce_palette  512    u16 LE, 9-bit GGGRRRBBB */
  int32_t reglines;  /* pce_vdc_reglines 263 x 16 B LE -- OPTIONAL, -1 if absent */
  /* pce_vce_pallines 263 x 512 u16 LE -- OPTIONAL, -1 if absent. Same
   * mid-frame-mutation problem as reglines, for colour instead of scroll. */
  int32_t pallines;
  /* pce_vdc_linepix 263 x 1024 u16 LE -- OPTIONAL, -1 if absent. The VDC's
   * own resolved line buffer; when present the profile USES IT instead of
   * re-running the line renderer, which is both exact and much cheaper. */
  int32_t linepix;
  /* pce_vce_xofflines 263 u16 LE -- OPTIONAL, -1 if absent. The RECORDED
   * framebuffer x of the picture's first pixel per scanline; supersedes
   * ab_pce_x_offset's arithmetic when present. */
  int32_t xofflines;
  /* pce_vce_srclines 263 u16 LE -- OPTIONAL, -1 if absent. Which line-buffer
   * pixel the row's first copied pixel came from; non-zero only when the
   * picture is clipped on the left. */
  int32_t srclines;
  /* pce_paldeltas -- OPTIONAL dot-stamped VCE palette write log, -1 if
   * absent. pallines is per-LINE; a MID-line colour write (Eaggy) renders
   * the head of the row through the OLD value, which no per-line snapshot
   * can express. */
  int32_t paldeltas;
} ab_pce_regions;

/* Per-frame snapshot. One bulk read per region; the per-byte path would be
 * 66k host calls a frame. Buffers are caller-owned and hold RAW LE BYTES --
 * they are decoded to u16 on access so a big-endian host cannot bite. */
typedef struct {
  unsigned char *vram;      /* AB_PCE_VRAM_WORDS * 2 */
  unsigned char sat[AB_PCE_SAT_WORDS * 2];
  unsigned char regs[AB_PCE_REGS * 2];
  unsigned char pal[AB_PCE_PAL_ENTRIES * 2];
  /* Per-line latched state. `has_reglines` is 0 when the core does not expose
   * the region; every consumer must check it rather than sniffing the bytes,
   * because an all-zero record is a legitimate value (BXR 0, BYR 0). */
  unsigned char reglines[AB_PCE_REGLINES * AB_PCE_REGLINE_SIZE];
  int has_reglines;
  /* Per-line VCE colour table. Big (263 * 1024 B) but it is the only way to
   * reproduce a mid-frame recolour; see the FACTS block. Caller-owned like
   * `vram`, so a consumer that does not need it pays nothing. */
  unsigned char *pallines;
  int has_pallines;
  /* Resolved per-line palette indices. Caller-owned like `vram`. */
  unsigned char *linepix;
  int has_linepix;
  unsigned char xofflines[AB_PCE_XOFFLINES * 2];
  int has_xofflines;
  unsigned char srclines[AB_PCE_XOFFLINES * 2];
  int has_srclines;
  /* Dot-stamped palette write log (raw region bytes). Caller-owned like
   * `vram`; NULL disables mid-row palette splits. A TRUNCATED log is
   * rejected whole (has_paldeltas 0): a partial undo rebuilds a line-start
   * table that never existed -- same rule as the MSX vdeltas log. */
  unsigned char *paldeltas;
  int has_paldeltas;
} ab_pce_frame;

/* One decoded per-line record. Mirrors the core's struct romdev_pce_regline;
 * see the header note on why it is the LATCHED state and not the live file. */
typedef struct {
  uint16_t bxr;      /* horizontal scroll this line rendered with */
  uint16_t bgy;      /* effective BYR (m_bg_offset_y) this line rendered with */
  uint16_t cr;       /* layer enables */
  uint16_t mwr;      /* BAT size + sprite mode */
  uint16_t hdw;      /* display width in 8px units - 1 */
  uint16_t raster;   /* echoed line number */
  uint16_t vpos;     /* VCE scanline this line rendered on; fb row = vpos-14 */
  int      valid;    /* 0 = never rendered / region absent; caller falls back */
  int      burst;    /* line was blanked: both layers off */
} ab_pce_regline;

/* Decode the record for VDC raster line `raster`. Returns 0 (and leaves *out
 * zeroed with valid=0) when the region is absent, the line is out of range, or
 * the slot was never written this frame. */
int ab_pce_regline_at(const ab_pce_frame *f, int raster, ab_pce_regline *out);

/* Decode the record that produced FRAMEBUFFER ROW `fb_row`, by matching the
 * recorded vpos (fb row = vpos - AB_PCE_VPOS_FB_ORIGIN). This is the exact
 * mapping and it supersedes the constant-bias search -- see the FACTS block.
 * Returns 0 when no rendered line landed on that row (a row above or below the
 * display window, which the VCE fills with the VDC's 0x100 idle pixel). */
int ab_pce_regline_for_row(const ab_pce_frame *f, int fb_row, int fb_height,
                           ab_pce_regline *out);

/* Framebuffer row 0's VCE scanline, derived from the frame height -- see
 * AB_PCE_LINES_TOP_BLANKING. The height is the only observable that
 * distinguishes the frontend's scanline_count setting. */
static inline int ab_pce_vpos_origin(int fb_height) {
  /* 224p: scanline_start 11. 240p: 2. Anything else: the default 0. */
  if (fb_height == 224) return 11 + AB_PCE_LINES_TOP_BLANKING;
  if (fb_height == 240) return 2 + AB_PCE_LINES_TOP_BLANKING;
  return AB_PCE_LINES_TOP_BLANKING;
}

/* 1 when the capture's reglines carry the vpos field (a core new enough to
 * record it). Consumers must check this before treating "no record for this
 * row" as "this row is blank". */
int ab_pce_reglines_have_vpos(const ab_pce_frame *f);

/* Framebuffer row -> VDC raster line bias (AB_PCE_RASTER_BIAS). */
int ab_pce_raster_bias(const ab_pce_frame *f);

/* Read this frame's state. Returns 1 on success. */
int ab_pce_frame_read(const ab_pce_regions *r, ab_pce_frame *f);

/* --- raw accessors ------------------------------------------------------- */
static inline uint16_t ab_pce_u16(const unsigned char *p, int word_index) {
  const size_t o = (size_t)word_index * 2;
  return (uint16_t)(p[o] | ((uint16_t)p[o + 1] << 8));
}
static inline uint16_t ab_pce_vram(const ab_pce_frame *f, int addr) {
  /* The VDC masks VRAM reads to 0x8000 words; out of range returns openbus,
   * which we model as 0 (a real openbus read is unreproducible from a
   * snapshot and never happens on a well-behaved cart). */
  if (addr < 0 || addr >= AB_PCE_VRAM_WORDS) return 0;
  return ab_pce_u16(f->vram, addr);
}
static inline uint16_t ab_pce_reg(const ab_pce_frame *f, int i) {
  if (i < 0 || i >= AB_PCE_REGS) return 0;
  return ab_pce_u16(f->regs, i);
}

/* --- geometry ------------------------------------------------------------
 * THE FRAMEBUFFER WIDTH IS THE **VCE** LINE WIDTH, NOT (HDW+1)*8.
 *
 * This was wrong for a long time and it is the single most consequential
 * geometry fact in the file, so it gets the long comment:
 *
 *   The VCE (huc6260) emits k_huc6260_line_width[overscan][speed] pixels for
 *   every row -- {256, 341, 512, 512} with overscan off, selected by the VCE
 *   control register's clock divider (CR bits 0-1). It gets each of those
 *   pixels by CLOCKING the VDC. The VDC only returns real picture during its
 *   HDW window (HuC6270::Clock: `m_active_line && m_h_state == HDW`); every
 *   clock outside that window returns 0x100, the VDC's "no pixel" code, which
 *   the VCE resolves through the palette exactly like any other index.
 *
 *   So a framebuffer row is: LEFT BORDER of index 0x100, then the (HDW+1)*8
 *   picture, then RIGHT BORDER -- and (HDW+1)*8 is in general NEITHER the
 *   framebuffer width nor a divisor of it. Deriving the width from HDR gave
 *   328 where the core produced 341, 240 where it produced 256, and so on;
 *   the reconstruction was a differently-sized image that could not be scored
 *   at all, let alone matched.
 *
 * WHERE the picture sits inside the row is a clock-domain question. The VDC
 * reaches HDW after (HSW+1 + HDS+1)*8 dot clocks from the start of its line,
 * while the VCE only starts COPYING to the framebuffer at m_screen_start_x =
 * k_huc6260_line_start[overscan][speed]. The difference is the offset:
 *
 *     x_offset = (HSW + 1 + HDS + 1) * 8 - screen_start_x
 *
 * Transcribed from HuC6270::NextHorizontalState (the HDS clock count) and
 * huc6260_inline.h SetOverscan (m_screen_start_x). Verified against measured
 * per-game shifts: it reproduces the offset exactly on every capture whose
 * picture was otherwise correct enough to localise (+0, +8, +16, -8), and the
 * games where a brute-force shift search disagreed were the ones still failing
 * for other reasons, where the search's argmax is noise rather than a reading.
 *
 * A negative offset is legal and happens (HSW=3, HDS=3 at 341-wide gives -8):
 * the VDC's window opens BEFORE the VCE starts copying, so the first pixels of
 * the picture are clipped off the left edge rather than shown.
 */

/* The VCE clock divider lives in the VCE control register, which is NOT part
 * of the 20-entry VDC register file. It is not exposed as its own region, so
 * it is inferred from the framebuffer width the host reports -- the width IS
 * the speed, one for one, via k_huc6260_line_width. */
static inline int ab_pce_vce_line_start(int fb_width) {
  /* k_huc6260_line_start[0] = {48, 72, 120, 120} for speeds 0..3, overscan
   * off. Keyed by the width those speeds produce. */
  switch (fb_width) {
    case 256: return 24 + 24;
    case 341: return 24 + 48;
    case 512: return 24 + 96;
    default:  return 24 + 24;
  }
}

/* The VDC's picture width -- how many pixels the VDC actually supplies inside
 * the framebuffer row. This is the (HDW+1)*8 quantity that used to be
 * mistaken for the framebuffer width. */
static inline int ab_pce_active_width(const ab_pce_frame *f) {
  int w = ((ab_pce_reg(f, AB_PCE_REG_HDR) & 0x7F) + 1) * 8;
  if (w > AB_PCE_MAX_W) w = AB_PCE_MAX_W;
  if (w < 8) w = 8;
  return w;
}

/* Signed offset of the VDC picture inside the framebuffer row. See above. */
static inline int ab_pce_x_offset(const ab_pce_frame *f, int fb_width) {
  const uint16_t hsr = ab_pce_reg(f, AB_PCE_REG_HSR);
  const int hsw = hsr & 0x1F;
  const int hds = (hsr >> 8) & 0x7F;
  return ((hsw + 1) + (hds + 1)) * 8 - ab_pce_vce_line_start(fb_width);
}

/* Backwards-compatible name. Callers that have a host framebuffer width should
 * use it directly; this remains the VDC active width. */
static inline int ab_pce_width(const ab_pce_frame *f) {
  return ab_pce_active_width(f);
}

/* --- colour --------------------------------------------------------------
 * VCE entry -> 0xRRGGBBAA. The entry is 9-bit GGGRRRBBB and the core renders
 * RGB565, so each component is quantised to 5/6/5 and widened back exactly as
 * the host widens. See the header note -- this is NOT `c * 255 / 7`.
 */
uint32_t ab_pce_rgba(const ab_pce_frame *f, int pal_index);

/* Build the RGBA LUT for the palette AS IT WAS ON VCE SCANLINE `vpos`. Falls
 * back to the frame-end table when the region is absent or the slot is
 * unwritten, so a caller can always use it. */
void ab_pce_build_lut_at(const ab_pce_frame *f, int vpos,
                         uint32_t out_lut[AB_PCE_PAL_ENTRIES]);

/* Mid-row palette splits (pce_paldeltas). ab_pce_pal_events_for_row collects
 * this row's dot-stamped writes IN LOG ORDER (chronological). The row's
 * emit must then: build the lut from pallines[vpos] (end-of-line state),
 * UNDO the events in REVERSE (restore oldv) to reach the line-START table,
 * and re-APPLY each event's newv when x reaches its dot. An event with
 * dot 0 is an hblank write: whole-row, no split (m_pixel_index sits on a
 * line-width multiple during blanking, so dot reads 0 there). */
typedef struct { int dot; int index; uint16_t oldv, newv; } ab_pce_pal_ev;
int ab_pce_pal_events_for_row(const ab_pce_frame *f, int vpos,
                              ab_pce_pal_ev *ev, int max);
/* Patch ONE lut entry to a raw 9-bit GGGRRRBBB value. */
void ab_pce_lut_set(uint32_t lut[AB_PCE_PAL_ENTRIES], int index, uint16_t raw);

/* Resolve a line-buffer code through the LUT, honouring AB_PCE_LB_BLACK
 * (burst mode bypasses the palette and is literally black). */
uint32_t ab_pce_resolve(const uint32_t lut[AB_PCE_PAL_ENTRIES], uint16_t code);

/* Build the full 512-entry RGBA LUT once per frame; the emit loop indexes it
 * instead of redoing the shifts per pixel. */
void ab_pce_build_lut(const ab_pce_frame *f, uint32_t out_lut[AB_PCE_PAL_ENTRIES]);

/* --- line rendering ------------------------------------------------------
 * Render ONE scanline into `line`, as the VDC's own line buffer codes:
 *   bits 0-3  colour entry within the sub-palette
 *   bits 4-7  sub-palette (colour table) number
 *   bit  8    AB_PCE_LB_SPRITE
 * A code whose low nibble is 0 has already been collapsed to 0 (backdrop).
 * The result indexes the VCE palette directly.
 *
 * `fb_row` is the FRAMEBUFFER row; the raster bias is applied internally, so
 * callers never repeat it.
 */
void ab_pce_render_line(const ab_pce_frame *f, int fb_row, int fb_height,
                        int width, int force_bg, int force_sprites,
                        uint16_t *line);

/* Render one FRAMEBUFFER row -- `fb_width` pixels wide, the width the host
 * reports, with the VDC picture placed at ab_pce_x_offset and index 0x100
 * border either side. This is what a consumer scoring against (or drawing
 * over) the emulator's framebuffer wants; ab_pce_render_line renders only the
 * VDC's active window and knows nothing about where it sits. */
void ab_pce_render_fb_row(const ab_pce_frame *f, int fb_row, int fb_height,
                          int fb_width, int force_bg, int force_sprites,
                          uint16_t *row);

/* --- emit ---------------------------------------------------------------- */
typedef struct {
  double ox, oy;      /* top-left of the game view in logical coords */
  double scale;
  int    height;      /* framebuffer rows to draw */
  /* The live CR may say "off" for a layer the frame plainly shows (see the
   * header note on latched registers). -1 = follow CR, 1 = force on,
   * 0 = force off. */
  int    force_bg;
  int    force_sprites;
  /* THE CORE'S FRAMEBUFFER WIDTH -- the VCE line width, not the VDC window.
   * 0 (or unset) means "use ab_pce_width(f)", which is the VDC's active
   * window and is what this profile used to assume unconditionally.
   *
   * THE TWO DISAGREE, AND ONLY ON SOME GAMES. The VDC's HDW window and the
   * VCE's copied line are different clock domains: a 256-wide game has them
   * equal, but a 341-wide game programs a 352px window into a 341px line and
   * the picture is CLIPPED on the left by the difference. ab_pce_x_offset and
   * the srclines path already handle that -- but ONLY if they are told the
   * real line width. Computing it from the VDC registers instead re-derives
   * the wrong number and the whole picture lands 8px off.
   *
   * This was invisible for a long time because the C test harness passes the
   * true width via an env var while ab_pce_emit had no parameter to accept
   * one: the harness scored 100% on games a real bezel rendered at 71%. Any
   * consumer with access to the host framebuffer width should set this. */
  int    fb_width;
} ab_pce_view;

/* Emit the whole picture as run-coalesced solid quads. The PCE has no
 * separately addressable "background layer" once the VDC has merged sprites
 * into its line buffer, so unlike the resolved-plane profiles this is ONE
 * pass -- splitting it would mean rendering every line twice.
 *
 * `suppress` is an optional width*height byte mask; a non-zero entry means
 * "this pixel belongs to replacement art, draw the background instead".
 */
extern int ab_pce_test_no_linepix;
extern int ab_pce_test_no_paldeltas;    /* ignore the dot-stamped palette log */
extern int ab_pce_test_pal_vpos_delta;
extern int ab_pce_test_pal_delta_row;
extern int ab_pce_test_cur_row;

int ab_pce_emit(ab_batch *b, const ab_pce_frame *f, const ab_pce_view *v,
                const unsigned char *suppress);

/* --- SATB helpers --------------------------------------------------------
 * SATB is 64 entries of 4 u16: [0] y, [1] x, [2] pattern<<1, [3] flags.
 * Screen position is y-64 and x-0x20 (see the header note on the biases).
 *
 * Sprite substitution keys on the PATTERN number, which is the PCE's
 * equivalent of a tile id: (sat[2] >> 1) & 0x3FF.
 */
typedef struct {
  int x0, y0, x1, y1;   /* screen-space, exclusive on x1/y1 */
} ab_pce_bounds;

static inline int ab_pce_sprite_pattern(const ab_pce_frame *f, int i) {
  return (ab_pce_u16(f->sat, i * 4 + 2) >> 1) & 0x3FF;
}

int ab_pce_mark_sprites(const ab_pce_frame *f, const ab_registry *reg,
                        int width, int height, unsigned char *suppress,
                        const ab_sub_rule **out_rule, ab_pce_bounds *out_bounds);

#endif /* AB_PCE_PROFILE_H */
