/* Host-stub test: proves atlas expansion, quad UVs and the registry are
 * correct without needing a wasm host. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define AB_IMPORT(m,n)
typedef struct { float x,y,u,v; uint32_t rgba,_pad; } ab_vertex;
static ab_vertex g_last[16384]; static int g_last_n, g_mesh_calls; static int32_t g_last_tex;
static uint32_t g_tex_px[65536]; static int g_tex_w, g_tex_h, g_next_tex = 1;
int32_t ab_mesh(const ab_vertex *v, int32_t n, int32_t tex){
  memcpy(g_last, v, (size_t)n*sizeof(ab_vertex)); g_last_n=n; g_last_tex=tex; g_mesh_calls++; return 1; }
int32_t ab_texture_create_rgba(const void *px, int32_t w, int32_t h){
  g_tex_w=w; g_tex_h=h; memcpy(g_tex_px, px, (size_t)w*h*4 > sizeof(g_tex_px) ? sizeof(g_tex_px) : (size_t)w*h*4);
  return g_next_tex++; }
int32_t ab_texture_destroy(int32_t t){ (void)t; return 1; }
static unsigned char g_region[4096];
int32_t ab_region_read_u8(int32_t r, int32_t o){ (void)r; return g_region[o & 4095]; }
/* Stubbed to 0 on purpose: exercises the FALLBACK path, so the per-byte
 * loop stays correct on hosts without the bulk import. */
int32_t ab_region_read(int32_t r, int32_t o, void *d, int32_t n){ (void)r;(void)o;(void)d;(void)n; return 0; }

#include "ab_render.c"

static int fails = 0;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s\n", msg); fails++; } }while(0)

int main(void){
  /* one 8x8 tile: solid colour index 1 on the top row, index 0 elsewhere */
  unsigned char chr[32] = {0};
  chr[0] = 0xFF;             /* plane0 row0 = all 1s -> colour 1 */
  uint32_t pal[8] = { 0xDEADBEEFu, 0xFF0000FFu, 0x00FF00FFu, 0x0000FFFFu,
                      0xDEADBEEFu, 0x112233FFu, 0u, 0u };

  ab_atlas atlas; memset(&atlas,0,sizeof(atlas));
  int rc = ab_atlas_build_2bpp(&atlas, chr, 2, pal, 2, 0x1234);
  CHECK(rc==1, "atlas built");
  CHECK(atlas.texture!=0, "atlas has texture");
  /* colour 0 must be TRANSPARENT, colour 1 must be palette entry 1 */
  CHECK(g_tex_px[0]==0xFF0000FFu, "tile px row0 = colour 1");
  CHECK(g_tex_px[(size_t)g_tex_w*1]==0u, "colour 0 is transparent (pal[0] is DEADBEEF: a painted backdrop would show)");
  /* palette 1 block: second palette applied */
  int pal1_y = atlas.rows*8;
  CHECK(g_tex_px[(size_t)g_tex_w*pal1_y]==0x112233FFu, "palette 1 applied");

  /* signature match = no rebuild */
  rc = ab_atlas_build_2bpp(&atlas, chr, 2, pal, 2, 0x1234);
  CHECK(rc==0, "signature match skips rebuild");
  rc = ab_atlas_build_2bpp(&atlas, chr, 2, pal, 2, 0x9999);
  CHECK(rc==1, "changed signature rebuilds");

  /* batch: one cell -> 6 verts, one mesh call */
  ab_batch *b = ab_batch_new(8);
  ab_batch_atlas_cell(b, &atlas, 0, 0, 100, 200, 4.0, 0, 0xFFFFFFFFu);
  int quads = ab_batch_flush(b, atlas.texture);
  CHECK(quads==1, "one quad");
  CHECK(g_last_n==6, "six vertices");
  CHECK(g_last_tex==atlas.texture, "mesh got atlas texture");
  CHECK(g_last[0].x==100.0f && g_last[0].y==200.0f, "quad origin");
  CHECK(g_last[2].x==132.0f, "quad scaled 8*4=32 wide");

  /* hflip swaps u */
  ab_batch_reset(b);
  ab_batch_atlas_cell(b, &atlas, 0, 0, 0, 0, 1.0, 1, 0xFFFFFFFFu);
  ab_batch_flush(b, atlas.texture);
  CHECK(g_last[0].u > g_last[1].u, "hflip reverses u");

  /* many quads in ONE mesh call: that is the whole point */
  ab_batch_reset(b);
  int before = g_mesh_calls;
  for (int i=0;i<1000;i++) ab_batch_atlas_cell(b,&atlas,0,0,i,0,1.0,0,0xFFFFFFFFu);
  quads = ab_batch_flush(b, atlas.texture);
  CHECK(quads==1000, "1000 quads batched");
  CHECK(g_mesh_calls==before+1, "1000 quads = ONE mesh call");

  /* registry */
  ab_registry *reg = ab_registry_new();
  ab_sub_rule rule; memset(&rule,0,sizeof(rule));
  rule.tiles[0]=0x53; rule.tiles[1]=0x54; rule.tiles[2]=0xFE; rule.tile_count=3;
  rule.anchor_exclude[0]=0xFE; rule.exclude_count=1;
  rule.texture=42; rule.base_w=15; rule.base_h=16; rule.ring=4;
  int id = ab_registry_add_sprite(reg,&rule);
  CHECK(id>0, "rule registered");
  CHECK(ab_registry_match_tile(reg,0x53)!=NULL, "0x53 suppressed");
  CHECK(ab_registry_match_tile(reg,0xFE)!=NULL, "shadow tile suppressed too");
  CHECK(ab_registry_match_tile(reg,0x99)==NULL, "unrelated tile not matched");
  const ab_sub_rule *got = ab_registry_match_tile(reg,0x53);
  CHECK(ab_registry_tile_anchors(got,0x53)==1, "body tile anchors");
  CHECK(ab_registry_tile_anchors(got,0xFE)==0, "shadow tile does NOT anchor");
  CHECK(ab_registry_remove(reg,id)==1, "rule removed");
  CHECK(ab_registry_match_tile(reg,0x53)==NULL, "removed rule stops matching");

  /* target compose */
  ab_target tgt; memset(&tgt,0,sizeof(tgt));
  int place[8] = { 0,0, 0,0,  0,1, 8,0 };
  rc = ab_target_build(&tgt, &atlas, chr, pal, place, 2, 16, 8);
  CHECK(rc==1, "target built");
  CHECK(g_tex_w==16 && g_tex_h==8, "target dims");
  CHECK(g_tex_px[0]==0xFF0000FFu, "target tile0 colour");
  CHECK(g_tex_px[8]==0x112233FFu, "target tile1 palette1 colour");

  /* region slurp */
  for(int i=0;i<256;i++) g_region[i]=(unsigned char)i;
  unsigned char buf[256];
  CHECK(ab_region_slurp(0,0,buf,256)==256, "slurp len");
  CHECK(buf[0]==0 && buf[255]==255, "slurp contents");

  ab_batch_free(b); ab_registry_free(reg);
  printf(fails? "\n%d FAILURES\n" : "\nall render-kit tests passed\n", fails);
  return fails?1:0;
}
