/* PC Engine profile tests.
 *
 * This profile is unlike the others: geargrafx exposes NO resolved pixel
 * planes, so ab_pce.c RE-RUNS the VDC's line renderer from VRAM + SATB +
 * registers. The load-bearing claims are therefore about the DECODE, not
 * about widening someone else's answer:
 *
 *   - the 9-bit VCE entry is GGGRRRBBB and resolves through RGB565
 *     quantisation, not the naive *255/7 RGBA8888 expansion,
 *   - tile planes are two words 8 apart; sprite planes are four words 16
 *     apart in a different layout,
 *   - a colour entry of 0 collapses to palette index 0 (shared backdrop),
 *   - sprite priority is resolved through a SECOND line buffer, so a
 *     behind-background sprite still occludes lower-numbered sprites.
 *
 * Scored against the real core at 100.0000% on 8 frames across 4 games; these
 * tests pin the individual rules so a regression names itself.
 *
 * The sentinel colours below are deliberately NOT black, for the same reason
 * the GB suite does it: a decode bug that resolves to 0 is invisible when the
 * expected value is also 0.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define AB_IMPORT(m,n)
typedef struct { float x,y,u,v; uint32_t rgba,_pad; } ab_vertex;
static int g_mesh_calls, g_last_n; static int32_t g_last_tex;
static ab_vertex g_verts[65536];
int32_t ab_mesh(const ab_vertex*v,int32_t n,int32_t t){
  g_last_n=n;g_last_tex=t;g_mesh_calls++;
  if(n>0&&(size_t)n<=sizeof(g_verts)/sizeof(g_verts[0])) memcpy(g_verts,v,(size_t)n*sizeof(ab_vertex));
  return 1; }
static uint32_t g_tex_px[65536]; static int g_tex_w,g_tex_h,g_next=1;
int32_t ab_texture_create_rgba(const void*px,int32_t w,int32_t h){
  size_t n=(size_t)w*h*4; if(n>sizeof(g_tex_px)) n=sizeof(g_tex_px);
  g_tex_w=w;g_tex_h=h;memcpy(g_tex_px,px,n);return g_next++; }
int32_t ab_texture_destroy(int32_t t){(void)t;return 1;}

/* Region 0 = VRAM (64 KB), 1 = SATB, 2 = regs, 3 = palette, 4 = reglines.
 *
 * R_VRAM is deliberately 0 and R_NONE deliberately -1: an absent region is -1,
 * and a struct field left to C's zero-init would name VRAM. The reglines
 * handle must therefore always be set EXPLICITLY -- see load_frame_nolines. */
enum { R_VRAM=0, R_SATB=1, R_REGS=2, R_PAL=3, R_LINES=4, R_COUNT=5, R_NONE=-1 };
static unsigned char g_vram[0x8000*2];
static unsigned char g_satb[0x100*2];
static unsigned char g_regs[20*2];
static unsigned char g_pal[512*2];
static unsigned char g_lines[263*16];
static unsigned char *g_region[R_COUNT] = { g_vram, g_satb, g_regs, g_pal, g_lines };
static int g_region_len[R_COUNT] = { sizeof(g_vram), sizeof(g_satb),
                                     sizeof(g_regs), sizeof(g_pal),
                                     sizeof(g_lines) };
int32_t ab_region_read_u8(int32_t r,int32_t o){
  if(r<0||r>=R_COUNT) return 0;
  if(o<0||o>=g_region_len[r]) return 0;
  return g_region[r][o]; }
/* Real bulk copy, so the tests exercise the FAST path. */
int32_t ab_region_read(int32_t r,int32_t o,void*d,int32_t n){
  if(r<0||r>=R_COUNT||o<0||n<=0) return 0;
  if(o+n > g_region_len[r]) n = g_region_len[r]-o;
  if(n<=0) return 0;
  memcpy(d,&g_region[r][o],(size_t)n);
  return n; }

#include "ab_render.c"
#include "ab_pce.c"

static int fails=0;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s\n", msg); fails++; } }while(0)

static void wr16(unsigned char *p, int i, uint16_t v) {
  p[i*2] = (unsigned char)(v & 0xFF);
  p[i*2+1] = (unsigned char)(v >> 8);
}

/* Write one per-scanline record, matching the core's struct romdev_pce_regline:
 * u16 bxr, bgy, cr, mwr, hdw, raster; u8 valid, burst; u16 pad. */
static void wr_regline(int line, uint16_t bxr, uint16_t bgy, uint16_t cr,
                       uint16_t mwr, int burst) {
  unsigned char *p = g_lines + line * 16;
  wr16(p, 0, bxr); wr16(p, 1, bgy); wr16(p, 2, cr); wr16(p, 3, mwr);
  wr16(p, 4, 31); wr16(p, 5, (uint16_t)line);
  p[12] = 1; p[13] = (unsigned char)(burst ? 1 : 0);
  p[14] = 0; p[15] = 0;
}

static void reset_state(void) {
  memset(g_vram,0,sizeof(g_vram));
  memset(g_satb,0,sizeof(g_satb));
  memset(g_regs,0,sizeof(g_regs));
  memset(g_pal,0,sizeof(g_pal));
  memset(g_lines,0,sizeof(g_lines));
  /* HDW=31 -> 256 px wide, the common case. BG+sprites enabled. */
  wr16(g_regs, AB_PCE_REG_HDR, 0x041F);
  wr16(g_regs, AB_PCE_REG_CR, AB_PCE_CR_BG_ON | AB_PCE_CR_SPR_ON);
  wr16(g_regs, AB_PCE_REG_MWR, 0x0010);   /* 64x32 BAT */
}

/* Put a solid 8x8 tile of colour entry `e` at tile index `t`.
 *
 * NOTE: tile index t occupies VRAM words (t<<4)..(t<<4)+15, and the BAT lives
 * at word 0 for bat_w*bat_h words. A low tile index therefore OVERLAPS the
 * BAT -- which silently made an earlier version of these tests read BAT
 * entries as tile bitmaps. Callers below use TILE_BG / TILE_SPR, both parked
 * past a 64x32 BAT (2048 words). */
static void make_solid_tile(int t, int e) {
  for (int row = 0; row < 8; row++) {
    uint16_t lo = 0, hi = 0;
    for (int b = 0; b < 8; b++) {
      if (e & 1) lo |= (uint16_t)(1u << b);
      if (e & 2) lo |= (uint16_t)(1u << (b+8));
      if (e & 4) hi |= (uint16_t)(1u << b);
      if (e & 8) hi |= (uint16_t)(1u << (b+8));
    }
    wr16(g_vram, (t<<4)+row, lo);
    wr16(g_vram, (t<<4)+row+8, hi);
  }
}

/* Tile / sprite-pattern indices, chosen so THREE regions never overlap:
 *   BAT          words 0x0000..0x07FF  (64x32 entries)
 *   TILE_BG      words 0x1000..0x101F  (tile index << 4, two tiles)
 *   PAT_SPR      words 0x2000..0x203F  (pattern << 6, one 16x16 cell)
 * Tiles and sprite patterns are indexed with DIFFERENT shifts (<<4 vs <<6),
 * so "different index" does not mean "different memory" -- an earlier version
 * of these tests had TILE_BG 0x100 and PAT_SPR 0x40 both landing on word
 * 0x1000, and the BG tile silently decoded the sprite's bitplanes. */
enum { TILE_BG = 0x100, PAT_SPR = 0x80 };

static ab_pce_frame FR;
static unsigned char FR_VRAM[0x8000*2];

/* The default load has NO reglines region -- that is the fallback path, and
 * every pre-existing test below asserts fallback behaviour. Tests that want
 * the per-line path call load_frame_lines() instead. */
static int load_frame(void) {
  ab_pce_regions rg = { R_VRAM, R_SATB, R_REGS, R_PAL, R_NONE, R_NONE, R_NONE };
  memset(&FR,0,sizeof(FR));
  FR.vram = FR_VRAM;
  return ab_pce_frame_read(&rg, &FR);
}

static int load_frame_lines(void) {
  ab_pce_regions rg = { R_VRAM, R_SATB, R_REGS, R_PAL, R_LINES, R_NONE, R_NONE };
  memset(&FR,0,sizeof(FR));
  FR.vram = FR_VRAM;
  return ab_pce_frame_read(&rg, &FR);
}

int main(void) {
  /* --- colour: RGB565 quantisation, GGGRRRBBB order ---------------------- */
  reset_state();
  /* entry 1 = pure green (GGG=7, RRR=0, BBB=0) -> 0b111000000 = 0x1C0 */
  wr16(g_pal, 1, 0x1C0);
  /* entry 2 = pure red   (RRR=7) -> 0b000111000 = 0x038 */
  wr16(g_pal, 2, 0x038);
  /* entry 3 = pure blue  (BBB=7) -> 0x007 */
  wr16(g_pal, 3, 0x007);
  CHECK(load_frame(), "frame_read should succeed with all four regions");

  const uint32_t green = ab_pce_rgba(&FR, 1);
  const uint32_t red   = ab_pce_rgba(&FR, 2);
  const uint32_t blue  = ab_pce_rgba(&FR, 3);
  /* Full-scale green through 6-bit quantisation is 63 -> (63<<2)|(63>>4)=255 */
  CHECK(green == 0x00FF00FFu, "GGG=7 must be pure green (GGGRRRBBB order)");
  CHECK(red   == 0xFF0000FFu, "RRR=7 must be pure red");
  CHECK(blue  == 0x0000FFFFu, "BBB=7 must be pure blue");

  /* The quantisation is the whole point: component 3 of 7 must land on the
   * 5/6/5 ladder, NOT on the naive 3*255/7 = 109. */
  wr16(g_pal, 4, (3u << 3));           /* RRR = 3 */
  wr16(g_pal, 5, (3u << 6));           /* GGG = 3 */
  load_frame();
  const uint32_t r3 = (ab_pce_rgba(&FR, 4) >> 24) & 0xFF;
  const uint32_t g3 = (ab_pce_rgba(&FR, 5) >> 16) & 0xFF;
  /* r5 = 3*31/7 = 13 -> (13<<3)|(13>>2) = 107 ; naive would be 109 */
  CHECK(r3 == 107, "red 3/7 must quantise through 5 bits (107, not 109)");
  /* g6 = 3*63/7 = 27 -> (27<<2)|(27>>4) = 109 */
  CHECK(g3 == 109, "green 3/7 must quantise through 6 bits (109)");

  /* --- background decode + entry-0 backdrop collapse --------------------- */
  reset_state();
  wr16(g_pal, 0x00, 0x038);      /* backdrop = red sentinel */
  wr16(g_pal, 0x21, 0x1C0);      /* sub-palette 2, entry 1 = green */
  make_solid_tile(TILE_BG, 1);       /* solid colour entry 1 */
  make_solid_tile(TILE_BG + 1, 0);   /* solid colour entry 0 (transparent) */
  /* BAT row 0: col 0 -> the entry-1 tile with colour table 2; col 1 -> the
   * entry-0 tile, which must collapse to the shared backdrop. */
  wr16(g_vram, 0, (uint16_t)((2u << 12) | TILE_BG));
  wr16(g_vram, 1, (uint16_t)((2u << 12) | (TILE_BG + 1)));
  load_frame();

  uint16_t line[AB_PCE_MAX_W];
  /* fb row 0 maps to raster line 8; BYR=0 so bg_y = 8, i.e. BAT row 1.
   * Point BYR at -8 so we land on BAT row 0. */
  wr16(g_regs, AB_PCE_REG_BYR, (uint16_t)((0 - AB_PCE_RASTER_BIAS) & 0x1FF));
  load_frame();
  ab_pce_render_line(&FR, 0, 0, 256, -1, 0, line);
  CHECK(line[0] == 0x21, "BG pixel must carry (colour_table<<4)|entry");
  CHECK(line[8] == 0x00,
        "a colour entry of 0 must collapse to palette index 0, NOT 0x20");

  /* --- sprite decode: position bias, plane stride ------------------------ */
  reset_state();
  wr16(g_pal, 0x00, 0x038);      /* backdrop red */
  wr16(g_pal, 0x13, 0x007);      /* sprite sub-palette 1, entry 3 = blue */
  /* Sprite 0: pattern 1, at screen (0,0) -> SATB x = 0x20, y = 64. */
  wr16(g_satb, 0, (uint16_t)(64 + AB_PCE_RASTER_BIAS));  /* y */
  wr16(g_satb, 1, 0x20);                                  /* x */
  wr16(g_satb, 2, (uint16_t)(PAT_SPR << 1));              /* pattern */
  wr16(g_satb, 3, (uint16_t)(AB_PCE_SPR_PRIORITY | 0x01));/* in front, pal 1 */
  /* The pattern lives at word PAT_SPR<<6. Planes 0 and 1 set -> entry 3. */
  for (int row = 0; row < 16; row++) {
    wr16(g_vram, (PAT_SPR << 6) + row, 0xFFFF);        /* plane 0 */
    wr16(g_vram, (PAT_SPR << 6) + row + 16, 0xFFFF);   /* plane 1 */
  }
  load_frame();
  ab_pce_render_line(&FR, 0, 0, 256, 0, -1, line);
  CHECK((line[0] & 0x0F) == 3, "sprite planes 0+1 must decode to entry 3");
  CHECK((line[0] & 0xF0) == 0x10, "sprite must use its sub-palette (1)");
  CHECK((line[0] & AB_PCE_LB_SPRITE) != 0, "sprite pixel must be tagged");
  CHECK((line[16] & 0x0F) == 0, "sprite is 16 wide; pixel 16 must be clear");

  /* --- sprite priority: behind-BG loses to an opaque BG pixel ------------ */
  reset_state();
  wr16(g_pal, 0x21, 0x1C0);
  wr16(g_pal, 0x13, 0x007);
  make_solid_tile(TILE_BG, 1);
  for (int c = 0; c < 64; c++) wr16(g_vram, c, (uint16_t)((2u << 12) | TILE_BG));
  wr16(g_regs, AB_PCE_REG_BYR, (uint16_t)((0 - AB_PCE_RASTER_BIAS) & 0x1FF));
  wr16(g_satb, 0, (uint16_t)(64 + AB_PCE_RASTER_BIAS));
  wr16(g_satb, 1, 0x20);
  wr16(g_satb, 2, (uint16_t)(PAT_SPR << 1));
  wr16(g_satb, 3, 0x0001);   /* priority CLEAR = behind background */
  for (int row = 0; row < 16; row++) {
    wr16(g_vram, (PAT_SPR << 6) + row, 0xFFFF);
    wr16(g_vram, (PAT_SPR << 6) + row + 16, 0xFFFF);
  }
  load_frame();
  ab_pce_render_line(&FR, 0, 0, 256, -1, -1, line);
  CHECK(line[0] == 0x21,
        "a behind-background sprite must LOSE to an opaque BG pixel");

  /* ... and WIN where the background is transparent. */
  make_solid_tile(TILE_BG, 0);  /* BG tile now entry 0 = transparent */
  load_frame();
  ab_pce_render_line(&FR, 0, 0, 256, -1, -1, line);
  CHECK((line[0] & 0x0F) == 3,
        "a behind-background sprite must WIN over a transparent BG pixel");

  /* --- x-flip ------------------------------------------------------------ */
  reset_state();
  wr16(g_pal, 0x13, 0x007);
  wr16(g_satb, 0, (uint16_t)(64 + AB_PCE_RASTER_BIAS));
  wr16(g_satb, 1, 0x20);
  wr16(g_satb, 2, (uint16_t)(PAT_SPR << 1));
  wr16(g_satb, 3, (uint16_t)(AB_PCE_SPR_PRIORITY | 0x01));
  /* One pixel lit at the sprite's leftmost column: bit 15 of plane 0. */
  for (int row = 0; row < 16; row++) wr16(g_vram, (PAT_SPR << 6) + row, 0x8000);
  load_frame();
  ab_pce_render_line(&FR, 0, 0, 256, 0, -1, line);
  CHECK((line[0] & 0x0F) != 0 && (line[15] & 0x0F) == 0,
        "unflipped: bit15 must land at the sprite's LEFT edge");
  wr16(g_satb, 3, (uint16_t)(AB_PCE_SPR_PRIORITY | AB_PCE_SPR_XFLIP | 0x01));
  load_frame();
  ab_pce_render_line(&FR, 0, 0, 256, 0, -1, line);
  CHECK((line[15] & 0x0F) != 0 && (line[0] & 0x0F) == 0,
        "x-flip must mirror the sprite across its 16px cell");

  /* --- y-flip ------------------------------------------------------------ */
  reset_state();
  wr16(g_pal, 0x13, 0x007);
  wr16(g_satb, 0, (uint16_t)(64 + AB_PCE_RASTER_BIAS));
  wr16(g_satb, 1, 0x20);
  wr16(g_satb, 2, (uint16_t)(PAT_SPR << 1));
  wr16(g_satb, 3, (uint16_t)(AB_PCE_SPR_PRIORITY | 0x01));
  wr16(g_vram, (PAT_SPR << 6) + 0, 0xFFFF);  /* only sprite row 0 lit */
  load_frame();
  ab_pce_render_line(&FR, 0, 0, 256, 0, -1, line);
  CHECK((line[0] & 0x0F) != 0, "unflipped: row 0 lit at fb row 0");
  wr16(g_satb, 3, (uint16_t)(AB_PCE_SPR_PRIORITY | AB_PCE_SPR_YFLIP | 0x01));
  load_frame();
  ab_pce_render_line(&FR, 0, 0, 256, 0, -1, line);
  CHECK((line[0] & 0x0F) == 0,
        "y-flip must move sprite row 0 to the BOTTOM of the cell");
  ab_pce_render_line(&FR, 15, 0, 256, 0, -1, line);
  CHECK((line[0] & 0x0F) != 0, "y-flip: row 0 must now appear at fb row 15");

  /* --- CR gating + force overrides -------------------------------------- */
  reset_state();
  wr16(g_pal, 0x21, 0x1C0);
  make_solid_tile(TILE_BG, 1);
  for (int c = 0; c < 64; c++) wr16(g_vram, c, (uint16_t)((2u << 12) | TILE_BG));
  wr16(g_regs, AB_PCE_REG_BYR, (uint16_t)((0 - AB_PCE_RASTER_BIAS) & 0x1FF));
  wr16(g_regs, AB_PCE_REG_CR, 0);    /* live CR says everything is OFF */
  load_frame();
  ab_pce_render_line(&FR, 0, 0, 256, -1, -1, line);
  CHECK(line[0] == 0,
        "CR bit7 clear must park the line at the no-background code");
  ab_pce_render_line(&FR, 0, 0, 256, 1, -1, line);
  CHECK(line[0] == 0x21,
        "force_bg must override a live CR that lies about the rendered frame");

  /* --- geometry ---------------------------------------------------------- */
  reset_state();
  load_frame();
  CHECK(ab_pce_width(&FR) == 256, "HDW=31 must give a 256px display");
  wr16(g_regs, AB_PCE_REG_HDR, 0x0427);   /* HDW=39 -> 320 */
  load_frame();
  CHECK(ab_pce_width(&FR) == 320, "HDW=39 must give a 320px display");

  /* --- emit: run coalescing --------------------------------------------- */
  reset_state();
  wr16(g_pal, 0x21, 0x1C0);
  make_solid_tile(TILE_BG, 1);
  for (int c = 0; c < 64; c++) wr16(g_vram, c, (uint16_t)((2u << 12) | TILE_BG));
  wr16(g_regs, AB_PCE_REG_BYR, (uint16_t)((0 - AB_PCE_RASTER_BIAS) & 0x1FF));
  load_frame();
  ab_batch *b = ab_batch_new(4096);
  ab_pce_view v; memset(&v,0,sizeof(v));
  v.scale = 1.0; v.height = 8; v.force_bg = 1; v.force_sprites = 0;
  const int quads = ab_pce_emit(b, &FR, &v, NULL);
  CHECK(quads == 8,
        "a flat 8-row frame must coalesce to ONE quad per row, not 256");
  ab_batch_free(b);

  /* --- emit: mid-row palette split (pce_paldeltas) -----------------------
   * The reason the region exists: pallines is per-LINE, so a colour write
   * that lands MID-line (Eaggy) leaves the row's head rendered through a
   * value no snapshot holds. One event at dot 128 changing the flat frame's
   * colour must split row 0 into TWO runs -- head resolved through oldv,
   * tail through newv -- and change nothing on any other row. */
  {
    static unsigned char pd[AB_PCE_PALDELTAS_SIZE];
    memset(pd, 0, sizeof pd);
    pd[0] = 1; pd[1] = 0;                       /* count=1, not truncated */
    const int ev_vpos = 0 + ab_pce_vpos_origin(8);
    unsigned char *e = pd + AB_PCE_PALDELTA_HDR;
    e[0] = (unsigned char)(ev_vpos & 0xFF); e[1] = (unsigned char)(ev_vpos >> 8);
    e[4] = 128; e[5] = 0;                       /* dot 128 */
    e[6] = 0x21; e[7] = 0;                      /* index 0x21 */
    e[8] = 0x07; e[9] = 0;                      /* oldv: a red-ish 9-bit val */
    e[10] = 0xC0; e[11] = 0x01;                 /* newv == current 0x1C0 */
    FR.paldeltas = pd; FR.has_paldeltas = 1;
    ab_batch *b2 = ab_batch_new(4096);
    const int q2 = ab_pce_emit(b2, &FR, &v, NULL);
    ab_batch_free(b2);
    CHECK(q2 == 9,
          "a mid-row palette event must split its row into two runs");
    ab_pce_test_no_paldeltas = 1;
    ab_batch *b3 = ab_batch_new(4096);
    const int q3 = ab_pce_emit(b3, &FR, &v, NULL);
    ab_batch_free(b3);
    ab_pce_test_no_paldeltas = 0;
    CHECK(q3 == 8,
          "with the palette log disabled the split disappears (control bites)");
    /* A truncated log must be rejected WHOLE by frame_read's gate; here the
     * renderer-side equivalent: has_paldeltas 0 means no splits. */
    FR.has_paldeltas = 0;
    ab_batch *b4 = ab_batch_new(4096);
    const int q4 = ab_pce_emit(b4, &FR, &v, NULL);
    ab_batch_free(b4);
    CHECK(q4 == 8, "an untrusted log contributes nothing");
    FR.paldeltas = NULL;
  }

  /* --- SATB helper: pattern is the substitution key ---------------------- */
  reset_state();
  wr16(g_satb, 2, (uint16_t)(0x123u << 1));
  load_frame();
  CHECK(ab_pce_sprite_pattern(&FR, 0) == 0x123,
        "sprite pattern must be SATB word 2 >> 1");

  /* --- per-scanline VDC state (pce_vdc_reglines) -------------------------
   * The reason this region exists: a game rewriting BXR/BYR per scanline for
   * parallax leaves the frame-end registers describing only the LAST split,
   * so a reconstruction gives every line the same scroll. Two BAT rows of
   * different tiles, and a split that makes fb rows 0 and 1 read DIFFERENT
   * rows, is enough to catch every way of getting this wrong. */
  reset_state();
  wr16(g_pal, 0x21, 0x1C0);            /* sub-palette 2 entry 1 = green */
  wr16(g_pal, 0x32, 0x007);            /* sub-palette 3 entry 2 = blue  */
  make_solid_tile(TILE_BG,     1);
  make_solid_tile(TILE_BG + 1, 2);
  /* BAT is 64 wide (MWR 0x0010). Row 0 -> the entry-1 tile, row 1 -> entry-2. */
  for (int c = 0; c < 64; c++) {
    wr16(g_vram, c,      (uint16_t)((2u << 12) | TILE_BG));
    wr16(g_vram, 64 + c, (uint16_t)((3u << 12) | (TILE_BG + 1)));
  }
  /* Frame-end BYR points at BAT row 0 for fb row 0, and would put fb row 1 on
   * BAT row 0 too (both inside the same 8px tile row). The per-line records
   * below deliberately disagree: raster 8 (= fb row 0) renders BAT row 0 and
   * raster 9 (= fb row 1) renders BAT row 1. Only a renderer that consults the
   * table per line can produce that. */
  wr16(g_regs, AB_PCE_REG_BYR, (uint16_t)((0 - AB_PCE_RASTER_BIAS) & 0x1FF));
  const uint16_t CR_ON = AB_PCE_CR_BG_ON | AB_PCE_CR_SPR_ON;
  wr_regline(0 + AB_PCE_RASTER_BIAS, 0, 0, CR_ON, 0x0010, 0);
  wr_regline(1 + AB_PCE_RASTER_BIAS, 0, 8, CR_ON, 0x0010, 0);
  CHECK(load_frame_lines(), "frame_read must succeed with reglines present");
  CHECK(FR.has_reglines, "has_reglines must be set when the region resolves");

  ab_pce_render_line(&FR, 0, 0, 256, -1, 0, line);
  CHECK(line[0] == 0x21, "per-line: fb row 0 must render BAT row 0");
  ab_pce_render_line(&FR, 1, 0, 256, -1, 0, line);
  CHECK(line[0] == 0x32,
        "per-line: fb row 1 must follow ITS OWN bgy to BAT row 1 -- this is "
        "the whole point of the region");

  /* The record holds the EFFECTIVE BYR. Adding the raster line on top is the
   * obvious wrong turn and would put fb row 1 nine rows further down. */
  ab_pce_regline rl;
  CHECK(ab_pce_regline_at(&FR, 1 + AB_PCE_RASTER_BIAS, &rl) && rl.bgy == 8,
        "regline bgy must be the effective BYR, used directly");

  /* A slot the VDC never rendered must NOT be trusted: zero is a legitimate
   * scroll, so only the valid flag can tell the two apart. */
  CHECK(!ab_pce_regline_at(&FR, 200, &rl),
        "an unwritten line must report invalid, not scroll 0");
  /* fb row 8 (raster 16) has NO record. The two wrong answers are separated
   * here on purpose: falling back gives bg_y = BYR + raster = 8, i.e. BAT
   * row 1 (blue); trusting the zeroed slot gives bgy 0, i.e. BAT row 0
   * (green). Picking a row where both land on the same BAT row would make
   * this assertion, and run.sh's valid-bit control, toothless. */
  ab_pce_render_line(&FR, 8, 0, 256, -1, 0, line);
  CHECK(line[0] == 0x32,
        "an invalid line must fall back to the frame-end registers, NOT be "
        "read as a zeroed record (scroll 0 is a legal value)");

  /* Burst mode blanks the line, and it blanks it to LITERAL BLACK. The VDC
   * returns HUC6270_PIXEL_BLACK and the VCE bypasses the colour table for
   * that code -- so it is NOT the 0x100 idle pixel and NOT the backdrop.
   * Asserting 0 here (the old expectation) let Parasol Stars' blanked
   * transition render flat blue against the hardware's flat black. */
  wr_regline(2 + AB_PCE_RASTER_BIAS, 0, 0, CR_ON, 0x0010, 1);
  load_frame_lines();
  ab_pce_render_line(&FR, 2, 0, 256, -1, -1, line);
  CHECK(line[0] == AB_PCE_LB_BLACK,
        "a burst-mode line must be literal black, not the backdrop");

  /* --- fallback: the region is OPTIONAL ---------------------------------
   * active-bezels ships against several hosts and only a freshly patched
   * geargrafx has 0x1A6. An absent region must degrade to the frame-end
   * registers, not fail the frame -- and must NOT be mistaken for region 0. */
  CHECK(load_frame(), "frame_read must succeed with reglines ABSENT");
  CHECK(!FR.has_reglines,
        "an absent reglines handle (-1) must not be read as region 0");
  ab_pce_render_line(&FR, 1, 0, 256, -1, 0, line);
  CHECK(line[0] == 0x21,
        "fallback: every line uses the frame-end BYR (old behaviour)");
  CHECK(!ab_pce_regline_at(&FR, 8, &rl),
        "regline_at must report nothing when the region is absent");

  if (fails) { printf("ab_pce: %d test(s) FAILED\n", fails); return 1; }
  printf("ab_pce: all tests passed\n");
  (void)g_mesh_calls; (void)g_last_n; (void)g_last_tex; (void)g_verts;
  (void)g_tex_px; (void)g_tex_w; (void)g_tex_h;
  return 0;
}
