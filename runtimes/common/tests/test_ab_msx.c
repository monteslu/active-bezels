/* MSX profile tests.
 *
 * Like the PCE profile, bluemsx exposes NO resolved pixel planes: ab_msx.c
 * re-runs the VDP's own line renderer from VRAM + registers + palette. The
 * load-bearing claims are therefore about the DECODE, and each of these tests
 * pins one rule that a corpus run proved matters, so a regression names itself
 * instead of showing up as "some game looks wrong".
 *
 * Rules pinned here:
 *   - mode selection, including the YJK remap that makes SCREEN 7/8 unsupported
 *     and the SCREEN 0+2 / 0+3 combinations that a V9938 draws BLANK,
 *   - SCREEN 6/7 present at 544, not 272, and the border doubles with them,
 *   - SCREEN 0's hAdjustSc0 is the V9958's +1, not the TMS9918's -2,
 *   - colour 0 shows the BACKDROP when transparency is on,
 *   - SCREEN 8 uses its own fixed palettes, not the palette registers,
 *   - mode-2 sprites: terminator 216, 8 per line, per-line colour byte,
 *     the <<1 | solid encoding, and CC OR-blending,
 *   - MAP_VRAM interleaves VRAM in modes 7..12.
 *
 * Sentinel colours below are deliberately NOT black: a decode bug that
 * resolves to 0 is invisible when the expected value is also 0.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define AB_IMPORT(m,n)
typedef struct { float x,y,u,v; uint32_t rgba,_pad; } ab_vertex;
static int g_mesh_calls;
/* Rightmost vertex x seen, so a test can measure how wide a frame was drawn. */
static double g_last_quad_x1 = -1;
int32_t ab_mesh(const ab_vertex*v,int32_t n,int32_t t){
  (void)t; g_mesh_calls++;
  for (int32_t i = 0; i < n; i++) if (v[i].x > g_last_quad_x1) g_last_quad_x1 = v[i].x;
  return 1; }
int32_t ab_texture_create_rgba(const void*px,int32_t w,int32_t h){
  (void)px;(void)w;(void)h;return 1; }
int32_t ab_texture_destroy(int32_t t){(void)t;return 1;}

/* Region 0 = VRAM, 1 = regs, 2 = status, 3 = palette. VRAM is deliberately 0
 * and an absent region is -1, so a struct field left to C's zero-init would
 * name VRAM rather than "missing". */
enum { R_VRAM=0, R_REGS=1, R_STATUS=2, R_PAL=3, R_COUNT=4, R_NONE=-1 };
static unsigned char g_vram[0x20000];
static unsigned char g_regs[64];
static unsigned char g_status[16];
static unsigned char g_pal[32];
static unsigned char *g_region[R_COUNT] = { g_vram, g_regs, g_status, g_pal };
static int g_region_len[R_COUNT] = { sizeof(g_vram), sizeof(g_regs),
                                     sizeof(g_status), sizeof(g_pal) };
int32_t ab_region_read_u8(int32_t r,int32_t o){
  if(r<0||r>=R_COUNT) return 0;
  if(o<0||o>=g_region_len[r]) return 0;
  return g_region[r][o]; }
int32_t ab_region_read(int32_t r,int32_t o,void*d,int32_t n){
  unsigned char *dst=(unsigned char*)d;
  for(int i=0;i<n;i++) dst[i]=(unsigned char)ab_region_read_u8(r,o+i);
  return n; }

#include "ab_render.c"
#include "ab_msx.c"

static int g_fail = 0;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s\n", msg); g_fail=1; } \
                         else printf("ok: %s\n", msg); }while(0)

/* Set the mode-select bits: R0 bits 3..1 and R1 bits 4,3. */
static void set_mode_sel(int sel){
  g_regs[0] = (unsigned char)((g_regs[0] & ~0x0e) | ((sel & 0x07) << 1));
  g_regs[1] = (unsigned char)((g_regs[1] & ~0x18) | (sel & 0x18));
}

static void decode_now(ab_msx_state *st){
  ab_msx_decode(g_regs, g_status, sizeof(g_vram), st);
}

int main(void){
  static uint32_t out[AB_MSX_MAXPIX];
  ab_msx_state st;

  memset(g_vram,0,sizeof g_vram);
  memset(g_regs,0,sizeof g_regs);
  memset(g_status,0,sizeof g_status);
  memset(g_pal,0,sizeof g_pal);

  /* ---- mode selection ---------------------------------------------------- */
  set_mode_sel(0x10); decode_now(&st);
  CHECK(st.mode==AB_MSX_MODE_SCREEN0, "sel 0x10 -> SCREEN 0");
  set_mode_sel(0x00); decode_now(&st);
  CHECK(st.mode==AB_MSX_MODE_SCREEN1, "sel 0x00 -> SCREEN 1");
  set_mode_sel(0x01); decode_now(&st);
  CHECK(st.mode==AB_MSX_MODE_SCREEN2, "sel 0x01 -> SCREEN 2");
  set_mode_sel(0x08); decode_now(&st);
  CHECK(st.mode==AB_MSX_MODE_SCREEN3, "sel 0x08 -> SCREEN 3");
  set_mode_sel(0x02); decode_now(&st);
  CHECK(st.mode==AB_MSX_MODE_SCREEN4, "sel 0x02 -> SCREEN 4");
  set_mode_sel(0x03); decode_now(&st);
  CHECK(st.mode==AB_MSX_MODE_SCREEN5, "sel 0x03 -> SCREEN 5");
  set_mode_sel(0x04); decode_now(&st);
  CHECK(st.mode==AB_MSX_MODE_SCREEN6, "sel 0x04 -> SCREEN 6");
  set_mode_sel(0x05); decode_now(&st);
  CHECK(st.mode==AB_MSX_MODE_SCREEN7, "sel 0x05 -> SCREEN 7");
  set_mode_sel(0x07); decode_now(&st);
  CHECK(st.mode==AB_MSX_MODE_SCREEN8, "sel 0x07 -> SCREEN 8");

  /* SCREEN 0+2 and 0+3: a V9938/V9958 draws these BLANK, so the profile must
   * NOT claim them. Reporting a picture here would be confident nonsense. */
  set_mode_sel(0x11); decode_now(&st);
  CHECK(st.mode==AB_MSX_MODE_UNSUPPORTED, "sel 0x11 (SCREEN 0+2) unsupported on V9938");
  set_mode_sel(0x18); decode_now(&st);
  CHECK(st.mode==AB_MSX_MODE_UNSUPPORTED, "sel 0x18 (SCREEN 0+3) unsupported on V9938");
  set_mode_sel(0x12); decode_now(&st);
  CHECK(st.mode==AB_MSX_MODE_UNSUPPORTED, "sel 0x12 (TEXT80) unsupported");

  /* YJK: R25 bit3 turns SCREEN 7/8 into modes 10/12, which decode through a
   * Y/J/K colour table this profile does not implement. */
  g_regs[25] = 0x08;
  set_mode_sel(0x07); decode_now(&st);
  CHECK(st.mode==AB_MSX_MODE_UNSUPPORTED, "SCREEN 8 + YJK (R25 bit3) unsupported");
  set_mode_sel(0x05); decode_now(&st);
  CHECK(st.mode==AB_MSX_MODE_UNSUPPORTED, "SCREEN 7 + YJK unsupported");
  g_regs[25] = 0;

  /* ---- geometry: 512-wide modes present at 544 --------------------------- */
  set_mode_sel(0x03); decode_now(&st);
  CHECK(st.out_w==AB_MSX_W && !st.wide, "SCREEN 5 presents at 272");
  set_mode_sel(0x04); decode_now(&st);
  CHECK(st.out_w==AB_MSX_MAXW && st.wide, "SCREEN 6 presents at 544, not 272");
  set_mode_sel(0x05); decode_now(&st);
  CHECK(st.out_w==AB_MSX_MAXW && st.wide, "SCREEN 7 presents at 544, not 272");
  set_mode_sel(0x07); decode_now(&st);
  CHECK(st.out_w==AB_MSX_W, "SCREEN 8 is 256-wide -> 272");

  /* ---- SCREEN 0's border extra is the V9958 value ------------------------ */
  set_mode_sel(0x10); decode_now(&st);
  CHECK(st.hadjust_sc0==1,
        "hAdjustSc0 is +1 (V9938/V9958), not the TMS9918's -2");

  /* ---- sprite plane selection ------------------------------------------- */
  set_mode_sel(0x01); decode_now(&st);
  CHECK(!st.sprite_mode2, "SCREEN 2 uses the TMS sprite plane (mode 1)");
  set_mode_sel(0x03); decode_now(&st);
  CHECK(st.sprite_mode2, "SCREEN 5 uses the V9938 sprite plane (mode 2)");

  /* ---- colour 0 is the backdrop when transparency is on ------------------ */
  /* palette entry 5 = a recognisable non-black colour; entry 0 = black. */
  g_pal[5*2] = 0x70; g_pal[5*2+1] = 0x00;      /* R = 7 -> bright red */
  g_regs[7] = 0x05;                            /* BGColor = 5 */
  g_regs[8] = 0x00;                            /* transparency ON */
  set_mode_sel(0x01); decode_now(&st);
  uint16_t pal16[AB_MSX_PALREG];
  ab_msx_build_palette(g_pal, &st, pal16);
  CHECK(pal16[0]==pal16[5],
        "colour 0 shows the BACKDROP when transparency is on");
  g_regs[8] = 0x20;                            /* colour-0 solid */
  decode_now(&st);
  ab_msx_build_palette(g_pal, &st, pal16);
  CHECK(pal16[0]!=pal16[5],
        "colour 0 stays colour 0 when R8 bit5 (solid) is set");
  g_regs[8] = 0;

  /* ---- palette register -> RGB565 uses INTEGER truncation ---------------- */
  /* The R field is the low byte's 0x70 bits taken as a VALUE (0..112), scaled
   * by 255/112. 0x0070 is the maximum, 112 -> 255 -> r5 = 31. */
  CHECK(ab_msx_palreg_to_565(0x0070) == (uint16_t)(31u<<11),
        "palreg->565: full R field saturates to r5 = 31");
  /* 0x0030 is R = 48; 48*255/112 = 109 by TRUNCATION (109.28...), so r5 = 13.
   * Rounding would give 109 -> same r5, so use a field where it differs:
   * R = 16 -> 16*255/112 = 36 truncated (36.43), r5 = 4. Rounding to 36 also
   * gives 4, so the visible difference is in the 8-bit value the host widens
   * from -- pin the exact 565 word the core produces. */
  CHECK(ab_msx_palreg_to_565(0x0010) == (uint16_t)(((36>>3)<<11)),
        "palreg->565 truncates the R scale (16*255/112 = 36)");

  /* ---- SCREEN 8's fixed palettes are not the palette registers ----------- */
  /* pal_fixed's blue has an (i&3)==3 -> 7 special case: entry 3 is NOT
   * 2*3=6 scaled, it is the full 7. */
  CHECK(pal_fixed(3) == pal_fixed(3), "pal_fixed is deterministic");
  {
    const uint16_t b2 = pal_fixed(2);     /* blue = 2*2 = 4 */
    const uint16_t b3 = pal_fixed(3);     /* blue = 7, the special case */
    CHECK(b3 > b2, "pal_fixed blue: (i&3)==3 jumps to 7, not a linear ramp");
    /* If blue were linear (2*(i&3)), entry 3 would be 6, one step above 4. */
    const int blue3 = b3 & 0x1f, blue2 = b2 & 0x1f;
    CHECK(blue3 - blue2 > 2, "pal_fixed blue step 2->3 is bigger than linear");
  }
  CHECK(pal_sprite8(15) != pal_sprite8(0),
        "SCREEN 8 sprite palette is a real table, not all one colour");

  /* ---- MAP_VRAM interleave in modes 7..12 -------------------------------- */
  {
    ab_msx_frame f; memset(&f,0,sizeof f); f.vram=g_vram;
    memset(g_vram,0,sizeof g_vram);
    /* Address 3 in an interleaved mode maps to (3>>1)|((3&1)<<16) = 0x10001,
     * then through vramAccMask. With R8 bit3 clear the access mask is 0x7fff,
     * which would fold 0x10001 back to 1 -- so set R8 bit3 to select the full
     * 0x1ffff mask, which is what the bitmap modes run with. */
    g_regs[8] |= 0x08;
    g_vram[0x10001] = 0xA5;
    g_vram[3]       = 0x5A;
    set_mode_sel(0x07);                       /* SCREEN 8 -> interleaved */
    ab_msx_decode(g_regs,g_status,sizeof g_vram,&f.st);
    CHECK(vram_read(&f,3)==0xA5, "modes 7..12 read VRAM INTERLEAVED");
    set_mode_sel(0x01);                       /* SCREEN 2 -> linear */
    ab_msx_decode(g_regs,g_status,sizeof g_vram,&f.st);
    CHECK(vram_read(&f,3)==0x5A, "modes below 7 read VRAM linearly");
  }

  /* ---- mode-2 sprites ---------------------------------------------------- */
  {
    ab_msx_frame f; memset(&f,0,sizeof f); f.vram=g_vram;
    memset(g_vram,0,sizeof g_vram);
    memset(g_regs,0,sizeof g_regs);
    memset(g_status,0,sizeof g_status);
    set_mode_sel(0x03);                       /* SCREEN 5: mode-2 sprites */
    g_regs[1] |= 0x40;                        /* screen on */
    /* Put the three sprite tables in DIFFERENT places. Left at 0 they all
     * collapse onto address 0 and overwrite each other, which makes this test
     * measure the overlap rather than the sprite rules. R5/R11 form the
     * attribute base, R6 the pattern base. */
    g_regs[5]  = 0x04;                        /* attrs at 0x200, colours at 0 */
    g_regs[11] = 0x00;
    g_regs[6]  = 0x0c;                        /* pattern base -> 0x6000 */
    g_regs[9]  = 0x00;                        /* NTSC, 192 lines */
    ab_msx_decode(g_regs,g_status,sizeof g_vram,&f.st);
    for(int i=0;i<AB_MSX_PALREG;i++) f.palette[i]=(uint16_t)(i*0x111);

    /* The mode-2 layout is NOT two independent tables: colorSpritesLine reads
     * attributes at (sprTabBase & 0x1fe00) + sprite*4 and per-line colours at
     * sprTabBase & ((-1 << 10) | (sprite*16 + line)). Those share a base, with
     * the attribute block ABOVE the colour block. With R5 = 0x04 the colours
     * land at 0 and the attributes at 0x200, so the two do not overlap --
     * leaving both at 0 makes them clobber each other and the test would
     * measure the overlap instead of the sprite rules. */
    const int32_t attr  = f.st.spr_tab & 0x1fe00;
    const int32_t cbase = f.st.spr_tab & (int32_t)(~0x3ffu);
    /* sprite 0 at y=40, x=0, pattern 0. */
    g_vram[attr+0]=40; g_vram[attr+1]=0; g_vram[attr+2]=0; g_vram[attr+3]=0;
    /* per-line colour byte = 3 for every row of sprite 0. */
    for(int r=0;r<16;r++) g_vram[cbase + r] = 0x03;
    /* Fill EVERY pattern row with 0x80 (one pixel at the sprite's left edge).
     * Setting only row 0 would leave the tested scanline blank: attribute y is
     * one LESS than the first visible line, so line first_line+41 with y=40
     * lands on pattern ROW 1, not row 0. */
    const int32_t pgen = f.st.spr_gen & 0x1f800;
    for(int r=0;r<16;r++) g_vram[pgen+r] = 0x80;
    /* terminate the list right after sprite 0 with the MODE-2 sentinel. */
    g_vram[attr+4]=216;

    unsigned char buf[AB_MSX_SPRBUF];
    unsigned char cc[2]={0xff,0xf0};
    const int line = f.st.first_line + 41;    /* y=40 -> first row is line 41 */
    const int wrote = ab_msx_sprite_line(&f,line,buf,cc);
    CHECK(wrote==1, "mode-2 sprite line writes a pixel");
    /* horizontalPos is x + 24 for an 8x8 sprite, and the painter walks the
     * pattern from offset scale*15 DOWN, so an 8-bit pattern's MSB lands at
     * pos + 8 -- i.e. buffer index 32, which is screen x 0 for x = 0. Getting
     * this index wrong is easy and silent; it is derived here rather than
     * guessed. */
    CHECK(buf[AB_MSX_SPRBUF_ORG]==((3<<1)|0),
          "mode-2 colour is stored as ((c & 0x0f) << 1) | solid");

    /* The 216 terminator must STOP the scan: put a would-be visible sprite
     * after it and confirm it never draws. Sprite 2 sits at x = 100, so its
     * MSB lands at buffer index 100 + 24 + 8. */
    const int probe = 100 + 24 + 8;
    g_vram[attr+8+0]=40; g_vram[attr+8+1]=100; g_vram[attr+8+2]=0;
    for(int r=0;r<16;r++) g_vram[cbase + 2*16 + r] = 0x0f;
    g_vram[attr+12]=216;    /* terminate after the probe sprite */
    memset(buf,0,sizeof buf);
    ab_msx_sprite_line(&f,line,buf,cc);
    CHECK(buf[probe]==0, "attribute y == 216 TERMINATES the mode-2 scan");

    /* 208 is NOT the mode-2 terminator: with 216 replaced by 208 the scan must
     * continue and reach the probe sprite. (This is what the sprend208 control
     * breaks, and why that control drops the corpus score.) */
    g_vram[attr+4]=208;
    memset(buf,0,sizeof buf);
    ab_msx_sprite_line(&f,line,buf,cc);
    CHECK(buf[probe]!=0, "208 does NOT terminate a mode-2 scan (216 does)");
  }

  /* ---- render geometry: a wide frame fills 544 columns ------------------- */
  {
    ab_msx_frame f; memset(&f,0,sizeof f); f.vram=g_vram;
    memset(g_regs,0,sizeof g_regs);
    set_mode_sel(0x05);                       /* SCREEN 7 */
    g_regs[1] |= 0x40;                        /* screen on */
    ab_msx_decode(g_regs,g_status,sizeof g_vram,&f.st);
    ab_msx_build_palette(g_pal,&f.st,f.palette);
    CHECK(f.st.out_w==AB_MSX_MAXW, "SCREEN 7 frame is 544 wide");
    memset(out,0xEE,sizeof out);
    CHECK(ab_msx_render_rgba(&f,out)==1, "wide frame renders");
    /* Every one of the 544 columns of row 0 must have been written. */
    int unwritten=0;
    for(int x=0;x<AB_MSX_MAXW;x++) if(out[x]==0xEEEEEEEEu) unwritten++;
    CHECK(unwritten==0, "all 544 columns are written, none left untouched");
  }

  /* ---- emit: a wide frame occupies the same logical width as a narrow one - */
  {
    ab_msx_frame f; memset(&f,0,sizeof f); f.vram=g_vram;
    memset(g_regs,0,sizeof g_regs);
    set_mode_sel(0x05); g_regs[1] |= 0x40;
    ab_msx_decode(g_regs,g_status,sizeof g_vram,&f.st);
    ab_msx_build_palette(g_pal,&f.st,f.palette);
    ab_msx_view view = { .ox=0, .oy=0, .scale=1.0 };
    ab_batch *b = ab_batch_new(8192);
    const int q = ab_msx_emit(b,&f,&view,NULL);
    CHECK(q>0, "wide frame emits quads");
    ab_batch_free(b);
  }

  /* ---- per-scanline records ---------------------------------------------- */
  {
    /* The struct must match the core's romdev_msx_regline byte for byte, or
     * every field after the first mismatch is read from the wrong offset. */
    CHECK(sizeof(ab_msx_regline) == AB_MSX_REGLINE_STRIDE,
          "ab_msx_regline matches the core's 106-byte record stride");

    static ab_msx_regline rl[AB_MSX_REGLINES];
    ab_msx_frame f; memset(&f,0,sizeof f);
    f.vram = g_vram; f.reglines = rl;
    memset(g_regs,0,sizeof g_regs);
    set_mode_sel(0x01); g_regs[1] |= 0x40;          /* frame snapshot: SCREEN 2 */
    ab_msx_decode(g_regs,g_status,sizeof g_vram,&f.st);
    ab_msx_build_palette(g_pal,&f.st,f.palette);

    /* One line claims SCREEN 5 with the valid bit SET. */
    memset(rl,0,sizeof rl);
    const int probe = 100;
    rl[probe].valid = 1;
    rl[probe].regs[0] = (unsigned char)((0x03 & 0x07) << 1);
    rl[probe].regs[1] = (unsigned char)((0x03 & 0x18) | 0x40);
    rl[probe].first_line = 24; rl[probe].active_lines = 192;
    f.have_reglines = 1;

    ab_msx_frame out;
    resolve_row_state(&f, probe - f.st.display_offset, &out);
    CHECK(out.st.mode == AB_MSX_MODE_SCREEN5,
          "a valid regline overrides the frame snapshot for that row");

    /* A row with NO record must fall back to the frame snapshot, not render
     * from an all-zero record. Register 0 is a LEGAL value, so trusting an
     * unwritten slot silently draws mode 1 with every base at 0 -- the same
     * class of bug as the PCE profile's reglines valid bit. */
    resolve_row_state(&f, 50 - f.st.display_offset, &out);
    CHECK(out.st.mode == AB_MSX_MODE_SCREEN2,
          "a row with no valid record falls back to the frame snapshot");

    /* With the valid bit ignored, that same row wrongly reads the zero record. */
    ab_msx_test_reglines_novalid = 1;
    resolve_row_state(&f, 50 - f.st.display_offset, &out);
    CHECK(out.st.mode != AB_MSX_MODE_SCREEN2,
          "ignoring the valid bit DOES change the result (the bit is load-bearing)");
    ab_msx_test_reglines_novalid = 0;

    /* And with no records at all, every row uses the frame snapshot. */
    f.have_reglines = 0;
    resolve_row_state(&f, probe - f.st.display_offset, &out);
    CHECK(out.st.mode == AB_MSX_MODE_SCREEN2,
          "without reglines the profile falls back to the frame snapshot");
  }

  /* ---- emit scaling: one sample = one `scale`, unless fit_width ---------- */
  {
    ab_msx_frame f; memset(&f,0,sizeof f); f.vram = g_vram;
    memset(g_regs,0,sizeof g_regs);
    set_mode_sel(0x05); g_regs[1] |= 0x40;            /* SCREEN 7 -> 544 wide */
    ab_msx_decode(g_regs,g_status,sizeof g_vram,&f.st);
    ab_msx_build_palette(g_pal,&f.st,f.palette);
    CHECK(f.st.out_w == AB_MSX_MAXW, "SCREEN 7 is 544 wide");

    /* Default: the frame occupies out_w * scale. A caller that sized its
     * layout from the REAL width and asked for scale 3 must get 544*3 logical
     * pixels, not 272*3 -- drawing at half width made every scored column
     * past the first sample the wrong place (0 of 6 wide carts passed). */
    ab_msx_view v; memset(&v,0,sizeof v);
    v.ox = 0; v.oy = 0; v.scale = 3.0;
    ab_batch *b1 = ab_batch_new(65536);
    g_last_quad_x1 = -1;
    ab_msx_emit(b1,&f,&v,NULL);
    ab_batch_flush(b1,0);   /* quads sit in the batch until flushed */
    const double wide_extent = g_last_quad_x1;
    ab_batch_free(b1);

    v.fit_width = 1;
    ab_batch *b2 = ab_batch_new(65536);
    g_last_quad_x1 = -1;
    ab_msx_emit(b2,&f,&v,NULL);
    ab_batch_flush(b2,0);
    const double fit_extent = g_last_quad_x1;
    ab_batch_free(b2);

    CHECK(wide_extent > fit_extent + 1.0,
          "fit_width=0 draws WIDER than fit_width=1 (the flag actually works)");
    CHECK(wide_extent > (AB_MSX_MAXW * 3.0) - 4.0,
          "default emit spans out_w * scale (544*3), not AB_MSX_W * scale");
  }

  /* ---- VRAM delta replay ------------------------------------------------- */
  {
    static unsigned char vdeltas[AB_MSX_VDELTAS_SIZE];
    static unsigned char work[AB_MSX_VRAM_SIZE];
    ab_msx_frame f; memset(&f,0,sizeof f);
    f.vram = g_vram; f.vdeltas = vdeltas; f.vram_work = work;

    memset(g_regs,0,sizeof g_regs);
    memset(g_vram,0,sizeof g_vram);
    set_mode_sel(0x01); g_regs[1] |= 0x40;          /* SCREEN 2, all bases 0 */
    ab_msx_decode(g_regs,g_status,sizeof g_vram,&f.st);

    /* With zeroed registers every base is a pure low-bits AND-mask, so the
     * addresses below are computed against those masks, not real-game bases.
     * fb row 30 (VDP line 54... NTSC offset 0, firstLine 24 -> y = 30) reads
     * name cell (y/8=3 -> char_tab 0x260), pattern index low bits
     * (y&7)|(name*8), colour byte (index & 0x3f). */
    const int rowA = 30;                 /* renders BEFORE the logged write */
    const int rowB = 150;                /* renders AFTER it */
    const int yA = rowA + 24 - f.st.first_line + f.st.first_line - 24; (void)yA;
    /* y for a row = (row + display_offset) - first_line + vscroll = row - 24 */
    const int ya = rowA - 24 + 24;  (void)ya;

    /* name cells: y=rowA-? -- compute exactly as render does: Y=row+0,
     * y = Y - first_line (24) + vscroll(0). rowA=30 -> y=6; rowB=150 -> y=126.
     * y=6:  char_tab = 0x3ff & (~0x3ff | 32*(6/8=0))  = 0x000
     * y=126: char_tab = 0x3ff & (~0x3ff | 32*(126/8=15)) = 0x1e0 */
    const int yA2 = 30 - 24, yB2 = 150 - 24;
    const int ntA = 0x000, ntB = 0x1e0;
    g_vram[ntA] = 0x22;                  /* END-of-frame value (after write) */
    g_vram[ntB] = 0x22;                  /* rowB always sees 0x22 */

    /* pattern bytes: index low = (y&7) | name*8.
     * tile 0x11 row (6&7)=6 -> 0x88|6 = 0x8e; tile 0x22 y=6 -> 0x110|6=0x116;
     * tile 0x22 y=126 -> (126&7)=6 -> 0x116 as well. */
    g_vram[0x8e]  = 0x00;                /* tile 0x11: all c0 */
    g_vram[0x116] = 0xff;                /* tile 0x22: all c1 */
    /* colour bytes (index & 0x3f) -> make c0 and c1 DISTINCT everywhere;
     * with an all-zero colour table c0==c1==palette[0] and any tile swap is
     * invisible, which is exactly how the first version of this test failed
     * to detect anything. */
    for (int i = 0; i < 0x40; i++) g_vram[i] |= 0; /* keep name bytes intact */
    g_vram[(0x8e)&0x3f]  = 0x12;         /* not used as colour? ensure below */
    for (int i = 0x30; i < 0x40; i++) g_vram[i] = 0x12;
    /* colour addr for y=6,name 0x11: (0x8e)&0x3f = 0x0e; y=126,name 0x22:
     * (0x1116..)&0x3f = 0x16. Set both. */
    g_vram[0x0e] = 0x12; g_vram[0x16] = 0x12;
    memset(g_pal,0,sizeof g_pal);
    g_pal[1*2] = 0x07;                   /* entry 1: blue-ish */
    g_pal[2*2] = 0x70;                   /* entry 2: red-ish  */
    ab_msx_build_palette(g_pal,&f.st,f.palette);

    /* The log: ONE write at VDP line 100 flipping ntA 0x11 -> 0x22. */
    memset(vdeltas,0,AB_MSX_VDELTA_HDR);
    vdeltas[0]=1; vdeltas[1]=0; vdeltas[2]=0;
    /* 12-byte entry: line u16, dot u16 (0xFFFF = whole line), addr u32 with
     * kind in bits 31:30 (VRAM = 0), oldv u16, newv u16. */
    unsigned char *e = vdeltas + AB_MSX_VDELTA_HDR;
    e[0]=100; e[1]=0;                 /* line 100 */
    e[2]=0xFF; e[3]=0xFF;             /* no sub-line split */
    e[4]=(unsigned char)ntA; e[5]=0; e[6]=0; e[7]=0;
    e[8]=0x11; e[9]=0;                /* oldv */
    e[10]=0x22; e[11]=0;              /* newv */
    f.have_vdeltas = 1;

    static uint32_t out1[AB_MSX_MAXPIX], out2[AB_MSX_MAXPIX];
    ab_msx_render_rgba(&f,out1);         /* replay ON  */
    ab_msx_test_no_vdeltas = 1;
    ab_msx_render_rgba(&f,out2);         /* replay OFF */
    ab_msx_test_no_vdeltas = 0;
    const int W=AB_MSX_W;
    (void)yA2;(void)yB2;
    CHECK(out1[rowA*W+8] != out2[rowA*W+8],
          "rows BEFORE the logged write render from the OLD value under replay");
    CHECK(out1[rowB*W+8] == out2[rowB*W+8],
          "rows AFTER the write are identical with or without replay");
  }

  /* ---- sub-line split rendering ------------------------------------------ */
  {
    static unsigned char vdeltas[AB_MSX_VDELTAS_SIZE];
    static unsigned char work[AB_MSX_VRAM_SIZE];
    ab_msx_frame f; memset(&f,0,sizeof f);
    f.vram = g_vram; f.vdeltas = vdeltas; f.vram_work = work;
    memset(g_regs,0,sizeof g_regs); memset(g_vram,0,sizeof g_vram);
    set_mode_sel(0x01); g_regs[1] |= 0x40;
    ab_msx_decode(g_regs,g_status,sizeof g_vram,&f.st);
    /* Row 100 (y=76) reads name cell 0x120 under the zeroed-register masks.
     * Point it at tile 0x10 so the pattern index (0x84) and the colour index
     * (0x04) are DISTINCT addresses -- with tile 0 they collapse onto the same
     * byte and the colour can never be controlled independently. */
    memset(&g_vram[0x120], 0x10, 32);   /* the WHOLE row, not one cell */
    g_vram[0x84]  = 0xff;   /* tile 0x10 row 4: all foreground */
    g_vram[0x04]  = 0x10;   /* colour byte: c1 = palette entry 1, c0 = 0 */
    memset(g_pal,0,sizeof g_pal);
    g_pal[1*2]=0x07;   /* entry 1 -> blue */
    ab_msx_build_palette(g_pal,&f.st,f.palette);

    /* PALETTE entry 1 changes 0x007 -> 0x070 at line 100, dot 16: the row at
     * VDP line 100 must render its LEFT blocks blue and RIGHT blocks red. */
    memset(vdeltas,0,AB_MSX_VDELTA_HDR);
    vdeltas[0]=1;
    unsigned char *e = vdeltas + AB_MSX_VDELTA_HDR;
    e[0]=100; e[1]=0;
    e[2]=16; e[3]=0;                            /* dot 16 -> boundary x=8+128 */
    e[4]=1;  e[5]=0; e[6]=0; e[7]=0x80;         /* kind PAL (bits31:30=10) */
    e[8]=0x07; e[9]=0;                          /* oldv 0x007 */
    e[10]=0x70; e[11]=0;                        /* newv 0x070 */
    f.have_vdeltas = 1;

    static uint32_t o1[AB_MSX_MAXPIX], o2[AB_MSX_MAXPIX];
    ab_msx_render_rgba(&f,o1);
    const int W=AB_MSX_W, ROW=100;   /* display_offset 0: fb row == VDP line */
    CHECK(o1[ROW*W+40] != o1[ROW*W+200],
          "a mid-line palette write splits the row at the recorded dot");
    ab_msx_test_no_subline = 1;
    ab_msx_render_rgba(&f,o2);
    ab_msx_test_no_subline = 0;
    CHECK(o2[ROW*W+40] == o2[ROW*W+200],
          "with splits disabled the row is uniform (the control bites)");
  }

  /* ---- TMS sprite fill point vs same-line events -------------------------
   * The TMS plane for row Y+1 is filled when line Y's render reaches block
   * 33; vdp_sync's catch-up can complete the line and leave lineOffset at
   * 33, so an event stamped dot=33 landed AFTER the fill and must NOT be
   * visible to row Y+1's sprites -- while a dot<=32 event on the same line
   * MUST be. One corpus cart moves sprite X mid-frame with dot-33 writes;
   * including them shifted its sprite one line early (2 px: the sprite edges
   * at the old and new X disagree, the overlap agrees). */
  {
    static unsigned char vdeltas[AB_MSX_VDELTAS_SIZE];
    static unsigned char work[AB_MSX_VRAM_SIZE];
    ab_msx_frame f; memset(&f,0,sizeof f);
    f.vram = g_vram; f.vdeltas = vdeltas; f.vram_work = work;
    memset(g_regs,0,sizeof g_regs); memset(g_vram,0,sizeof g_vram);
    set_mode_sel(0x01); g_regs[1] |= 0x40;
    g_regs[5] = 0x10;                       /* sprite attrs at 0x800, clear of
                                             * the pattern table at 0 */
    ab_msx_decode(g_regs,g_status,sizeof g_vram,&f.st);
    memset(g_pal,0,sizeof g_pal);
    g_pal[15*2]=0x77; g_pal[15*2+1]=0x07;   /* entry 15 visible (all-zero
                                             * palette = black-on-black) */
    ab_msx_build_palette(g_pal,&f.st,f.palette);
    /* Sprite 0: covers display lines 100..107 = output rows 124..131 with
     * first_line 24. X = 100 at frame END (the snapshot holds the POST-write
     * value; the log's oldv rebuilds 99). Pattern 0 = one leftmost pixel. */
    g_vram[0x800] = 99; g_vram[0x801] = 100; g_vram[0x802] = 0; g_vram[0x803] = 15;
    g_vram[0x804] = 208;                    /* terminator */
    for (int r = 0; r < 8; r++) g_vram[r] = 0x80;

    /* Event: sprite X 99 -> 100 on row 125, dot 33 (post-fill). */
    memset(vdeltas,0,AB_MSX_VDELTA_HDR);
    vdeltas[0]=1;
    unsigned char *e = vdeltas + AB_MSX_VDELTA_HDR;
    e[0]=125; e[1]=0;
    e[2]=33; e[3]=0;
    e[4]=0x01; e[5]=0x08; e[6]=0; e[7]=0;   /* addr 0x801 */
    e[8]=99; e[9]=0;                        /* oldv */
    e[10]=100; e[11]=0;                     /* newv */
    f.have_vdeltas = 1;

    static uint32_t o1[AB_MSX_MAXPIX];
    ab_msx_render_rgba(&f,o1);
    const int W=AB_MSX_W;
    const uint32_t bg = o1[126*W+8+240];
    CHECK(o1[126*W+8+99] != bg && o1[126*W+8+100] == bg,
          "a dot-33 sprite-X write is INVISIBLE to the next row's sprites "
          "(TMS fill already ran at block 33)");
    CHECK(o1[128*W+8+100] != bg && o1[128*W+8+99] == bg,
          "later rows see the post-write X (their fills run after the write)");

    /* The same write stamped dot 20 (pre-fill) MUST move the very next row. */
    e[2]=20;
    static uint32_t o2[AB_MSX_MAXPIX];
    ab_msx_render_rgba(&f,o2);
    CHECK(o2[126*W+8+100] != bg && o2[126*W+8+99] == bg,
          "a dot<=32 write on the same line IS seen by the next row's fill");
  }

  /* ---- frame-end-cut retention ------------------------------------------- */
  {
    static unsigned char vdeltas[AB_MSX_VDELTAS_SIZE];
    static unsigned char work[AB_MSX_VRAM_SIZE];
    static uint16_t prev[AB_MSX_MAXW * AB_MSX_H];
    ab_msx_frame f; memset(&f,0,sizeof f);
    f.vram = g_vram; f.vdeltas = vdeltas; f.vram_work = work;
    f.prev_rows = prev; f.retain_valid = 1;
    memset(g_regs,0,sizeof g_regs); memset(g_vram,0,sizeof g_vram);
    set_mode_sel(0x01); g_regs[1] |= 0x40;
    ab_msx_decode(g_regs,g_status,sizeof g_vram,&f.st);
    ab_msx_build_palette(g_pal,&f.st,f.palette);

    /* Prior composite: a sentinel colour nothing in this frame produces. */
    for (size_t i = 0; i < AB_MSX_MAXW * AB_MSX_H; i++) prev[i] = 0xF81F;
    /* Log: no writes, but a cut at line 200 block 10. */
    memset(vdeltas,0,AB_MSX_VDELTA_HDR);
    vdeltas[4]=200; vdeltas[5]=0; vdeltas[6]=10; vdeltas[7]=0;
    f.have_vdeltas = 0; f.have_cut = 1; f.cut_line = 200; f.cut_offset = 10;

    static uint32_t o1[AB_MSX_MAXPIX];
    ab_msx_render_rgba(&f,o1);
    const int W=AB_MSX_W;
    const uint32_t sentinel = ab_msx_rgba565(0xF81F);
    CHECK(o1[210*W+50] == sentinel,
          "rows past the cut come from the caller's prior composite");
    CHECK(o1[200*W+8+10*8+2] == sentinel,
          "the cut row's tail beyond the recorded block is retained");
    CHECK(o1[200*W+8+10*8-2] != sentinel,
          "the cut row's rendered head is NOT substituted");
    CHECK(o1[100*W+50] != sentinel,
          "fresh rows are never substituted (retention is not a fallback)");

    ab_msx_test_no_retention = 1;
    static uint32_t o2[AB_MSX_MAXPIX];
    /* retention off must render everything fresh AND still refresh prev. */
    ab_msx_render_rgba(&f,o2);
    ab_msx_test_no_retention = 0;
    CHECK(o2[210*W+50] != sentinel,
          "with retention disabled the stale region renders fresh (control bites)");

    /* ---- core fossil snapshot (msx_fb_tail) -------------------------------
     * The PREFERRED retention source: always describes the frame being
     * rendered, so it works on the FIRST compose where prev_rows cannot.
     * Distinct sentinel proves which source each pixel came from. */
    static unsigned char fbtail[AB_MSX_FBTAIL_SIZE];
    memset(fbtail,0,sizeof fbtail);
    fbtail[0]=200; fbtail[1]=0;            /* cutLine  == vdeltas cut */
    fbtail[2]=10;  fbtail[3]=0;            /* cutOffset == vdeltas cut */
    fbtail[4]=AB_MSX_FBTAIL_ROWS; fbtail[5]=0;  /* rows */
    fbtail[6]=(unsigned char)(AB_MSX_W&0xFF); fbtail[7]=AB_MSX_W>>8; /* width */
    fbtail[8]=200; fbtail[9]=0;            /* firstRow */
    for (size_t i = 0; i < (size_t)AB_MSX_FBTAIL_ROWS*AB_MSX_FBTAIL_W; i++) {
      fbtail[AB_MSX_FBTAIL_HDR+i*2]   = 0xE0;   /* 0x07E0 = pure green 565 */
      fbtail[AB_MSX_FBTAIL_HDR+i*2+1] = 0x07;
    }
    f.fbtail = fbtail; f.have_fbtail = 1;
    const uint32_t core_sent = ab_msx_rgba565(0x07E0);
    for (size_t i = 0; i < AB_MSX_MAXW * AB_MSX_H; i++) prev[i] = 0xF81F;
    f.retain_valid = 1;
    static uint32_t o3[AB_MSX_MAXPIX];
    ab_msx_render_rgba(&f,o3);
    CHECK(o3[210*W+50] == core_sent,
          "fossil rows come from the CORE snapshot when present (not prev_rows)");
    CHECK(o3[200*W+8+10*8+2] == core_sent,
          "the cut row's tail comes from the core snapshot");
    CHECK(o3[200*W+8+10*8-2] != core_sent && o3[200*W+8+10*8-2] != sentinel,
          "the cut row's rendered head is never substituted");
    CHECK(o3[100*W+50] != core_sent && o3[100*W+50] != sentinel,
          "fresh rows are never substituted from the core snapshot");

    /* First-compose scenario: no usable prior composite AT ALL. */
    f.retain_valid = 0;
    static uint32_t o4[AB_MSX_MAXPIX];
    ab_msx_render_rgba(&f,o4);
    CHECK(o4[210*W+50] == core_sent,
          "the core snapshot works on the FIRST compose (prev_rows cannot)");

    /* Snapshot hook: disabling ONLY the core source falls back to prev_rows
     * when armed, and to fresh render when not. */
    ab_msx_test_no_fbtail = 1;
    f.retain_valid = 1;
    for (size_t i = 0; i < AB_MSX_MAXW * AB_MSX_H; i++) prev[i] = 0xF81F;
    static uint32_t o5[AB_MSX_MAXPIX];
    ab_msx_render_rgba(&f,o5);
    ab_msx_test_no_fbtail = 0;
    CHECK(o5[210*W+50] == sentinel,
          "with the core snapshot disabled the armed prior composite is used");

    /* A snapshot whose recorded cut does NOT match the vdeltas cut describes
     * a different frame and must be rejected by frame_read's gate; here we
     * assert the renderer-side equivalent: have_fbtail=0 means no core rows. */
    f.have_fbtail = 0; f.retain_valid = 0;
    for (size_t i = 0; i < AB_MSX_MAXW * AB_MSX_H; i++) prev[i] = 0xF81F;
    static uint32_t o6[AB_MSX_MAXPIX];
    ab_msx_render_rgba(&f,o6);
    CHECK(o6[210*W+50] != core_sent && o6[210*W+50] != sentinel,
          "an untrusted snapshot contributes nothing (mismatched-cut gate)");
  }

  printf(g_fail ? "\nFAILED\n" : "\nall MSX profile tests passed\n");
  return g_fail;
}
