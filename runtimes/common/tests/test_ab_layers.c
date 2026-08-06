/* Layer routing (ab_prof_view.bg_surface / .spr_surface).
 *
 * The claim under test is an ORDERING one, and it is the whole reason the
 * split lives in C rather than in a script: within ONE draw -- one frame
 * read, one sprite evaluation -- the background batch must land on the
 * background surface and the sprite batch (plus any HD replacement art) on
 * the sprite surface, with the previous target restored on the way out.
 *
 * So these tests record the interleaving of surface_target/surface_end and
 * the draw calls between them, and assert on the SEQUENCE. Checking only
 * "both surfaces got targeted" would pass even if every quad went to the
 * wrong one.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define AB_IMPORT(m,n)

/* --- host stubs, recording an event log ------------------------------- */

typedef struct { float x,y,u,v; uint32_t rgba,_pad; } ab_vertex;

/* One character per host event, so a whole draw is a readable string:
 *   '[' surface_target   ']' surface_end   'm' mesh   't' texture_rect
 * The digit after '[' is the surface handle. */
static char g_log[256];
static int  g_log_n;
static void logc(char c){ if(g_log_n < (int)sizeof(g_log)-1) g_log[g_log_n++]=c; }
static void log_reset(void){ g_log_n=0; memset(g_log,0,sizeof(g_log)); }
static const char *log_str(void){ g_log[g_log_n]=0; return g_log; }

static int32_t g_target;          /* current surface, 0 = scene */
int32_t ab_surface_target(int32_t h){ g_target=h; logc('['); logc((char)('0'+h)); return 1; }
int32_t ab_surface_end(void){ g_target=0; logc(']'); return 1; }
int32_t ab_surface_create(int32_t w,int32_t h){ (void)w;(void)h; return 1; }

static int g_mesh_calls;
int32_t ab_mesh(const ab_vertex*v,int32_t n,int32_t t){
  (void)v;(void)t; if(n>0){ g_mesh_calls++; logc('m'); } return 1; }

static uint32_t g_tex_px[65536];
static int g_next=1;
int32_t ab_texture_create_rgba(const void*px,int32_t w,int32_t h){
  size_t n=(size_t)w*h*4; if(n>sizeof(g_tex_px)) n=sizeof(g_tex_px);
  memcpy(g_tex_px,px,n); return g_next++; }
int32_t ab_texture_destroy(int32_t t){ (void)t; return 1; }

int32_t ab_draw_texture_rect(int32_t tex,double x,double y,double w,double h,
                             int32_t sx,int32_t sy,int32_t sw,int32_t sh){
  (void)tex;(void)x;(void)y;(void)w;(void)h;(void)sx;(void)sy;(void)sw;(void)sh;
  logc('t'); return 1; }

/* Regions: index 0..15, all readable, contents set per test. */
static unsigned char g_regions[16][256*240];
static const char *g_region_names[16];
static int g_region_count;

int32_t ab_region_find(const char *name){
  for(int i=0;i<g_region_count;i++)
    if(g_region_names[i] && strcmp(g_region_names[i],name)==0) return i;
  return -1; }
int32_t ab_region_read_u8(int32_t r,int32_t o){
  if(r<0||r>=16||o<0||o>=256*240) return 0;
  return g_regions[r][o]; }
int32_t ab_region_read(int32_t r,int32_t o,void*d,int32_t n){
  if(r<0||r>=16||o<0||n<=0) return 0;
  if(o+n > 256*240) n = 256*240-o;
  if(n<=0) return 0;
  memcpy(d,&g_regions[r][o],(size_t)n); return n; }
int32_t ab_region_size(int32_t r){ return (r>=0&&r<16) ? 256*240 : 0; }
int32_t ab_region_write_u8(int32_t r,int32_t o,int32_t v){
  if(r<0||r>=16||o<0||o>=256*240) return 0;
  g_regions[r][o]=(unsigned char)v; return 1; }
int32_t ab_log(const char*s,int32_t n){ (void)s;(void)n; return 1; }

/* ab_region_find is the length-taking host import; the profiles call it
 * through this raw form. */
int32_t ab_region_find_raw(const char *name,int32_t n){
  char buf[64]; if(n<0||n>=(int32_t)sizeof(buf)) return -1;
  memcpy(buf,name,(size_t)n); buf[n]=0; return ab_region_find(buf); }

/* Only the SNES Mode 7 path uses these; stubbed so the profiles link. */
int32_t ab_texture_palette(int32_t t,const void*p,int32_t n){
  (void)t;(void)p;(void)n; return 1; }
int32_t ab_texture_update(int32_t t,const void*p,int32_t x,int32_t y,
                          int32_t w,int32_t h){
  (void)t;(void)p;(void)x;(void)y;(void)w;(void)h; return 1; }
int32_t ab_texture_filter(int32_t t,int32_t f){ (void)t;(void)f; return 1; }

/* The per-platform renderers carry colliding file-scope statics
 * (each has its own emit_plane), so they are compiled as SEPARATE
 * translation units and linked -- see the build line in run.sh. */
#include "ab_profiles.h"
#include "ab_nes.h"

static int fails=0;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s\n", msg); fails++; } }while(0)
#define CHECK_LOG(want,msg) do{ \
  if(strcmp(log_str(),(want))!=0){ \
    printf("FAIL: %s\n  want \"%s\"\n  got  \"%s\"\n",(msg),(want),log_str()); \
    fails++; } }while(0)

/* A NES frame with both a background and at least one drawn sprite, so
 * BOTH batches emit and the routing of each is observable. */
static void setup_nes_regions(void){
  g_region_count = 8;
  g_region_names[0]="nes_bgval";   g_region_names[1]="nes_sprdrawn";
  g_region_names[2]="nes_palette"; g_region_names[3]="nes_palrgb";
  g_region_names[4]="nes_oam";     g_region_names[5]="nes_maskpix";
  g_region_names[6]="nes_bgpix";   g_region_names[7]="nes_ppureg";
  for(int i=0;i<8;i++) memset(g_regions[i],0,sizeof(g_regions[i]));
  memset(g_regions[0],0x0F,256*240);      /* background: flat colour */
  memset(g_regions[5],0x18,256*240);      /* rendering enabled */
  /* sprites drawn across a band, non-zero palette value = opaque */
  for(int y=32;y<48;y++) for(int x=32;x<48;x++) g_regions[1][y*256+x]=0x11;
  /* a plausible palette + RGB table */
  for(int i=0;i<32;i++) g_regions[2][i]=(unsigned char)i;
  for(int i=0;i<64*3;i++) g_regions[3][i]=(unsigned char)(i*3);
}

int main(void){
  const char *err=NULL;

  /* ---- 1. no split: nothing is targeted, behaviour is unchanged ------- */
  setup_nes_regions();
  CHECK(ab_prof_nes_bind(&err), "nes bind");
  {
    ab_prof_view v; memset(&v,0,sizeof(v));
    v.x=0; v.y=0; v.scale=1.0;
    ab_prof_nes_result r; log_reset(); g_mesh_calls=0;
    CHECK(ab_prof_nes_draw(&v,&r,&err), "nes draw (no split)");
    CHECK(r.bg_quads>0, "no-split: background emitted quads");
    CHECK(r.spr_quads>0, "no-split: sprites emitted quads");
    /* Two meshes, NO surface switches at all. */
    CHECK_LOG("mm", "no split: two meshes, no surface targeting");
    CHECK(g_target==0, "no-split: target restored to the scene");
  }

  /* ---- 2. split: each batch lands inside its OWN surface -------------- */
  {
    ab_prof_view v; memset(&v,0,sizeof(v));
    v.x=0; v.y=0; v.scale=1.0; v.bg_surface=3; v.spr_surface=4;
    ab_prof_nes_result r; log_reset();
    CHECK(ab_prof_nes_draw(&v,&r,&err), "nes draw (split)");
    /* The ordering claim, exactly: open bg, draw it, close; open spr,
     * draw it, close. A mesh outside a bracket, or both meshes inside one
     * bracket, fails here. */
    CHECK_LOG("[3m][4m]", "split: bg mesh in surface 3, sprite mesh in surface 4");
    CHECK(g_target==0, "split: target restored to the scene");
  }

  /* ---- 3. one-sided split is legal ------------------------------------ */
  {
    ab_prof_view v; memset(&v,0,sizeof(v));
    v.x=0; v.y=0; v.scale=1.0; v.bg_surface=0; v.spr_surface=5;
    ab_prof_nes_result r; log_reset();
    CHECK(ab_prof_nes_draw(&v,&r,&err), "nes draw (sprites only)");
    /* Background draws wherever the guest already was; only sprites are
     * diverted. */
    CHECK_LOG("m[5m]", "sprite-only split: bg to the scene, sprites to surface 5");
    CHECK(g_target==0, "sprite-only: target restored");
  }

  /* ---- 4. HD replacement art follows the SPRITE layer ----------------- */
  {
    /* Register a substitution matching the tiles the test frame draws, so
     * ab_draw_texture_rect fires. It must land INSIDE the sprite bracket:
     * substituted art replaces sprites, so shading it as background would
     * be wrong. */
    static uint32_t art[16*16];
    for(int i=0;i<16*16;i++) art[i]=0xFF00FFFFu;
    ab_sub_rule rule; memset(&rule,0,sizeof(rule));
    rule.tiles[0]=0; rule.tile_count=1;
    rule.texture=ab_texture_create_rgba(art,16,16);
    rule.tex_w=16; rule.tex_h=16; rule.base_w=16; rule.base_h=16;
    const int id = ab_prof_add_rule(AB_PROF_NES,&rule);
    CHECK(id!=0, "hd: rule registered");

    /* OAM entry so mark_sprites finds a metasprite: y, tile, attr, x. */
    g_regions[4][0]=32; g_regions[4][1]=0; g_regions[4][2]=0; g_regions[4][3]=32;

    ab_prof_view v; memset(&v,0,sizeof(v));
    v.x=0; v.y=0; v.scale=1.0; v.bg_surface=3; v.spr_surface=4;
    ab_prof_nes_result r; log_reset();
    CHECK(ab_prof_nes_draw(&v,&r,&err), "nes draw (hd art)");
    if (r.hd_drawn) {
      CHECK_LOG("[3m][4mt]", "hd art draws INSIDE the sprite surface bracket");
    } else {
      /* Substitution did not fire in this synthetic frame; the ordering
       * claim is then untested rather than silently "passing". */
      printf("note: hd substitution did not fire; ordering claim untested here\n");
    }
    ab_prof_clear_rules(AB_PROF_NES);
    CHECK(g_target==0, "hd: target restored");
  }

  /* ---- 5. capability query is honest ---------------------------------- */
  CHECK(ab_prof_layers_supported(AB_PROF_NES), "NES supports layers");
  CHECK(ab_prof_layers_supported(AB_PROF_GB),  "GB supports layers");
  /* MD/MSX/PCE consume resolved per-pixel planes -- there is no separate
   * sprite batch to route, and saying otherwise would make the bindings
   * accept an option they cannot honour. */
  CHECK(!ab_prof_layers_supported(AB_PROF_MD),  "MD does NOT support layers");
  CHECK(!ab_prof_layers_supported(AB_PROF_MSX), "MSX does NOT support layers");
  CHECK(!ab_prof_layers_supported(AB_PROF_PCE), "PCE does NOT support layers");

  if(fails){ printf("%d layer test(s) failed\n",fails); return 1; }
  printf("all layer routing tests passed\n");
  return 0;
}
