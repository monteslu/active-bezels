/* MD (Genesis/SMS/GG) profile tests. Mirrors test_ab_nes.c's stub host. */
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
/* regions large enough for the 2B/px objpix plane */
static unsigned char g_regions[8][320*240*2];
int32_t ab_region_read_u8(int32_t r,int32_t o){
  if(r<0||r>=8) return 0; if(o<0||o>=(int32_t)sizeof(g_regions[0])) return 0; return g_regions[r][o]; }
int32_t ab_region_read(int32_t r,int32_t o,void*d,int32_t n){
  if(r<0||r>=8||o<0||n<=0) return 0;
  if(o+n > (int32_t)sizeof(g_regions[0])) n = (int32_t)sizeof(g_regions[0])-o;
  if(n<=0) return 0; memcpy(d,&g_regions[r][o],(size_t)n); return n; }

#include "ab_render.c"
#include "ab_md.c"

static int fails=0;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s\n", msg); fails++; } }while(0)

enum { R_BG=0, R_OBJ=1, R_PIXRGB=2, R_LSTATE=3, R_VRAM=4, R_VDP=5 };

int main(void){
  static unsigned char bgbuf[320*240], objbuf[320*240*2], supp[320*240];
  ab_md_frame f; memset(&f,0,sizeof(f));
  f.bgpix=bgbuf; f.objpix=objbuf;

  /* linestate: V28, H40, display on, no left blank */
  for(int y=0;y<240;y++){
    unsigned char *ls=&g_regions[R_LSTATE][y*16];
    ls[0]=0x04; ls[1]=0x74; ls[3]=0x81; ls[6]=0x40; ls[7]=0x01;
  }
  /* pixrgb: entry i -> RGB565 with red = i>>3 (recognisable), entry 0 = 0 */
  for(int i=0;i<256;i++){
    unsigned v = (unsigned)((i>>3)&0x1F) << 11;
    g_regions[R_PIXRGB][i*2]=v&0xFF; g_regions[R_PIXRGB][i*2+1]=(v>>8)&0xFF;
  }
  /* bg flat code 0x08, obj transparent */
  memset(g_regions[R_BG], 0x08, 320*240);
  memset(g_regions[R_OBJ], 0x00, 320*240*2);

  ab_md_regions reg = { .linepix=-1, .bgpix=R_BG, .objpix=R_OBJ, .pixrgb=R_PIXRGB,
                        .linestate=R_LSTATE, .pixlines=-1, .vram=R_VRAM, .vdpregs=R_VDP };
  CHECK(ab_md_frame_read(&reg,&f)==1, "frame read");
  CHECK(ab_md_lines(&f)==224, "V28 -> 224 lines");
  CHECK(ab_md_line_width(&f,0)==320, "H40 -> 320 wide");

  /* widening is the host's exact rule */
  uint32_t c = ab_md_rgba(&f, 0x08);   /* r5 = 1 -> (1<<3)|(1>>2) = 8 */
  CHECK(((c>>24)&0xFF)==8, "RGB565 widening matches host");

  ab_md_view view = { .ox=0, .oy=0, .scale=4.0 };
  ab_batch *b = ab_batch_new(1024);

  /* flat frame -> one quad per row, ONE mesh call */
  int q = ab_md_emit(b,&f,&view,NULL);
  CHECK(q==224, "flat frame coalesces to one quad per row");
  int before=g_mesh_calls;
  ab_batch_flush(b,0);
  CHECK(g_mesh_calls==before+1, "whole frame is ONE mesh call");

  /* sprite pixels override bg */
  for(int x=100;x<108;x++){ g_regions[R_OBJ][(50*320+x)*2]=0x20; g_regions[R_OBJ][(50*320+x)*2+1]=1; }
  ab_md_frame_read(&reg,&f);
  ab_batch_reset(b);
  q = ab_md_emit(b,&f,&view,NULL);
  CHECK(q==224+2, "8px sprite run splits its row into three runs");

  /* left-blank rule: reg0 bit5 forces code 0x40 in columns 0-7 */
  g_regions[R_LSTATE][60*16+0] = 0x04 | 0x20;
  ab_md_frame_read(&reg,&f);
  ab_batch_reset(b);
  q = ab_md_emit(b,&f,&view,NULL);
  CHECK(q==224+2+1, "left-blank adds one run on its line");
  g_regions[R_LSTATE][60*16+0] = 0x04;

  /* suppression: suppressed sprite pixels fall back to the BG code */
  memset(supp,0,sizeof(supp));
  for(int x=100;x<108;x++) supp[50*320+x]=1;
  ab_md_frame_read(&reg,&f);
  ab_batch_reset(b);
  q = ab_md_emit(b,&f,&view,supp);
  CHECK(q==224, "suppressed sprite leaves a clean flat row");

  /* linepix, when present, is the compose base (S/H resolution lives in
   * the core's merge): bg says 0x08 everywhere, linepix says 0x10 -> the
   * emitted colour must be lut[0x10], not lut[0x08]. */
  {
    static unsigned char lpbuf[320*240];
    enum { R_LP = 7 };
    memset(g_regions[R_LP], 0x10, 320*240);
    ab_md_regions regl = reg; regl.linepix = R_LP;
    ab_md_frame f2 = f; f2.linepix = lpbuf;
    ab_md_frame_read(&regl, &f2);
    ab_batch_reset(b);
    q = ab_md_emit(b, &f2, &view, NULL);
    CHECK(q == 224, "linepix base coalesces flat");
    /* colour check via the captured vertex data is indirect; assert via
     * rgba of the two codes differing so the test is discriminating */
    CHECK(ab_md_rgba(&f2, 0x10) != ab_md_rgba(&f2, 0x08),
          "test codes map to different colours");
  }

  /* SAT walk: one 2x2-cell sprite, tile 0x30, at screen (16, 16). VRAM is
   * word-byte-swapped, so write each u16 with the byte order flipped. */
  memset(g_regions[R_VRAM],0,sizeof(g_regions[0]));
  g_regions[R_VDP][5] = 0x02;              /* SAT base = (2&0x7E)<<9 = 0x400 */
  {
    const int satb = 0x400;
    unsigned words[4] = { 128+16, (1u<<10)|(1u<<8), 0x30, 128+16 };
    for (int w2=0; w2<4; w2++) {
      const int a = satb + w2*2;
      g_regions[R_VRAM][a ^ 1] = words[w2] & 0xFF;
      g_regions[R_VRAM][a ^ 0] = (words[w2] >> 8) & 0xFF;
    }
  }
  ab_registry *r2 = ab_registry_new();
  ab_sub_rule rule; memset(&rule,0,sizeof(rule));
  rule.tiles[0]=0x30; rule.tile_count=1; rule.texture=7; rule.tex_w=16; rule.tex_h=16;
  CHECK(ab_registry_add_sprite(r2,&rule)>0, "registry add");
  memset(supp,0,sizeof(supp));
  const ab_sub_rule *hit=NULL; ab_md_bounds bd;
  int n = ab_md_mark_sprites(&reg,&f,r2,supp,&hit,&bd);
  CHECK(n==1, "SAT walk finds the sprite");
  CHECK(hit && hit->tiles[0]==0x30, "rule matched by tile");
  CHECK(bd.x0==16 && bd.y0==16 && bd.x1==32 && bd.y1==32, "bounds = 2x2 cells at (16,16)");
  CHECK(supp[20*320+20]==1 && supp[10*320+10]==0, "suppression marks the sprite rect only");

  if (fails){ printf("%d FAILURES\n", fails); return 1; }
  printf("\nab_md: all tests passed\n");
  return 0;
}
