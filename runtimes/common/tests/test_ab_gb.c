/* GB/GBC profile tests. The load-bearing claims are that a frame becomes a
 * few hundred quads instead of 23,040 pixels, and that the priority /
 * blank-frame / valid-bit rules the 1396-cart exact run depends on are
 * actually implemented -- assert them, do not hope.
 *
 * The sentinel colours below are deliberately NOT black. A transparency or
 * fallback bug that resolves to 0 is invisible when the expected colour is
 * also 0; that is exactly how the NES transparency assert was unfalsifiable
 * until palette[0] became a sentinel.
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
#define GBT_REGION_BYTES (144*160*2)
static unsigned char g_regions[8][GBT_REGION_BYTES];
int32_t ab_region_read_u8(int32_t r,int32_t o){
  if(r<0||r>=8) return 0; if(o<0||o>=GBT_REGION_BYTES) return 0; return g_regions[r][o]; }
/* Real bulk copy, so the tests exercise the FAST path. */
int32_t ab_region_read(int32_t r,int32_t o,void*d,int32_t n){
  if(r<0||r>=8||o<0||n<=0) return 0;
  if(o+n > GBT_REGION_BYTES) n = GBT_REGION_BYTES-o;
  if(n<=0) return 0; memcpy(d,&g_regions[r][o],(size_t)n); return n; }

#include "ab_render.c"
#include "ab_gb.c"

static int fails=0;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s\n", msg); fails++; } }while(0)

enum { R_LINEREGS=0, R_BGPIX=1, R_SPRPIX=2, R_PALLINE=3, R_BGCOL=4, R_SPRCOL=5, R_OAM=6 };

/* Sentinels: distinct, non-zero, and distinguishable after 565->8888. */
#define C_BG   0x7BEFu   /* mid grey-ish */
#define C_SPR  0xF800u   /* pure red */
#define C_FILL 0xFFDFu   /* gambatte's white (note the g<<6 quirk: NOT FFFF) */

static void set_pal(int line,int obj,int idx,uint16_t v){
  size_t o=(size_t)line*AB_GB_PAL_STRIDE+(obj?64:0)+(size_t)idx*2;
  g_regions[R_PALLINE][o]=(unsigned char)(v&0xFF);
  g_regions[R_PALLINE][o+1]=(unsigned char)(v>>8);
}
static void set_lr(int line,int off,unsigned char v){
  g_regions[R_LINEREGS][(size_t)line*AB_GB_LR_STRIDE+off]=v;
}

int main(void){
  static unsigned char bgbuf[AB_GB_PIX], sprbuf[AB_GB_PIX], supp[AB_GB_PIX];
  static unsigned char bgc[AB_GB_PIX*2], sprc[AB_GB_PIX*2];
  ab_gb_frame f; memset(&f,0,sizeof(f));
  f.bgpix=bgbuf; f.sprpix=sprbuf; f.bgcol15=bgc; f.sprcol15=sprc;

  ab_gb_regions R;
  R.lineregs=R_LINEREGS; R.bgpix=R_BGPIX; R.sprpix=R_SPRPIX;
  R.palline=R_PALLINE; R.bgcol15=-1; R.sprcol15=-1; R.oam=R_OAM;

  /* Baseline frame: BG entry 1 everywhere (valid), no sprites, LCD on with
   * BG enabled (LCDC bit0 = master priority on for the CGB path). */
  memset(g_regions[R_BGPIX], AB_GB_PIX_VALID|1, AB_GB_PIX);
  memset(g_regions[R_SPRPIX], 0, AB_GB_PIX);
  memset(g_regions[R_LINEREGS], 0, sizeof(g_regions[R_LINEREGS]));
  for(int y=0;y<AB_GB_H;y++){ set_lr(y,AB_GB_LR_LCDC,0x91); set_pal(y,0,1,C_BG); set_pal(y,1,1,C_SPR); }

  CHECK(ab_gb_frame_read(&R,&f)==1, "frame_read succeeds");
  CHECK(f.have_percolour==0, "per-colour planes absent when regions are -1");

  ab_gb_view v; v.ox=0; v.oy=0; v.scale=7.0;
  ab_batch *b = ab_batch_new(4096);

  /* --- 1. a flat frame coalesces to ONE quad per line -------------------- */
  ab_batch_reset(b);
  int q = ab_gb_emit_background(b, &f, &v);
  CHECK(q==AB_GB_H, "flat background = one quad per line (run coalescing works)");
  CHECK(q < AB_GB_PIX/10, "quad count is far below the pixel count");
  g_mesh_calls=0; ab_batch_flush(b,0);
  CHECK(g_mesh_calls==1, "the whole background layer is ONE mesh call");

  /* colour actually made it through the 565->8888 widening */
  CHECK(g_last_n>0 && g_verts[0].rgba==ab_gb_rgba565(C_BG), "BG quad carries the palette colour");

  /* --- 2. sprite priority ------------------------------------------------ */
  /* A sprite pixel with entry != 0 and no bgpriority WINS over a non-zero BG. */
  bgbuf[0]  = AB_GB_PIX_VALID|1;
  sprbuf[0] = AB_GB_PIX_VALID|1;
  CHECK(ab_gb_spr_visible(bgbuf[0],sprbuf[0],0,1)==1, "DMG: plain sprite beats BG");

  /* bgpriority set AND BG entry non-zero -> sprite hidden. */
  sprbuf[0] = AB_GB_PIX_VALID|AB_GB_PIX_SPRIO|1;
  CHECK(ab_gb_spr_visible(bgbuf[0],sprbuf[0],0,1)==0, "DMG: bgpriority sprite loses to non-zero BG");
  /* ...but shows where the BG entry is 0. */
  CHECK(ab_gb_spr_visible(AB_GB_PIX_VALID|0,sprbuf[0],0,1)==1, "DMG: bgpriority sprite shows over BG entry 0");

  /* CGB: the BG TILE's attr bit7 also forces the sprite behind. */
  sprbuf[0] = AB_GB_PIX_VALID|1;
  CHECK(ab_gb_spr_visible(AB_GB_PIX_VALID|AB_GB_PIX_PRIO|1,sprbuf[0],1,1)==0,
        "CGB: BG tile-attr priority hides a plain sprite");
  /* ...and LCDC bit0 clear (master priority off) disables the whole rule. */
  CHECK(ab_gb_spr_visible(AB_GB_PIX_VALID|AB_GB_PIX_PRIO|1,sprbuf[0],1,0)==1,
        "CGB: master priority off -> sprites win everywhere");

  /* entry 0 is transparent, never a sprite pixel */
  CHECK(ab_gb_spr_visible(AB_GB_PIX_VALID|1,AB_GB_PIX_VALID|0,0,1)==0,
        "sprite entry 0 is transparent");
  /* an invalid (never-written) sprite byte is not a sprite */
  CHECK(ab_gb_spr_visible(AB_GB_PIX_VALID|1,1,0,1)==0,
        "sprite pixel without the VALID bit is ignored");

  /* --- 3. sprite layer emits only sprite pixels -------------------------- */
  memset(g_regions[R_SPRPIX], 0, AB_GB_PIX);
  for(int x=0;x<8;x++) g_regions[R_SPRPIX][10*AB_GB_W+x] = AB_GB_PIX_VALID|1;
  ab_gb_frame_read(&R,&f);
  ab_batch_reset(b);
  q = ab_gb_emit_sprites(b, &f, &v, NULL);
  CHECK(q==1, "an 8px sprite run is ONE quad");
  g_mesh_calls=0; ab_batch_flush(b,0);
  CHECK(g_verts[0].rgba==ab_gb_rgba565(C_SPR), "sprite quad uses the OBJ palette");

  /* suppression removes it entirely (the HD-replacement path) */
  memset(supp,0,AB_GB_PIX);
  for(int x=0;x<8;x++) supp[10*AB_GB_W+x]=1;
  ab_batch_reset(b);
  q = ab_gb_emit_sprites(b, &f, &v, supp);
  CHECK(q==0, "suppressed sprite pixels emit nothing");

  /* --- 4. blank frame (LCD off) ----------------------------------------- */
  for(int y=0;y<AB_GB_H;y++){ set_lr(y,AB_GB_LR_BLANK,1); set_pal(y,0,0,C_FILL); }
  ab_gb_frame_read(&R,&f);
  ab_batch_reset(b);
  q = ab_gb_emit_background(b, &f, &v);
  CHECK(q==AB_GB_H, "blank frame = one full-width quad per line");
  g_mesh_calls=0; ab_batch_flush(b,0);
  CHECK(g_verts[0].rgba==ab_gb_rgba565(C_FILL),
        "blank frame uses the captured fill colour, not black");
  ab_batch_reset(b);
  CHECK(ab_gb_emit_sprites(b,&f,&v,NULL)==0, "blank frame draws no sprites");
  for(int y=0;y<AB_GB_H;y++) set_lr(y,AB_GB_LR_BLANK,0);

  /* --- 5. the RGB565 widening, including gambatte's green quirk ---------- */
  /* White is 0xFFDF (5-bit green at <<6 leaves bit 5 clear), and it must
   * widen to full 0xFF on every channel. */
  /* 0xFFDF is gambatte's white. Green is 62/63, NOT 63, because the core
   * packs a 5-bit green into the 6-bit field at <<6 -- so the widened value
   * is fffbff, not ffffff. This is the quirk the whole corpus depends on. */
  CHECK(ab_gb_rgba565(0xFFDFu)==0xFFFBFFFFu, "0xFFDF widens to fffbff (g<<6 quirk)");
  CHECK(ab_gb_rgba565(0x0000u)==0x000000FFu, "0x0000 widens to opaque black");
  CHECK((ab_gb_rgba565(0xF800u)>>24)==0xFF, "red channel widens to 0xFF");
  CHECK(((ab_gb_rgba565(0xF800u)>>16)&0xFF)==0x00, "red-only has no green");

  /* --- 6. per-pixel colour planes are preferred, and valid-gated --------- */
  R.bgcol15=R_BGCOL; R.sprcol15=R_SPRCOL;
  memset(g_regions[R_BGPIX], AB_GB_PIX_VALID|1, AB_GB_PIX);
  memset(g_regions[R_SPRPIX], 0, AB_GB_PIX);
  for(int i=0;i<AB_GB_PIX;i++){ g_regions[R_BGCOL][i*2]=(unsigned char)(C_SPR&0xFF);
                                g_regions[R_BGCOL][i*2+1]=(unsigned char)(C_SPR>>8); }
  ab_gb_frame_read(&R,&f);
  CHECK(f.have_percolour==1, "per-colour planes detected when present");
  ab_batch_reset(b); ab_gb_emit_background(b,&f,&v);
  g_mesh_calls=0; ab_batch_flush(b,0);
  CHECK(g_verts[0].rgba==ab_gb_rgba565(C_SPR),
        "per-pixel colour plane overrides the per-line palette");

  /* A pixel WITHOUT the valid bit must fall back to the per-line table even
   * though the colour plane holds a legal value there. 0 is a colour, not a
   * sentinel -- this exact confusion rendered 10 carts entirely black. */
  bgbuf[0]=1;  /* entry 1, VALID CLEAR */
  memcpy(f.bgpix,bgbuf,1);
  {
    uint32_t viaplane = ab_gb_rgba565(C_SPR);
    uint32_t viatable = ab_gb_rgba565(C_BG);
    ab_batch_reset(b); ab_gb_emit_background(b,&f,&v);
    g_mesh_calls=0; ab_batch_flush(b,0);
    CHECK(g_verts[0].rgba==viatable && viatable!=viaplane,
          "invalid pixel falls back to the per-line palette");
  }

  /* --- 7. OAM marking uses GB offsets, not NES ones ---------------------- */
  R.bgcol15=-1; R.sprcol15=-1;
  memset(g_regions[R_OAM],0,160);
  /* One object at screen (0,0): GB OAM stores y+16, x+8. */
  g_regions[R_OAM][0]=16; g_regions[R_OAM][1]=8; g_regions[R_OAM][2]=0x42;
  ab_gb_frame_read(&R,&f);
  {
    ab_registry *reg = ab_registry_new();
    ab_sub_rule rule; memset(&rule,0,sizeof(rule));
    rule.tiles[0]=0x42; rule.tile_count=1; rule.texture=1; rule.tex_w=8; rule.tex_h=8;
    ab_registry_add_sprite(reg,&rule);
    const ab_sub_rule *hit=NULL; ab_gb_bounds bounds;
    memset(supp,0,AB_GB_PIX);
    int n = ab_gb_mark_sprites(&f,reg,8,supp,&hit,&bounds);
    CHECK(n==1, "matching OAM tile is marked");
    CHECK(bounds.x0==0 && bounds.y0==0, "GB OAM offsets: screen = (x-8, y-16)");
    CHECK(bounds.x1==8 && bounds.y1==8, "8x8 sprite bounds");
    CHECK(supp[0]==1, "marked sprite pixels are suppressed");
    /* An object parked off-screen (y >= 160) is ignored. */
    g_regions[R_OAM][0]=200; ab_gb_frame_read(&R,&f);
    memset(supp,0,AB_GB_PIX);
    CHECK(ab_gb_mark_sprites(&f,reg,8,supp,&hit,&bounds)==0, "off-screen OAM ignored");
    ab_registry_free(reg);
  }

  ab_batch_free(b);
  if(fails){ printf("%d FAILURES\n",fails); return 1; }
  printf("ab_gb: all tests passed\n");
  return 0;
}
