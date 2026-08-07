/* NES profile tests. The load-bearing claim is that a frame becomes a few
 * hundred quads instead of 57,344 pixels -- assert it, do not hope. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define AB_IMPORT(m,n)
typedef struct { float x,y,u,v; uint32_t rgba,_pad; } ab_vertex;
static int g_mesh_calls, g_last_n; static int32_t g_last_tex;
int32_t ab_mesh(const ab_vertex*v,int32_t n,int32_t t){(void)v;g_last_n=n;g_last_tex=t;g_mesh_calls++;return 1;}
static uint32_t g_tex_px[65536]; static int g_tex_w,g_tex_h,g_next=1;
int32_t ab_texture_create_rgba(const void*px,int32_t w,int32_t h){
  size_t n=(size_t)w*h*4; if(n>sizeof(g_tex_px)) n=sizeof(g_tex_px);
  g_tex_w=w;g_tex_h=h;memcpy(g_tex_px,px,n);return g_next++; }
int32_t ab_texture_destroy(int32_t t){(void)t;return 1;}
static unsigned char g_regions[8][256*240];
int32_t ab_region_read_u8(int32_t r,int32_t o){
  if(r<0||r>=8) return 0; if(o<0||o>=256*240) return 0; return g_regions[r][o]; }
/* Real bulk copy, so the NES tests exercise the FAST path. */
int32_t ab_region_read(int32_t r,int32_t o,void*d,int32_t n){
  if(r<0||r>=8||o<0||n<=0) return 0;
  if(o+n > 256*240) n = 256*240-o;
  if(n<=0) return 0; memcpy(d,&g_regions[r][o],(size_t)n); return n; }

#include "ab_render.c"
#include "ab_nes.c"

static int fails=0;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s\n", msg); fails++; } }while(0)

enum { R_BGVAL=0, R_SPRD=1, R_PAL=2, R_PALRGB=3, R_OAM=4, R_MASK=5, R_BGPIX=6 };

int main(void){
  static unsigned char bgbuf[256*240], sprbuf[256*240], supp[256*240];
  ab_nes_frame f; memset(&f,0,sizeof(f));
  f.bgval=bgbuf; f.sprdrawn=sprbuf;

  /* background: flat colour 0x0F everywhere; sprites: all transparent */
  memset(g_regions[R_BGVAL], 0x0F, 256*240);
  memset(g_regions[R_SPRD], AB_NES_SPR_TRANSPARENT, 256*240);
  /* palrgb: make index 0x0F a recognisable colour */
  for(int i=0;i<64;i++){ g_regions[R_PALRGB][i*3]=i; g_regions[R_PALRGB][i*3+1]=0; g_regions[R_PALRGB][i*3+2]=0; }
  memset(g_regions[R_MASK], 0x18, 240);   /* rendering on, no emphasis */

  ab_nes_regions reg = { .chr=-1, .palette=R_PAL, .palrgb=R_PALRGB, .oam=R_OAM,
                         .ppureg=-1, .masklines=R_MASK, .bgval=R_BGVAL, .sprdrawn=R_SPRD };
  CHECK(ab_nes_frame_read(&reg,&f)==1, "frame read");
  CHECK(f.have_palrgb==1, "palrgb present");
  CHECK(f.have_masklines==1, "masklines present");

  /* colour: index 0x0F through palrgb, no emphasis */
  uint32_t c = ab_nes_rgba(&f, 0x0F, 0x18);
  CHECK(((c>>24)&0xFF)==0x0F, "palrgb red channel used");
  /* bit-6 flagged value must resolve the SAME as unflagged (the 64-entry trap) */
  CHECK(ab_nes_rgba(&f,0x0F|0x40,0x18)==c, "bit-6 flagged value masks correctly");
  /* greyscale ANDs the INDEX with 0x30 */
  uint32_t g = ab_nes_rgba(&f, 0x0F, 0x19);
  CHECK(((g>>24)&0xFF)==0x00, "greyscale masks index to 0x30 band");
  /* emphasis changes the colour */
  CHECK(ab_nes_rgba(&f,0x20,0x18) != ab_nes_rgba(&f,0x20,0x18|0x20), "emphasis applied");

  ab_nes_view view = { .ox=0, .oy=0, .scale=4.0 };
  ab_batch *b = ab_batch_new(1024);

  /* FLAT background: 224 rows x 1 run = 224 quads, NOT 57344 pixels. */
  int q = ab_nes_emit_background(b,&f,&view,0x18,NULL);
  CHECK(q==AB_NES_H, "flat bg coalesces to one quad per row");
  int before=g_mesh_calls;
  ab_batch_flush(b,0);
  CHECK(g_mesh_calls==before+1, "whole background is ONE mesh call");
  CHECK(g_last_tex==0, "background flushed untextured (vertex colour)");

  /* sprites all transparent -> nothing drawn */
  ab_batch_reset(b);
  q = ab_nes_emit_sprites(b,&f,&view,0x18,NULL);
  CHECK(q==0, "transparent sprite layer draws nothing");

  /* one opaque sprite run of 8 px on line 100 */
  for(int x=10;x<18;x++) g_regions[R_SPRD][100*256+x]=0x21;
  ab_nes_frame_read(&reg,&f);
  ab_batch_reset(b);
  q = ab_nes_emit_sprites(b,&f,&view,0x18,NULL);
  CHECK(q==1, "8px sprite run = ONE quad");

  /* behind-background sprite hidden where bg is opaque (bg bit6 clear) */
  for(int x=10;x<18;x++) g_regions[R_SPRD][100*256+x]=0x21|AB_NES_SPR_BEHIND;
  ab_nes_frame_read(&reg,&f);
  ab_batch_reset(b);
  q = ab_nes_emit_sprites(b,&f,&view,0x18,NULL);
  CHECK(q==0, "behind-BG sprite hidden by opaque background");
  /* ...but drawn where the background pixel is flagged transparent */
  for(int x=10;x<18;x++) g_regions[R_BGVAL][100*256+x]=0x0F|0x40;
  ab_nes_frame_read(&reg,&f);
  ab_batch_reset(b);
  q = ab_nes_emit_sprites(b,&f,&view,0x18,NULL);
  CHECK(q==1, "behind-BG sprite drawn over transparent background");

  /* --- bgpix opacity path (the RoadBlasters / Section Z rule) ------------
   * With nes_bgpix present it is the SOLE opacity source: pattern entry 0 =
   * transparent. Critically, it must override bgval's bit 6 -- on a line
   * with BG rendering disabled, bgval reports opaque (bit6 clear) while the
   * hardware background is blank and the behind-sprite IS drawn. */
  static unsigned char bgpixbuf[256*240];
  f.bgpix = bgpixbuf;
  ab_nes_regions regp = reg; regp.bgpix = R_BGPIX;
  /* bgpix says opaque (entry 1) even though bgval bit6 says transparent */
  for(int x=10;x<18;x++){ g_regions[R_BGPIX][100*256+x]=0x01;
                          g_regions[R_BGVAL][100*256+x]=0x0F|0x40; }
  ab_nes_frame_read(&regp,&f);
  CHECK(f.have_bgpix==1, "bgpix present");
  ab_batch_reset(b);
  q = ab_nes_emit_sprites(b,&f,&view,0x18,NULL);
  CHECK(q==0, "bgpix opaque hides behind-BG sprite (overrides bgval bit6)");
  /* the discriminating case: bgval says OPAQUE (bit6 clear -- BG-disabled
   * line) but bgpix says transparent -> the sprite the hardware draws. */
  for(int x=10;x<18;x++){ g_regions[R_BGPIX][100*256+x]=0x00;
                          g_regions[R_BGVAL][100*256+x]=0x0F; }
  ab_nes_frame_read(&regp,&f);
  ab_batch_reset(b);
  q = ab_nes_emit_sprites(b,&f,&view,0x18,NULL);
  CHECK(q==1, "bgpix transparent draws behind-BG sprite bgval would hide");
  /* restore the no-bgpix frame state for the tests below */
  f.bgpix = NULL; f.have_bgpix = 0;
  for(int x=10;x<18;x++) g_regions[R_BGVAL][100*256+x]=0x0F|0x40;
  ab_nes_frame_read(&reg,&f);

  /* suppression: the substitution seam */
  memset(supp,0,sizeof(supp));
  for(int x=10;x<18;x++) supp[100*256+x]=1;
  ab_batch_reset(b);
  q = ab_nes_emit_sprites(b,&f,&view,0x18,supp);
  CHECK(q==0, "suppressed sprite pixels are not drawn");

  /* OAM marking + anchor bounds */
  ab_registry *rg = ab_registry_new();
  ab_sub_rule rule; memset(&rule,0,sizeof(rule));
  rule.tiles[0]=0x53; rule.tiles[1]=0xFE; rule.tile_count=2;
  rule.anchor_exclude[0]=0xFE; rule.exclude_count=1;
  ab_registry_add_sprite(rg,&rule);

  memset(g_regions[R_OAM],0xF0,256);              /* park everything */
  g_regions[R_OAM][0]=99;  g_regions[R_OAM][1]=0x53; g_regions[R_OAM][3]=50;  /* body */
  g_regions[R_OAM][4]=120; g_regions[R_OAM][5]=0xFE; g_regions[R_OAM][7]=50;  /* shadow */
  ab_nes_frame_read(&reg,&f);

  memset(supp,0,sizeof(supp));
  ab_nes_bounds bnd; const ab_sub_rule *got=NULL;
  int marked = ab_nes_mark_sprites(&f,rg,8,supp,&got,&bnd);
  CHECK(marked==2, "both body and shadow sprites marked");
  CHECK(supp[100*256+50]==1, "body pixels suppressed");
  CHECK(supp[121*256+50]==1, "shadow pixels suppressed too");
  CHECK(bnd.y0==100 && bnd.y1==108, "bounds from BODY only, shadow excluded");
  CHECK(got!=NULL, "rule reported");

  /* worst case: alternating pixels cannot coalesce -- prove the quad count
   * tracks reality rather than being a constant. */
  memset(g_regions[R_BGVAL], 0x0F, 256*240);   /* clear leftovers from the
       behind-BG assertions above, which legitimately split their own row */
  for(int x=0;x<256;x++) g_regions[R_BGVAL][50*256+x] = (unsigned char)(x&1?0x0F:0x20);
  ab_nes_frame_read(&reg,&f);
  ab_batch_reset(b);
  q = ab_nes_emit_background(b,&f,&view,0x18,NULL);
  CHECK(q == (AB_NES_H-1) + 256, "223 flat rows + one 256-quad alternating row");
  printf("  [info] flat frame=%d quads, one alternating row=%d quads (57344 px)\n", AB_NES_H, q);

  ab_batch_free(b); ab_registry_free(rg);
  printf(fails? "\n%d FAILURES\n" : "\nall NES profile tests passed\n", fails);
  return fails?1:0;
}
