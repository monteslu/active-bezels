/*
 * ab_profiles.c -- language-neutral cores for the platform redraw profiles.
 *
 * This is the logic that used to live in the Lua bindings (ab_nes_lua.c and
 * siblings), moved wholesale when the Python/JS/Ruby runtimes became the
 * second, third and fourth consumers. The rendering itself lives in
 * ab_nes.c / ab_gb.c / ab_md.c / ab_snes.c / ab_msx.c / ab_pce.c; this file
 * owns per-profile state (regions, buffers, batch, registry, replacement
 * art) and the draw orchestration, so every runtime gets the exact same
 * frame for the exact same call.
 *
 * The API is DECLARATIVE on purpose: scripts register what to substitute
 * and then ask for a frame to be drawn. There is no per-sprite callback
 * into the scripting language, because that would reintroduce the FFI cost
 * the C renderers exist to remove (measured on NES: 57k per-pixel host
 * calls were ~24ms of a 16.7ms frame).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../sdk/active_bezel.h"

#include "ab_profiles.h"
#include "ab_nes.h"
#include "ab_gb.h"
#include "ab_md.h"
#include "ab_msx.h"
#include "ab_pce.h"
#include "ab_snes.h"

/* ------------------------------------------------------------- shared -- */

static int32_t region_or(const char *name, int32_t fallback) {
  int32_t r = ab_region_find(name);
  return r >= 0 ? r : fallback;
}

static int32_t region_any(const char *a, const char *b, const char *c) {
  int32_t r = ab_region_find(a);
  if (r < 0) r = ab_region_find(b);
  if (r < 0) r = ab_region_find(c);
  return r;
}

/* --- layer routing -------------------------------------------------------
 *
 * A redraw emits its layers as separate batches; ab_prof_view's bg_surface
 * and spr_surface route them to separate offscreen surfaces so a bezel can
 * shade each differently (see the header for why this is not a script-side
 * concern). Handle 0 means "leave the target alone", which is the default
 * and reproduces the old single-destination behaviour exactly.
 *
 * layer_end() must run on EVERY path out of a draw, including the error
 * ones: leaving the guest's subsequent draws pointed at an offscreen
 * surface blanks the visible scene, and the guest has no way to tell that
 * happened. */
static int layer_begin(int32_t surface) {
  if (!surface) return 0;
  ab_surface_target(surface);
  return 1;
}

static void layer_end(int active) {
  if (active) ab_surface_end();
}

/* Substitution bookkeeping, identical across the five sprite profiles:
 * a kit registry plus the replacement art table parallel to rule ids. */
typedef struct {
  ab_registry *registry;
  struct { int id; int32_t texture; int w, h; } art[64];
  int art_count;
} ab_prof_subst;

/* The five sprite profiles all begin with this prefix, so the shared rule
 * management can address any of them through it. C guarantees a pointer to
 * a struct points at its first member. */
typedef struct { int bound; ab_prof_subst sub; } ab_prof_common;

static ab_prof_common *prof_state(ab_prof_id p);   /* defined after the Gs */

int ab_prof_add_rule(ab_prof_id p, const ab_sub_rule *rule) {
  ab_prof_subst *s = &prof_state(p)->sub;
  const int id = ab_registry_add_sprite(s->registry, rule);
  if (!id) return 0;
  if (s->art_count < (int)(sizeof(s->art) / sizeof(s->art[0]))) {
    s->art[s->art_count].id = id;
    s->art[s->art_count].texture = rule->texture;
    s->art[s->art_count].w = rule->tex_w;
    s->art[s->art_count].h = rule->tex_h;
    s->art_count++;
  }
  return id;
}

int ab_prof_remove_rule(ab_prof_id p, int id) {
  ab_prof_subst *s = &prof_state(p)->sub;
  const int removed = ab_registry_remove(s->registry, id);
  for (int i = 0; i < s->art_count; i++) {
    if (s->art[i].id == id) {
      for (int j = i; j < s->art_count - 1; j++) s->art[j] = s->art[j + 1];
      s->art_count--;
      break;
    }
  }
  return removed;
}

int ab_prof_layers_supported(ab_prof_id p) {
  /* See the header. NES and GB emit background and sprites as separate
   * batches; MD/MSX/PCE resolve their layers per pixel before we ever see
   * them, so there is nothing to route apart. */
  return p == AB_PROF_NES || p == AB_PROF_GB;
}

void ab_prof_clear_rules(ab_prof_id p) {
  ab_prof_subst *s = &prof_state(p)->sub;
  ab_registry_clear(s->registry);
  s->art_count = 0;
}

int ab_prof_bound(ab_prof_id p) { return prof_state(p)->bound; }

static void subst_find_art(const ab_prof_subst *s, int rule_id,
                           int32_t *tex, int *tw, int *th) {
  *tex = 0; *tw = 0; *th = 0;
  for (int i = 0; i < s->art_count; i++)
    if (s->art[i].id == rule_id) {
      *tex = s->art[i].texture; *tw = s->art[i].w; *th = s->art[i].h;
      return;
    }
}

static void subst_free(ab_prof_subst *s) {
  if (s->registry) { ab_registry_free(s->registry); s->registry = NULL; }
  s->art_count = 0;
}

/* ---------------------------------------------------------------- NES -- */

static struct {
  int bound;
  ab_prof_subst sub;
  ab_nes_regions regions;
  ab_nes_frame   frame;
  unsigned char *bgval;
  unsigned char *bgpix;
  unsigned char *sprdrawn;
  unsigned char *suppress;
  ab_batch      *batch;
} G_nes;

int ab_prof_nes_bind(const char **err) {
  if (G_nes.bound) return 1;

  G_nes.regions.chr       = region_or("nes_chr", -1);
  G_nes.regions.palette   = region_or("nes_palette", -1);
  G_nes.regions.palrgb    = region_or("nes_palrgb", -1);
  G_nes.regions.oam       = region_or("nes_oam", -1);
  G_nes.regions.ppureg    = region_or("nes_ppu_regs", -1);
  G_nes.regions.masklines = region_or("nes_masklines", -1);
  G_nes.regions.bgval     = region_or("nes_bgval", -1);
  G_nes.regions.bgpix     = region_or("nes_bgpix", -1);
  G_nes.regions.sprdrawn  = region_or("nes_sprdrawn", -1);

  if (G_nes.regions.bgval < 0 || G_nes.regions.sprdrawn < 0) {
    *err = "nes: requires nes_bgval and nes_sprdrawn "
           "(the resolved layers); is the core patched?";
    return 0;
  }

  const size_t plane = (size_t)AB_NES_W * AB_NES_LINES;
  G_nes.bgval    = (unsigned char *)malloc(plane);
  G_nes.sprdrawn = (unsigned char *)malloc(plane);
  G_nes.suppress = (unsigned char *)malloc(plane);
  /* bgpix drives behind-BG sprite opacity; optional (older cores lack it). */
  G_nes.bgpix    = (G_nes.regions.bgpix >= 0) ? (unsigned char *)malloc(plane) : NULL;
  G_nes.batch    = ab_batch_new(4096);
  G_nes.sub.registry = ab_registry_new();
  if (!G_nes.bgval || !G_nes.sprdrawn || !G_nes.suppress || !G_nes.batch
      || !G_nes.sub.registry
      || (G_nes.regions.bgpix >= 0 && !G_nes.bgpix)) {
    *err = "nes: out of memory";
    return 0;
  }
  G_nes.frame.bgval    = G_nes.bgval;
  G_nes.frame.bgpix    = G_nes.bgpix;
  G_nes.frame.sprdrawn = G_nes.sprdrawn;
  G_nes.bound = 1;
  return 1;
}

static int nes_sprite_height(void) {
  /* Sprite height comes from PPUCTRL bit5 -- 8x16 mode is common and getting
   * it wrong suppresses only the top half of every replaced sprite. */
  if (G_nes.regions.ppureg >= 0
      && (ab_region_read_u8(G_nes.regions.ppureg, 0) & 0x20))
    return 16;
  return 8;
}

/* Per-pixel "do not draw" mask for the BACKGROUND layer, filled by
 * ab_prof_nes_hide_cell. This is how a bezel takes ownership of a class of
 * background tiles: the redraw never emits them at all, so there is nothing
 * to paint over afterwards and the tiles never receive the layer's
 * treatment. The HUD text is the motivating case -- text that has been
 * warped and hue-rotated and then patched back over is a hack; text that
 * was never drawn in the first place is correct. */
static unsigned char G_nes_hide[AB_NES_W * AB_NES_LINES];
static int G_nes_hide_any = 0;

void ab_prof_nes_hide_cell(int cx, int cy) {
  if (cx < 0 || cy < 0) return;
  const int x0 = cx * 8, y0 = cy * 8 + AB_NES_OVERSCAN_TOP;
  if (x0 + 8 > AB_NES_W || y0 + 8 > AB_NES_LINES) return;
  for (int y = y0; y < y0 + 8; y++)
    memset(G_nes_hide + (size_t)y * AB_NES_W + x0, 1, 8);
  G_nes_hide_any = 1;
}

/* Per-slot sprite suppression -- see the header. Marked into the same
 * per-pixel mask the HD-substitution path already uses, so the emit needs
 * no new branch. */
static int G_nes_hide_slot[64];
static int G_nes_hide_slot_any = 0;

static int G_nes_iso_slot[64];
static int G_nes_iso_any = 0;

void ab_prof_nes_isolate_sprite(int slot) {
  if (slot < 0 || slot >= 64) return;
  G_nes_iso_slot[slot] = 1;
  G_nes_iso_any = 1;
}

void ab_prof_nes_hide_sprite(int slot) {
  if (slot < 0 || slot >= 64) return;
  G_nes_hide_slot[slot] = 1;
  G_nes_hide_slot_any = 1;
}

void ab_prof_nes_clear_hidden(void) {
  if (G_nes_hide_any) memset(G_nes_hide, 0, sizeof(G_nes_hide));
  G_nes_hide_any = 0;
  if (G_nes_hide_slot_any) memset(G_nes_hide_slot, 0, sizeof(G_nes_hide_slot));
  G_nes_hide_slot_any = 0;
  if (G_nes_iso_any) memset(G_nes_iso_slot, 0, sizeof(G_nes_iso_slot));
  G_nes_iso_any = 0;
}

int ab_prof_nes_draw(const ab_prof_view *v, ab_prof_nes_result *r,
                     const char **err) {
  ab_nes_view view;
  view.ox = v->x; view.oy = v->y; view.scale = v->scale;

  if (!ab_nes_frame_read(&G_nes.regions, &G_nes.frame)) {
    *err = "nes.draw: frame read failed";
    return 0;
  }

  int frame_mask = 0x18;   /* rendering on; per-line masklines override */
  if (G_nes.regions.ppureg >= 0)
    frame_mask = ab_region_read_u8(G_nes.regions.ppureg, 1);

  const int sprite_h = nes_sprite_height();

  /* Decide substitution BEFORE anything is drawn. */
  const ab_sub_rule *rule = NULL;
  ab_nes_bounds bounds;
  int marked = 0;
  memset(&bounds, 0, sizeof(bounds));
  if (G_nes.sub.art_count > 0) {
    memset(G_nes.suppress, 0, (size_t)AB_NES_W * AB_NES_LINES);
    marked = ab_nes_mark_sprites(&G_nes.frame, G_nes.sub.registry, sprite_h,
                                 G_nes.suppress, &rule, &bounds);
  }

  /* Background, then sprites. Each layer is ONE mesh command, optionally
   * routed to its own surface (v->bg_surface / v->spr_surface). */
  const unsigned char *hide = G_nes_hide_any ? G_nes_hide : NULL;
  int bg_quads;
  int layer;
  if (v->solid_surface) {
    /* Split: backdrop to bg_surface, solid tiles to solid_surface. Two
     * emits over the same decoded frame -- no extra region reads, no second
     * sprite evaluation. */
    layer = layer_begin(v->bg_surface);
    ab_batch_reset(G_nes.batch);
    bg_quads = ab_nes_emit_background_sel(G_nes.batch, &G_nes.frame, &view,
                                          frame_mask, hide, AB_NES_BG_EMPTY);
    ab_batch_flush(G_nes.batch, 0);
    layer_end(layer);

    layer = layer_begin(v->solid_surface);
    ab_batch_reset(G_nes.batch);
    bg_quads += ab_nes_emit_background_sel(G_nes.batch, &G_nes.frame, &view,
                                           frame_mask, hide, AB_NES_BG_SOLID);
    ab_batch_flush(G_nes.batch, 0);
    layer_end(layer);
  } else {
    layer = layer_begin(v->bg_surface);
    ab_batch_reset(G_nes.batch);
    bg_quads = ab_nes_emit_background(G_nes.batch, &G_nes.frame, &view,
                                      frame_mask, hide);
    ab_batch_flush(G_nes.batch, 0);
    layer_end(layer);
  }

  /* Fold slot suppression into the same per-pixel mask the HD substitution
   * path uses. If no substitution ran, the buffer has not been cleared this
   * frame, so clear it here before marking. */
  int use_suppress = marked;
  if (G_nes_iso_any) {
    /* Emit ONLY the isolated slots: suppress every sprite pixel, then clear
     * the mask back over the pixels those slots cover. */
    memset(G_nes.suppress, 1, (size_t)AB_NES_W * AB_NES_LINES);
    const int sh = nes_sprite_height();
    for (int slot = 0; slot < 64; slot++) {
      if (!G_nes_iso_slot[slot]) continue;
      const int oy = ab_region_read_u8(G_nes.regions.oam, slot * 4);
      const int ox = ab_region_read_u8(G_nes.regions.oam, slot * 4 + 3);
      if (oy >= 0xEF) continue;
      for (int yy = oy + 1; yy < oy + 1 + sh; yy++) {
        if (yy < 0 || yy >= AB_NES_LINES) continue;
        for (int xx = ox; xx < ox + 8; xx++) {
          if (xx < 0 || xx >= AB_NES_W) continue;
          G_nes.suppress[(size_t)yy * AB_NES_W + xx] = 0;
        }
      }
    }
    use_suppress = 1;
  } else if (G_nes_hide_slot_any) {
    /* Always clear before marking.
     *
     * This used to clear only `if (!marked)`, on the assumption that the
     * substitution pass was the only other writer. It is not: an
     * isolate_sprite draw fills this same buffer with 1s, and a guest that
     * does an isolate pass and then a normal draw in the same frame (draw
     * the player alone, then draw the world) left those 1s standing -- so
     * the second draw suppressed EVERY sprite. On SMB that showed up as the
     * goombas silently never rendering while the player looked fine. */
    memset(G_nes.suppress, 0, (size_t)AB_NES_W * AB_NES_LINES);
    /* The clear also drops whatever mark_sprites wrote for HD substitution
     * earlier in this draw, so redo it -- the two features have to compose,
     * not cancel. */
    if (marked) {
      const ab_sub_rule *r2 = NULL;
      ab_nes_bounds b2;
      memset(&b2, 0, sizeof(b2));
      ab_nes_mark_sprites(&G_nes.frame, G_nes.sub.registry,
                          nes_sprite_height(), G_nes.suppress, &r2, &b2);
    }
    const int sprite_h2 = nes_sprite_height();
    for (int slot = 0; slot < 64; slot++) {
      if (!G_nes_hide_slot[slot]) continue;
      const int oy = ab_region_read_u8(G_nes.regions.oam, slot * 4);
      const int ox = ab_region_read_u8(G_nes.regions.oam, slot * 4 + 3);
      if (oy >= 0xEF) continue;
      for (int yy = oy + 1; yy < oy + 1 + sprite_h2; yy++) {
        if (yy < 0 || yy >= AB_NES_LINES) continue;
        for (int xx = ox; xx < ox + 8; xx++) {
          if (xx < 0 || xx >= AB_NES_W) continue;
          G_nes.suppress[(size_t)yy * AB_NES_W + xx] = 1;
        }
      }
    }
    use_suppress = 1;
  }

  layer = layer_begin(v->spr_surface);
  ab_batch_reset(G_nes.batch);
  const int spr_quads = ab_nes_emit_sprites(G_nes.batch, &G_nes.frame, &view,
                                            frame_mask,
                                            use_suppress ? G_nes.suppress : NULL);
  ab_batch_flush(G_nes.batch, 0);

  /* Replacement art, anchored to the live metasprite bounds. This is still
   * inside the SPRITE layer: substituted art replaces sprites, so it must
   * land wherever the sprites went or it would be shaded as background. */
  int hd_drawn = 0;
  if (marked && rule && bounds.x1 > bounds.x0) {
    int32_t tex; int tw, th;
    subst_find_art(&G_nes.sub, rule->id, &tex, &tw, &th);
    if (tex) {
      const double bw = bounds.x1 - bounds.x0;
      const double bh = bounds.y1 - bounds.y0;
      /* The canvas is body + a transparent ring; scale the ring with the body
       * so overhang stays proportional to the live footprint. */
      const double rx = (rule->base_w > 0)
                      ? rule->ring * (bw / rule->base_w) * view.scale : 0.0;
      const double ry = (rule->base_h > 0)
                      ? rule->ring * (bh / rule->base_h) * view.scale : 0.0;
      ab_draw_texture_rect(tex,
        view.ox + bounds.x0 * view.scale - rx,
        view.oy + (bounds.y0 - AB_NES_OVERSCAN_TOP) * view.scale - ry,
        bw * view.scale + rx * 2, bh * view.scale + ry * 2,
        0, 0, tw, th);
      hd_drawn = 1;
    }
  }
  layer_end(layer);

  r->bg_quads = bg_quads;
  r->spr_quads = spr_quads;
  r->hd_drawn = hd_drawn;
  r->sprites_replaced = marked;
  /* Consume only what THIS draw actually used.
   *
   * Both directions of getting this wrong are real bugs already seen:
   *  - Clearing nothing left an isolate list standing, so every later draw
   *    emitted only those slots -- "most sprites stopped rendering".
   *  - Clearing everything wiped the hide_cell marks that a LATER draw in
   *    the same frame still needed, so suppressed background text rendered
   *    twice (once warped in the world layer, once clean on top).
   * A guest that does an isolate pass and then a normal pass in one frame
   * sets the two independently, so they are consumed independently. */
  if (G_nes_iso_any) {
    memset(G_nes_iso_slot, 0, sizeof(G_nes_iso_slot));
    G_nes_iso_any = 0;
  } else {
    ab_prof_nes_clear_hidden();
  }
  return 1;
}

int ab_prof_nes_sprite_bounds(int out[4]) {
  const ab_sub_rule *rule = NULL;
  ab_nes_bounds b;
  memset(G_nes.suppress, 0, (size_t)AB_NES_W * AB_NES_LINES);
  const int sprite_h = nes_sprite_height();
  if (!ab_nes_frame_read(&G_nes.regions, &G_nes.frame)) return 0;
  const int n = ab_nes_mark_sprites(&G_nes.frame, G_nes.sub.registry, sprite_h,
                                    G_nes.suppress, &rule, &b);
  if (!n || b.x1 <= b.x0) return 0;
  out[0] = b.x0; out[1] = b.y0; out[2] = b.x1; out[3] = b.y1;
  return 1;
}

void ab_prof_nes_shutdown(void) {
  free(G_nes.bgval);    G_nes.bgval = NULL;
  free(G_nes.bgpix);    G_nes.bgpix = NULL;
  free(G_nes.sprdrawn); G_nes.sprdrawn = NULL;
  free(G_nes.suppress); G_nes.suppress = NULL;
  if (G_nes.batch) { ab_batch_free(G_nes.batch); G_nes.batch = NULL; }
  subst_free(&G_nes.sub);
  G_nes.bound = 0;
}

/* ----------------------------------------------------------------- GB -- */

static struct {
  int bound;
  ab_prof_subst sub;
  ab_gb_regions regions;
  ab_gb_frame   frame;
  unsigned char *bgpix;
  unsigned char *sprpix;
  unsigned char *bgcol15;
  unsigned char *sprcol15;
  unsigned char *suppress;
  ab_batch      *batch;
} G_gb;

int ab_prof_gb_bind(const char **err) {
  if (G_gb.bound) return 1;

  G_gb.regions.lineregs = region_or("gb_lineregs", -1);
  G_gb.regions.bgpix    = region_or("gb_bgpix", -1);
  G_gb.regions.sprpix   = region_or("gb_sprpix", -1);
  G_gb.regions.palline  = region_or("gb_palline", -1);
  G_gb.regions.bgcol15  = region_or("gb_bgcol15", -1);
  G_gb.regions.sprcol15 = region_or("gb_sprcol15", -1);
  G_gb.regions.oam      = region_or("gb_oam", -1);

  if (G_gb.regions.bgpix < 0 || G_gb.regions.sprpix < 0 ||
      G_gb.regions.lineregs < 0 || G_gb.regions.palline < 0) {
    *err = "gb: requires gb_lineregs, gb_bgpix, gb_sprpix and "
           "gb_palline (the resolved layers); is the core patched?";
    return 0;
  }

  G_gb.bgpix    = (unsigned char *)malloc(AB_GB_PIX);
  G_gb.sprpix   = (unsigned char *)malloc(AB_GB_PIX);
  G_gb.bgcol15  = (unsigned char *)malloc((size_t)AB_GB_PIX * 2);
  G_gb.sprcol15 = (unsigned char *)malloc((size_t)AB_GB_PIX * 2);
  G_gb.suppress = (unsigned char *)malloc(AB_GB_PIX);
  G_gb.batch    = ab_batch_new(4096);
  G_gb.sub.registry = ab_registry_new();
  if (!G_gb.bgpix || !G_gb.sprpix || !G_gb.bgcol15 || !G_gb.sprcol15 ||
      !G_gb.suppress || !G_gb.batch || !G_gb.sub.registry) {
    *err = "gb: out of memory";
    return 0;
  }
  G_gb.frame.bgpix    = G_gb.bgpix;
  G_gb.frame.sprpix   = G_gb.sprpix;
  G_gb.frame.bgcol15  = G_gb.bgcol15;
  G_gb.frame.sprcol15 = G_gb.sprcol15;
  G_gb.bound = 1;
  return 1;
}

int ab_prof_gb_draw(const ab_prof_view *v, ab_prof_gb_result *r,
                    const char **err) {
  ab_gb_view view;
  view.ox = v->x; view.oy = v->y; view.scale = v->scale;

  if (!ab_gb_frame_read(&G_gb.regions, &G_gb.frame)) {
    *err = "gb.draw: frame read failed";
    return 0;
  }

  /* Sprite height is LCDC bit 2, sampled from line 0's captured registers.
   * Getting it wrong suppresses only the top half of every replaced sprite. */
  const int sprite_h = ab_gb_sprite_height(&G_gb.frame, 0);

  /* Decide substitution BEFORE anything is drawn. */
  const ab_sub_rule *rule = NULL;
  ab_gb_bounds bounds;
  int marked = 0;
  memset(&bounds, 0, sizeof(bounds));
  if (G_gb.sub.art_count > 0) {
    memset(G_gb.suppress, 0, AB_GB_PIX);
    marked = ab_gb_mark_sprites(&G_gb.frame, G_gb.sub.registry, sprite_h,
                                G_gb.suppress, &rule, &bounds);
  }

  /* Background, then sprites. Each layer is ONE mesh command, optionally
   * routed to its own surface (v->bg_surface / v->spr_surface). */
  int layer = layer_begin(v->bg_surface);
  ab_batch_reset(G_gb.batch);
  const int bg_quads = ab_gb_emit_background(G_gb.batch, &G_gb.frame, &view);
  ab_batch_flush(G_gb.batch, 0);
  layer_end(layer);

  layer = layer_begin(v->spr_surface);
  ab_batch_reset(G_gb.batch);
  const int spr_quads = ab_gb_emit_sprites(G_gb.batch, &G_gb.frame, &view,
                                           marked ? G_gb.suppress : NULL);
  ab_batch_flush(G_gb.batch, 0);

  /* Replacement art, anchored to the live metasprite bounds. Still inside
   * the SPRITE layer: substituted art replaces sprites. */
  int hd_drawn = 0;
  if (marked && rule && bounds.x1 > bounds.x0) {
    int32_t tex; int tw, th;
    subst_find_art(&G_gb.sub, rule->id, &tex, &tw, &th);
    if (tex) {
      const double bw = bounds.x1 - bounds.x0;
      const double bh = bounds.y1 - bounds.y0;
      /* The canvas is body + a transparent ring; scale the ring with the body
       * so overhang stays proportional to the live footprint. */
      const double rx = (rule->base_w > 0)
                      ? rule->ring * (bw / rule->base_w) * view.scale : 0.0;
      const double ry = (rule->base_h > 0)
                      ? rule->ring * (bh / rule->base_h) * view.scale : 0.0;
      /* No overscan offset on GB: the whole 160x144 framebuffer is visible. */
      ab_draw_texture_rect(tex,
        view.ox + bounds.x0 * view.scale - rx,
        view.oy + bounds.y0 * view.scale - ry,
        bw * view.scale + rx * 2, bh * view.scale + ry * 2,
        0, 0, tw, th);
      hd_drawn = 1;
    }
  }
  layer_end(layer);

  r->bg_quads = bg_quads;
  r->spr_quads = spr_quads;
  r->hd_drawn = hd_drawn;
  r->sprites_replaced = marked;
  return 1;
}

int ab_prof_gb_sprite_bounds(int out[4]) {
  const ab_sub_rule *rule = NULL;
  ab_gb_bounds b;
  if (!ab_gb_frame_read(&G_gb.regions, &G_gb.frame)) return 0;
  memset(G_gb.suppress, 0, AB_GB_PIX);
  const int n = ab_gb_mark_sprites(&G_gb.frame, G_gb.sub.registry,
                                   ab_gb_sprite_height(&G_gb.frame, 0),
                                   G_gb.suppress, &rule, &b);
  if (!n || b.x1 <= b.x0) return 0;
  out[0] = b.x0; out[1] = b.y0; out[2] = b.x1; out[3] = b.y1;
  return 1;
}

void ab_prof_gb_shutdown(void) {
  free(G_gb.bgpix);    G_gb.bgpix = NULL;
  free(G_gb.sprpix);   G_gb.sprpix = NULL;
  free(G_gb.bgcol15);  G_gb.bgcol15 = NULL;
  free(G_gb.sprcol15); G_gb.sprcol15 = NULL;
  free(G_gb.suppress); G_gb.suppress = NULL;
  if (G_gb.batch) { ab_batch_free(G_gb.batch); G_gb.batch = NULL; }
  subst_free(&G_gb.sub);
  G_gb.bound = 0;
}

/* ----------------------------------------------------------------- MD -- */

static struct {
  int bound;
  ab_prof_subst sub;
  ab_md_regions regions;
  ab_md_frame   frame;
  unsigned char *linepix;
  unsigned char *bgpix;
  unsigned char *objpix;
  unsigned char *suppress;
  unsigned char *pixlines;
  ab_batch      *batch;
} G_md;

int ab_prof_md_bind(const char **err) {
  if (G_md.bound) return 1;

  G_md.regions.linepix   = region_any("md_linepix", "sms_linepix", "gg_linepix");
  G_md.regions.bgpix     = region_any("md_bgpix", "sms_bgpix", "gg_bgpix");
  G_md.regions.objpix    = region_any("md_objpix", "sms_objpix", "gg_objpix");
  G_md.regions.pixrgb    = region_any("md_pixrgb", "sms_pixrgb", "gg_pixrgb");
  G_md.regions.linestate = region_any("md_linestate", "sms_linestate", "gg_linestate");
  G_md.regions.pixlines  = region_any("md_pixlines", "sms_pixlines", "gg_pixlines");
  G_md.regions.vram      = ab_region_find("video_ram");
  G_md.regions.vdpregs   = region_any("genesis_vdp_regs", "sms_vdp_regs", "sms_vdp_regs");

  if (G_md.regions.bgpix < 0 || G_md.regions.objpix < 0
      || G_md.regions.pixrgb < 0 || G_md.regions.linestate < 0) {
    *err = "md: resolved-layer capture regions missing; "
           "is the gpgx core patched?";
    return 0;
  }

  G_md.linepix  = (G_md.regions.linepix >= 0)
                ? (unsigned char *)malloc((size_t)AB_MD_MAX_W * AB_MD_MAX_H) : NULL;
  G_md.bgpix    = (unsigned char *)malloc((size_t)AB_MD_MAX_W * AB_MD_MAX_H);
  G_md.objpix   = (unsigned char *)malloc((size_t)AB_MD_MAX_W * AB_MD_MAX_H * 2);
  G_md.suppress = (unsigned char *)malloc((size_t)AB_MD_MAX_W * AB_MD_MAX_H);
  G_md.pixlines = (G_md.regions.pixlines >= 0)
                ? (unsigned char *)malloc((size_t)AB_MD_MAX_H * 512) : NULL;
  G_md.batch    = ab_batch_new(4096);
  G_md.sub.registry = ab_registry_new();
  if (!G_md.bgpix || !G_md.objpix || !G_md.suppress || !G_md.batch
      || !G_md.sub.registry) {
    *err = "md: out of memory";
    return 0;
  }
  G_md.frame.linepix  = G_md.linepix;
  G_md.frame.bgpix    = G_md.bgpix;
  G_md.frame.objpix   = G_md.objpix;
  G_md.frame.pixlines = G_md.pixlines;
  G_md.bound = 1;
  return 1;
}

int ab_prof_md_draw(const ab_prof_view *v, ab_prof_md_result *r,
                    const char **err) {
  ab_md_view view;
  view.ox = v->x; view.oy = v->y; view.scale = v->scale;

  if (!ab_md_frame_read(&G_md.regions, &G_md.frame)) {
    *err = "md.draw: frame read failed";
    return 0;
  }

  const ab_sub_rule *rule = NULL;
  ab_md_bounds bounds;
  int marked = 0;
  memset(&bounds, 0, sizeof(bounds));
  if (G_md.sub.art_count > 0) {
    memset(G_md.suppress, 0, (size_t)AB_MD_MAX_W * AB_MD_MAX_H);
    marked = ab_md_mark_sprites(&G_md.regions, &G_md.frame, G_md.sub.registry,
                                G_md.suppress, &rule, &bounds);
  }

  /* NO layer split here, deliberately. Unlike NES/GB -- which emit a
   * background batch and a sprite batch this code could route apart -- the
   * MD path consumes gpgx's RESOLVED per-pixel planes: priority, shadow and
   * highlight are already applied per pixel, so "the sprite layer" is not a
   * separable batch to redirect. Honouring bg_surface here would have to
   * re-render from scratch and would still not reproduce the core's
   * per-pixel priority resolution. ab_prof_md_layers_supported() reports
   * this so a binding can refuse the option loudly instead of ignoring it. */
  int layer = layer_begin(v->bg_surface ? v->bg_surface : v->spr_surface);
  ab_batch_reset(G_md.batch);
  const int quads = ab_md_emit(G_md.batch, &G_md.frame, &view,
                               marked ? G_md.suppress : NULL);
  ab_batch_flush(G_md.batch, 0);

  int hd_drawn = 0;
  if (marked && rule && bounds.x1 > bounds.x0) {
    int32_t tex; int tw, th;
    subst_find_art(&G_md.sub, rule->id, &tex, &tw, &th);
    if (tex) {
      const double bw = bounds.x1 - bounds.x0;
      const double bh = bounds.y1 - bounds.y0;
      const double rx = (rule->base_w > 0)
                      ? rule->ring * (bw / rule->base_w) * view.scale : 0.0;
      const double ry = (rule->base_h > 0)
                      ? rule->ring * (bh / rule->base_h) * view.scale : 0.0;
      ab_draw_texture_rect(tex,
        view.ox + bounds.x0 * view.scale - rx,
        view.oy + bounds.y0 * view.scale - ry,
        bw * view.scale + rx * 2, bh * view.scale + ry * 2,
        0, 0, tw, th);
      hd_drawn = 1;
    }
  }
  layer_end(layer);

  r->quads = quads;
  r->hd_drawn = hd_drawn;
  r->sprites_replaced = marked;
  return 1;
}

int ab_prof_md_sprite_bounds(int out[4]) {
  const ab_sub_rule *rule = NULL;
  ab_md_bounds b;
  memset(G_md.suppress, 0, (size_t)AB_MD_MAX_W * AB_MD_MAX_H);
  if (!ab_md_frame_read(&G_md.regions, &G_md.frame)) return 0;
  const int n = ab_md_mark_sprites(&G_md.regions, &G_md.frame,
                                   G_md.sub.registry, G_md.suppress, &rule, &b);
  if (!n || b.x1 <= b.x0) return 0;
  out[0] = b.x0; out[1] = b.y0; out[2] = b.x1; out[3] = b.y1;
  return 1;
}

void ab_prof_md_shutdown(void) {
  free(G_md.linepix);  G_md.linepix = NULL;
  free(G_md.bgpix);    G_md.bgpix = NULL;
  free(G_md.objpix);   G_md.objpix = NULL;
  free(G_md.suppress); G_md.suppress = NULL;
  free(G_md.pixlines); G_md.pixlines = NULL;
  if (G_md.batch) { ab_batch_free(G_md.batch); G_md.batch = NULL; }
  subst_free(&G_md.sub);
  G_md.bound = 0;
}

/* ---------------------------------------------------------------- MSX -- */

static struct {
  int bound;
  ab_prof_subst sub;
  ab_msx_regions regions;
  ab_msx_frame   frame;
  unsigned char *vram;
  unsigned char *suppress;
  ab_msx_regline *reglines;
  unsigned char  *vdeltas;
  unsigned char  *vram_work;
  uint16_t       *prev_rows;
  unsigned char  *fbtail;
  ab_batch      *batch;
} G_msx;

int ab_prof_msx_bind(const char **err) {
  if (G_msx.bound) return 1;

  G_msx.regions.vram    = region_or("msx_vram", -1);
  G_msx.regions.regs    = region_or("msx_vdp_regs", -1);
  G_msx.regions.status  = region_or("msx_vdp_status", -1);
  G_msx.regions.palette = region_or("msx_palette", -1);
  /* OPTIONAL -- only a bluemsx new enough to have the per-scanline capture
   * exposes it. -1 makes ab_msx.c fall back to the frame snapshot, which is
   * correct for games that never change VDP state mid-screen and is the only
   * behaviour older cores can offer. Not part of the required-region check.
   *
   * This being unwired is NOT harmless: the fallback is byte-identical to the
   * old renderer, so a bezel silently loses every per-scanline fix (SCREEN 5
   * drops from 99.99% to 98.61%, one cart from 99.82% to 43.69%) with no error
   * anywhere. Wire it, and prove it is wired with a control that unwires it. */
  G_msx.regions.reglines = region_or("msx_vdp_reglines", -1);
  /* Same contract, same trap: OPTIONAL, and unwired means the bezel silently
   * renders every row from the end-of-frame VRAM snapshot -- one cart
   * scores 91% instead of 100% with no error anywhere. */
  G_msx.regions.vdeltas = region_or("msx_vram_deltas", -1);
  /* Core fossil-row snapshot. OPTIONAL, and unwired means the rows at/beyond
   * the frame-end cut render from state that never produced them -- the
   * fossil floor (262px) comes back with no error anywhere. */
  G_msx.regions.fbtail = region_or("msx_fb_tail", -1);

  if (G_msx.regions.vram < 0 || G_msx.regions.regs < 0
      || G_msx.regions.palette < 0) {
    *err = "msx: requires msx_vram, msx_vdp_regs and msx_palette "
           "(the raw VDP state); is this an MSX core?";
    return 0;
  }

  G_msx.vram     = (unsigned char *)malloc(AB_MSX_VRAM_SIZE);
  /* AB_MSX_MAXPIX, not AB_MSX_PIX: SCREEN 6/7 present at 544 wide, and
   * ab_msx_mark_sprites indexes the mask by st.out_w. Sizing this to the
   * 272-wide common case overruns the buffer by 2x on those modes. */
  G_msx.suppress = (unsigned char *)malloc(AB_MSX_MAXPIX);
  /* The per-scanline records the profile reads into. Allocated only when the
   * region exists; ab_msx_frame_read checks BOTH the region id and this
   * pointer before touching it. */
  G_msx.reglines = (G_msx.regions.reglines >= 0)
                 ? (ab_msx_regline *)malloc(sizeof(ab_msx_regline) * AB_MSX_REGLINES)
                 : NULL;
  /* Delta log + the mutable working copy the per-line replay renders from. */
  G_msx.vdeltas   = (G_msx.regions.vdeltas >= 0)
                  ? (unsigned char *)malloc(AB_MSX_VDELTAS_SIZE) : NULL;
  G_msx.vram_work = (G_msx.regions.vdeltas >= 0)
                  ? (unsigned char *)malloc(AB_MSX_VRAM_SIZE) : NULL;
  /* Core fossil-row snapshot buffer (see ab_msx.h for why the LIVE bezel
   * must retain from the core snapshot, never from its own prior composite:
   * ticks fire per COMPOSE, so a prior composite is missing-or-stale here). */
  G_msx.fbtail = (G_msx.regions.fbtail >= 0)
               ? (unsigned char *)malloc(AB_MSX_FBTAIL_SIZE) : NULL;
  G_msx.batch    = ab_batch_new(4096);
  G_msx.sub.registry = ab_registry_new();
  if (!G_msx.vram || !G_msx.suppress || !G_msx.batch || !G_msx.sub.registry ||
      (G_msx.regions.reglines >= 0 && !G_msx.reglines) ||
      (G_msx.regions.vdeltas >= 0 && (!G_msx.vdeltas || !G_msx.vram_work)) ||
      (G_msx.regions.fbtail >= 0 && !G_msx.fbtail)) {
    *err = "msx: out of memory";
    return 0;
  }
  G_msx.frame.vram      = G_msx.vram;
  G_msx.frame.reglines  = G_msx.reglines;
  G_msx.frame.vdeltas   = G_msx.vdeltas;
  G_msx.frame.vram_work = G_msx.vram_work;
  /* prev_rows retention stays NULL in the live binding: it is exact only
   * for a per-frame composer (the C test harness). Arming it here corrupted
   * active rows from stale composites (a wide-mode frame, 95.012%). */
  G_msx.frame.prev_rows = NULL;
  G_msx.frame.retain_valid = 0;
  G_msx.frame.fbtail = G_msx.fbtail;
  G_msx.bound = 1;
  return 1;
}

int ab_prof_msx_mode(int *mode, const char **desc) {
  if (!ab_msx_frame_read(&G_msx.regions, &G_msx.frame))
    return AB_PROF_MSX_MODE_READ_FAIL;
  const int m = G_msx.frame.st.mode;
  if (m == AB_MSX_MODE_UNSUPPORTED) {
    /* What is genuinely left: TEXT80, the YJK modes SCREEN 10/12, and the
     * SCREEN 0+2 / 0+3 selector combinations (which a V9938 draws blank). */
    *desc = "unsupported (TEXT80, a YJK mode, or a blank-on-V9938 "
            "mode combination)";
    return AB_PROF_MSX_MODE_UNSUPPORTED;
  }
  *mode = m;
  *desc = m == AB_MSX_MODE_SCREEN0 ? "SCREEN 0 (Text 1)"
        : m == AB_MSX_MODE_SCREEN1 ? "SCREEN 1 (Graphic I)"
        : m == AB_MSX_MODE_SCREEN2 ? "SCREEN 2 (Graphic II)"
        : m == AB_MSX_MODE_SCREEN3 ? "SCREEN 3 (Multicolour)"
        : m == AB_MSX_MODE_SCREEN4 ? "SCREEN 4 (Graphic III)"
        : m == AB_MSX_MODE_SCREEN5 ? "SCREEN 5 (Graphic IV)"
        : m == AB_MSX_MODE_SCREEN6 ? "SCREEN 6 (Graphic V, 512 wide)"
        : m == AB_MSX_MODE_SCREEN7 ? "SCREEN 7 (Graphic VI, 512 wide)"
                                   : "SCREEN 8 (Graphic VII)";
  return AB_PROF_MSX_MODE_OK;
}

int ab_prof_msx_draw(const ab_prof_msx_view *pv, ab_prof_msx_result *r,
                     const char **err) {
  /* memset FIRST: the struct gained fit_width, and a stack view with only the
   * three original fields assigned leaves it holding garbage -- which would
   * flip the scaling policy at random. */
  ab_msx_view view;
  memset(&view, 0, sizeof(view));
  view.ox = pv->v.x; view.oy = pv->v.y; view.scale = pv->v.scale;
  /* Squeeze 512-wide modes into the narrow-mode footprint. Default off: a
   * caller that sized its layout from the real frame width wants one sample
   * per `scale` logical pixels. */
  view.fit_width = pv->fit_width;

  if (!ab_msx_frame_read(&G_msx.regions, &G_msx.frame)) {
    *err = "msx.draw: frame read failed";
    return 0;
  }

  /* An unsupported mode draws NOTHING and says so. Emitting a half-right
   * picture would be worse than an empty one -- the bezel cannot tell that a
   * confident-looking frame is wrong, but it can branch on this flag. */
  if (!ab_msx_supported(&G_msx.frame)) {
    memset(r, 0, sizeof(*r));
    r->supported = 0;
    r->mode = -1;
    return 1;
  }

  /* Decide substitution BEFORE anything is drawn. */
  const ab_sub_rule *rule = NULL;
  ab_msx_bounds bounds;
  int marked = 0;
  memset(&bounds, 0, sizeof(bounds));
  if (G_msx.sub.art_count > 0) {
    memset(G_msx.suppress, 0, AB_MSX_MAXPIX);
    marked = ab_msx_mark_sprites(&G_msx.frame, G_msx.sub.registry,
                                 G_msx.suppress, &rule, &bounds);
  }

  /* One pass: MSX composites sprites into the same scanline walk the VDP
   * does, so splitting into layers would produce a DIFFERENT picture, not a
   * faster one. Flushed as a single mesh command. */
  ab_batch_reset(G_msx.batch);
  const int quads = ab_msx_emit(G_msx.batch, &G_msx.frame, &view,
                                marked ? G_msx.suppress : NULL);
  ab_batch_flush(G_msx.batch, 0);

  /* Replacement art, anchored to the live metasprite bounds. */
  int hd_drawn = 0;
  if (marked && rule && bounds.x1 > bounds.x0) {
    int32_t tex; int tw, th;
    subst_find_art(&G_msx.sub, rule->id, &tex, &tw, &th);
    if (tex) {
      const double bw = bounds.x1 - bounds.x0;
      const double bh = bounds.y1 - bounds.y0;
      /* The canvas is body + a transparent ring; scale the ring with the body
       * so overhang stays proportional to the live footprint. */
      const double rx = (rule->base_w > 0)
                      ? rule->ring * (bw / rule->base_w) * view.scale : 0.0;
      const double ry = (rule->base_h > 0)
                      ? rule->ring * (bh / rule->base_h) * view.scale : 0.0;
      /* Sprite coordinates are DISPLAY-relative; the framebuffer has an 8px
       * border (plus R18's adjust) to the left and firstLine rows above. */
      const double px = view.ox
                      + (AB_MSX_BORDER + G_msx.frame.st.h_adjust + bounds.x0)
                        * view.scale;
      const double py = view.oy
                      + (G_msx.frame.st.first_line - G_msx.frame.st.display_offset
                         + bounds.y0) * view.scale;
      ab_draw_texture_rect(tex, px - rx, py - ry,
                           bw * view.scale + rx * 2, bh * view.scale + ry * 2,
                           0, 0, tw, th);
      hd_drawn = 1;
    }
  }

  r->quads = quads;
  r->hd_drawn = hd_drawn;
  r->sprites_replaced = marked;
  r->supported = 1;
  r->mode = G_msx.frame.st.mode;
  /* The frame's ACTUAL pixel width: 272 normally, 544 in SCREEN 6/7. A bezel
   * laying out around the game view needs this rather than assuming 272. */
  r->width = G_msx.frame.st.out_w;
  /* Whether this frame was reconstructed from PER-SCANLINE records or from
   * the single frame snapshot. A bezel (or a test) can assert on it instead
   * of silently getting the degraded path. */
  r->per_line = G_msx.frame.have_reglines;
  r->vram_replay = G_msx.frame.have_vdeltas;
  /* True when the fossil region beyond the frame-end cut was substituted
   * from the core snapshot this draw (the only live-correct source). */
  r->retained = (G_msx.frame.have_cut && G_msx.frame.have_fbtail);
  return 1;
}

int ab_prof_msx_sprites(ab_prof_msx_sprite *out, int max) {
  if (!ab_msx_frame_read(&G_msx.regions, &G_msx.frame)) return -1;

  const ab_msx_state *st = &G_msx.frame.st;
  const uint32_t vm = st->vram_mask;
  const int32_t base = st->spr_tab & (int32_t)~0x7fu;
  const int pmask = st->sprites_16 ? 0xfc : 0xff;

  int n = 0;
  for (int i = 0; i < AB_MSX_SPRITES && n < max; i++) {
    const int32_t a = base + i * 4;
    const int sy = G_msx.frame.vram[a & vm];
    if (sy == AB_MSX_SPR_END) break;
    const int attr = G_msx.frame.vram[(a + 3) & vm];
    out[n].index = i;
    /* Attribute y is one less than the first visible line; the colour byte's
     * bit 7 ("early clock") shifts the sprite 32px LEFT. */
    out[n].x = G_msx.frame.vram[(a + 1) & vm] - ((attr >> 2) & 0x20);
    out[n].y = sy + 1;
    out[n].pattern = G_msx.frame.vram[(a + 2) & vm] & pmask;
    out[n].colour = attr & 0x0f;
    n++;
  }
  return n;
}

int ab_prof_msx_sprite_bounds(int out[4]) {
  const ab_sub_rule *rule = NULL;
  ab_msx_bounds b;
  if (!ab_msx_frame_read(&G_msx.regions, &G_msx.frame)) return 0;
  memset(G_msx.suppress, 0, AB_MSX_MAXPIX);
  const int n = ab_msx_mark_sprites(&G_msx.frame, G_msx.sub.registry,
                                    G_msx.suppress, &rule, &b);
  if (!n || b.x1 <= b.x0) return 0;
  out[0] = b.x0; out[1] = b.y0; out[2] = b.x1; out[3] = b.y1;
  return 1;
}

void ab_prof_msx_shutdown(void) {
  free(G_msx.vram);      G_msx.vram = NULL;
  free(G_msx.suppress);  G_msx.suppress = NULL;
  free(G_msx.reglines);  G_msx.reglines = NULL;
  free(G_msx.vdeltas);   G_msx.vdeltas = NULL;
  free(G_msx.vram_work); G_msx.vram_work = NULL;
  free(G_msx.prev_rows); G_msx.prev_rows = NULL;
  free(G_msx.fbtail);    G_msx.fbtail = NULL;
  G_msx.frame.prev_rows = NULL; G_msx.frame.fbtail = NULL;
  G_msx.frame.reglines = NULL; G_msx.frame.vdeltas = NULL;
  G_msx.frame.vram_work = NULL;
  if (G_msx.batch) { ab_batch_free(G_msx.batch); G_msx.batch = NULL; }
  subst_free(&G_msx.sub);
  G_msx.bound = 0;
}

/* ---------------------------------------------------------------- PCE -- */

static struct {
  int bound;
  ab_prof_subst sub;
  ab_pce_regions regions;
  ab_pce_frame   frame;
  unsigned char *vram;
  unsigned char *suppress;
  unsigned char *pallines;
  unsigned char *linepix;
  unsigned char *paldeltas;
  int            suppress_size;
  ab_batch      *batch;
} G_pce;

int ab_prof_pce_bind(const char **err) {
  if (G_pce.bound) return 1;

  G_pce.regions.vram    = region_or("pce_vdc_vram", -1);
  G_pce.regions.satb    = region_or("pce_vdc_satb", -1);
  G_pce.regions.regs    = region_or("pce_vdc_regs", -1);
  G_pce.regions.palette = region_or("pce_vce_palette", -1);
  /* OPTIONAL -- only a geargrafx new enough to have the per-scanline capture
   * exposes it. -1 makes ab_pce.c fall back to the frame-end registers, which
   * is correct for games that do not raster-split and is the only behaviour
   * older cores can offer. Not part of the required-region check. */
  G_pce.regions.reglines = region_or("pce_vdc_reglines", -1);
  /* The other four per-line regions, same contract: OPTIONAL, -1 when the
   * core predates them, and the profile falls back. Leaving them unresolved
   * is NOT harmless -- pce_vdc_linepix is the VDC's own resolved line buffer,
   * so the profile can use it INSTEAD of re-running the line renderer (exact
   * and cheaper), and pce_vce_pallines is the only way to follow a mid-frame
   * recolour. Unwired, a bezel silently gets the degraded path with no error. */
  G_pce.regions.pallines  = region_or("pce_vce_pallines",  -1);
  G_pce.regions.linepix   = region_or("pce_vdc_linepix",   -1);
  G_pce.regions.xofflines = region_or("pce_vce_xofflines", -1);
  G_pce.regions.srclines  = region_or("pce_vce_srclines",  -1);
  /* Dot-stamped VCE palette write log: without it a mid-line recolour
   * renders the row's head through the end-of-line value with no error
   * anywhere -- the same silent-fallback trap as every optional region
   * above. */
  G_pce.regions.paldeltas = region_or("pce_paldeltas", -1);

  if (G_pce.regions.vram < 0 || G_pce.regions.satb < 0 ||
      G_pce.regions.regs < 0 || G_pce.regions.palette < 0) {
    *err = "pce: requires pce_vdc_vram, pce_vdc_satb, pce_vdc_regs "
           "and pce_vce_palette; is the core patched?";
    return 0;
  }

  G_pce.suppress_size = AB_PCE_MAX_W * AB_PCE_MAX_H;
  G_pce.vram     = (unsigned char *)malloc((size_t)AB_PCE_VRAM_WORDS * 2);
  G_pce.suppress = (unsigned char *)malloc((size_t)G_pce.suppress_size);
  /* Caller-owned per-line buffers, allocated ONLY when the region exists so a
   * bezel on an older core pays nothing for them. */
  G_pce.pallines = (G_pce.regions.pallines >= 0)
                 ? (unsigned char *)malloc((size_t)AB_PCE_PALLINES * AB_PCE_PALLINE_SIZE)
                 : NULL;
  G_pce.paldeltas = (G_pce.regions.paldeltas >= 0)
                  ? (unsigned char *)malloc((size_t)AB_PCE_PALDELTAS_SIZE) : NULL;
  G_pce.linepix  = (G_pce.regions.linepix >= 0)
                 ? (unsigned char *)malloc((size_t)AB_PCE_LINEPIX_LINES * AB_PCE_LINEPIX_SIZE)
                 : NULL;
  G_pce.batch    = ab_batch_new(8192);
  G_pce.sub.registry = ab_registry_new();
  if (!G_pce.vram || !G_pce.suppress || !G_pce.batch || !G_pce.sub.registry ||
      (G_pce.regions.pallines >= 0 && !G_pce.pallines) ||
      (G_pce.regions.linepix  >= 0 && !G_pce.linepix) ||
      (G_pce.regions.paldeltas >= 0 && !G_pce.paldeltas)) {
    *err = "pce: out of memory";
    return 0;
  }
  G_pce.frame.vram      = G_pce.vram;
  G_pce.frame.pallines  = G_pce.pallines;
  G_pce.frame.linepix   = G_pce.linepix;
  G_pce.frame.paldeltas = G_pce.paldeltas;
  G_pce.bound = 1;
  return 1;
}

void ab_prof_pce_view_init(ab_prof_pce_view *v) {
  v->v.x = 0; v->v.y = 0; v->v.scale = 4.0;
  /* PCE resolves its layers per pixel like MD, so it has no separable
   * sprite batch to route; ab_prof_layers_supported() says so and the
   * bindings refuse the option. Zero them anyway -- an uninitialised
   * handle here would target a random surface. */
  v->v.bg_surface = 0; v->v.spr_surface = 0; v->v.solid_surface = 0;
  v->height = 224;
  v->force_bg = -1;
  v->force_sprites = -1;
  /* 0 = "derive from the VDC registers". A bezel that can read the host's
   * framebuffer width should pass it -- see ab_pce_view.fb_width. */
  v->fb_width = 0;
  v->pal_delta_row = AB_PROF_PCE_UNSET;
  v->pal_delta = AB_PROF_PCE_UNSET;
  v->no_linepix = -1;
  v->no_paldeltas = -1;
}

int ab_prof_pce_draw(const ab_prof_pce_view *pv, ab_prof_pce_result *r,
                     const char **err) {
  ab_pce_view view;
  view.ox = pv->v.x; view.oy = pv->v.y; view.scale = pv->v.scale;
  view.height = pv->height;
  view.force_bg = pv->force_bg;
  view.force_sprites = pv->force_sprites;
  /* THE CORE'S framebuffer width (the VCE line width), not the VDC window.
   * They differ on 341-wide games and the difference is an 8px left clip
   * that puts the entire picture out of place. */
  view.fb_width = pv->fb_width;

  /* Diagnostic/control: force the reconstruction path instead of the core's
   * resolved line buffer. Only poked when the script actually passed the
   * field, exactly like the original binding. */
  if (pv->pal_delta_row != AB_PROF_PCE_UNSET)
    ab_pce_test_pal_delta_row = pv->pal_delta_row;
  if (pv->pal_delta != AB_PROF_PCE_UNSET)
    ab_pce_test_pal_vpos_delta = pv->pal_delta;
  if (pv->no_linepix >= 0)   ab_pce_test_no_linepix = pv->no_linepix;
  if (pv->no_paldeltas >= 0) ab_pce_test_no_paldeltas = pv->no_paldeltas;

  if (view.fb_width < 0 || view.fb_width > AB_PCE_MAX_W) view.fb_width = 0;
  if (view.height <= 0 || view.height > AB_PCE_MAX_H) view.height = AB_PCE_MAX_H;

  if (!ab_pce_frame_read(&G_pce.regions, &G_pce.frame)) {
    *err = "pce.draw: frame read failed";
    return 0;
  }

  /* Same rule the emitter uses: caller-supplied framebuffer width wins, and
   * the VDC active window is only the fallback. The suppress mask and the
   * sprite marking must be indexed by the SAME width the emitter walks, or
   * replacement art lands on the wrong columns. */
  const int width = (view.fb_width > 0) ? view.fb_width
                                        : ab_pce_width(&G_pce.frame);

  /* Decide substitution BEFORE anything is drawn. */
  const ab_sub_rule *rule = NULL;
  ab_pce_bounds bounds;
  int marked = 0;
  memset(&bounds, 0, sizeof(bounds));
  if (G_pce.sub.art_count > 0) {
    const size_t need = (size_t)width * (size_t)view.height;
    if (need <= (size_t)G_pce.suppress_size) {
      memset(G_pce.suppress, 0, need);
      marked = ab_pce_mark_sprites(&G_pce.frame, G_pce.sub.registry, width,
                                   view.height, G_pce.suppress, &rule, &bounds);
    }
  }

  /* One pass: the VDC has already merged sprites into its line buffer, so
   * there is no separate background layer to emit (see ab_pce.h). */
  ab_batch_reset(G_pce.batch);
  const int quads = ab_pce_emit(G_pce.batch, &G_pce.frame, &view,
                                marked ? G_pce.suppress : NULL);
  ab_batch_flush(G_pce.batch, 0);

  /* Replacement art, anchored to the live metasprite bounds. */
  int hd_drawn = 0;
  if (marked && rule && bounds.x1 > bounds.x0) {
    int32_t tex; int tw, th;
    subst_find_art(&G_pce.sub, rule->id, &tex, &tw, &th);
    if (tex) {
      const double bw = bounds.x1 - bounds.x0;
      const double bh = bounds.y1 - bounds.y0;
      /* The canvas is body + a transparent ring; scale the ring with the body
       * so overhang stays proportional to the live footprint. */
      const double rx = (rule->base_w > 0)
                      ? rule->ring * (bw / rule->base_w) * view.scale : 0.0;
      const double ry = (rule->base_h > 0)
                      ? rule->ring * (bh / rule->base_h) * view.scale : 0.0;
      ab_draw_texture_rect(tex,
        view.ox + bounds.x0 * view.scale - rx,
        view.oy + bounds.y0 * view.scale - ry,
        bw * view.scale + rx * 2, bh * view.scale + ry * 2,
        0, 0, tw, th);
      hd_drawn = 1;
    }
  }

  r->quads = quads;
  r->hd_drawn = hd_drawn;
  r->sprites_replaced = marked;
  r->width = width;
  r->height = view.height;
  return 1;
}

int ab_prof_pce_sprite_bounds(int height, int fb_width, int out[4]) {
  if (height <= 0) height = 224;
  if (fb_width < 0 || fb_width > AB_PCE_MAX_W) fb_width = 0;
  const ab_sub_rule *rule = NULL;
  ab_pce_bounds b;
  if (!ab_pce_frame_read(&G_pce.regions, &G_pce.frame)) return 0;
  const int width = (fb_width > 0) ? fb_width : ab_pce_width(&G_pce.frame);
  const size_t need = (size_t)width * (size_t)height;
  if (need > (size_t)G_pce.suppress_size) return 0;
  memset(G_pce.suppress, 0, need);
  const int n = ab_pce_mark_sprites(&G_pce.frame, G_pce.sub.registry, width,
                                    height, G_pce.suppress, &rule, &b);
  if (!n || b.x1 <= b.x0) return 0;
  out[0] = b.x0; out[1] = b.y0; out[2] = b.x1; out[3] = b.y1;
  return 1;
}

int ab_prof_pce_get_geometry(ab_prof_pce_geometry *g) {
  if (!ab_pce_frame_read(&G_pce.regions, &G_pce.frame)) return 0;
  static const int bat_w[8] = { 32, 64, 128, 128, 32, 64, 128, 128 };
  static const int bat_h[8] = { 32, 32, 32, 32, 64, 64, 64, 64 };
  const uint16_t cr  = ab_pce_reg(&G_pce.frame, AB_PCE_REG_CR);
  const uint16_t mwr = ab_pce_reg(&G_pce.frame, AB_PCE_REG_MWR);
  const int scr = (mwr >> 4) & 7;
  g->width    = ab_pce_width(&G_pce.frame);
  g->bg       = (cr & AB_PCE_CR_BG_ON) != 0;
  g->sprites  = (cr & AB_PCE_CR_SPR_ON) != 0;
  g->bat_w    = bat_w[scr];
  g->bat_h    = bat_h[scr];
  g->scroll_x = ab_pce_reg(&G_pce.frame, AB_PCE_REG_BXR);
  g->scroll_y = ab_pce_reg(&G_pce.frame, AB_PCE_REG_BYR);
  return 1;
}

void ab_prof_pce_shutdown(void) {
  free(G_pce.vram);      G_pce.vram = NULL;
  free(G_pce.pallines);  G_pce.pallines = NULL;
  free(G_pce.linepix);   G_pce.linepix = NULL;
  free(G_pce.paldeltas); G_pce.paldeltas = NULL;
  G_pce.frame.pallines = NULL; G_pce.frame.linepix = NULL;
  G_pce.frame.paldeltas = NULL;
  free(G_pce.suppress);  G_pce.suppress = NULL;
  if (G_pce.batch) { ab_batch_free(G_pce.batch); G_pce.batch = NULL; }
  subst_free(&G_pce.sub);
  G_pce.bound = 0;
}

/* ---------------------------------------------------- shared dispatch -- */

static ab_prof_common *prof_state(ab_prof_id p) {
  switch (p) {
    case AB_PROF_NES: return (void *)&G_nes;
    case AB_PROF_GB:  return (void *)&G_gb;
    case AB_PROF_MD:  return (void *)&G_md;
    case AB_PROF_MSX: return (void *)&G_msx;
    case AB_PROF_PCE: default: return (void *)&G_pce;
  }
}

/* --------------------------------------------------------------- SNES -- */
/*
 * The `snes` profile: the whole Mode 7 HD re-projection tick in C. The
 * scripting side shrinks to orchestration. Why C: the Lua version of this
 * tick cost 25-31ms steady / 100-230ms on VRAM-streaming ticks. Every hot
 * loop here -- linepix 565->RGBA, the OBJ depth-run scan, palette build,
 * plane/overlay row assembly, per-line UV math -- is a few hundred
 * microseconds in C. Draw calls collapse to <=4 meshes + 2 texture rects.
 */

#define SNES_LOGICAL_W 1920
#define SNES_LOGICAL_H 1080
#define SNES_MAX_VERTS 65536

static struct {
  int bound;
  int32_t r_lpx, r_lstate, r_finfo, r_m7, r_depth, r_clip, r_vram, r_cgram;

  /* per-tick capture buffers */
  uint8_t fi[8];
  uint8_t lstate[240 * 16];
  uint8_t m7[240 * 16];
  uint8_t depth[240 * 512];
  uint8_t clip[240 * 16];
  uint8_t lpx[480 * 1024];
  uint8_t vram[65536];
  uint8_t vram_cache[65536];
  int     vram_valid;
  uint8_t cgram[512];
  uint8_t cgram_cache[512];
  int     cgram_valid;

  /* CPU-side texture images */
  uint8_t lpx_rgba[256 * 240 * 4];
  uint8_t *lpx_big;              /* hi-res/interlace (up to 512x480) */
  size_t   lpx_big_size;
  uint8_t pal_rgba[256 * 4];
  uint8_t *plane;               /* 1024*1024*4, R=index A=255 */
  uint8_t *ov_rgba;             /* 4096*4096*4, painted static tiles */
  uint8_t *ov_idx;              /* 4096*4096*4, painted indexed tiles */

  /* painted tiles (tiles.bin v2) */
  uint8_t *hd_blob;
  struct { uint8_t mode; const uint8_t *data; } hd[256];
  int hd_any, hd_any_rgba, hd_any_idx;

  /* GPU handles */
  int32_t tex_lpx, tex_pal, tex_plane, tex_ov_rgba, tex_ov_idx;
  /* Textures whose draws are still QUEUED. The host flushes queued draws
   * at compose time, after the guest tick returns, so a texture destroyed
   * in the same tick that drew it is gone before the flush reads it --
   * rows silently render black. Destroy LAST tick's textures instead
   * (the rule the Lua oracle documents as "destroyed NEXT tick"). */
  int32_t pending_free[4];
  int      pending_n;

  ab_vertex verts[SNES_MAX_VERTS];
} G_snes;

static void snes_retire_pending(void) {
  for (int i = 0; i < G_snes.pending_n; i++)
    if (G_snes.pending_free[i]) ab_texture_destroy(G_snes.pending_free[i]);
  G_snes.pending_n = 0;
}

static void snes_defer_free(int32_t tex) {
  if (!tex) return;
  if (G_snes.pending_n <
      (int)(sizeof(G_snes.pending_free) / sizeof(G_snes.pending_free[0])))
    G_snes.pending_free[G_snes.pending_n++] = tex;
}

int ab_prof_snes_bound(void) { return G_snes.bound; }

int ab_prof_snes_bind(const char **err) {
  if (G_snes.bound) return 1;
  G_snes.r_lpx    = ab_region_find("snes_linepix");
  G_snes.r_lstate = ab_region_find("snes_linestate");
  G_snes.r_finfo  = ab_region_find("snes_frameinfo");
  G_snes.r_m7     = ab_region_find("snes_m7lines");
  G_snes.r_depth  = ab_region_find("snes_linedepth");
  G_snes.r_clip   = ab_region_find("snes_cliplines");
  G_snes.r_vram   = ab_region_find("video_ram");
  G_snes.r_cgram  = ab_region_find("snes_cgram");
  if (G_snes.r_lpx < 0 || G_snes.r_lstate < 0 || G_snes.r_finfo < 0 ||
      G_snes.r_m7 < 0 || G_snes.r_depth < 0 || G_snes.r_clip < 0 ||
      G_snes.r_vram < 0 || G_snes.r_cgram < 0) {
    *err = "snes: capture regions missing (need the m7/depth/clip snes9x build)";
    return 0;
  }
  G_snes.plane = (uint8_t *)malloc(1024 * 1024 * 4);
  if (!G_snes.plane) { *err = "snes: plane alloc failed"; return 0; }
  G_snes.bound = 1;
  return 1;
}

/* tiles.bin v2: 'T','2', u16 LE count, then per tile:
 * u16 LE id, u8 mode, payload (mode 0: 32*32*4 RGBA; mode 1: 32*32*2 idx+alpha) */
int ab_prof_snes_set_hd_tiles(const void *blob_v, size_t n,
                              int *indexed_count, int *rgba_count,
                              const char **err) {
  const char *blob = (const char *)blob_v;
  if (n < 4 || blob[0] != 'T' || blob[1] != '2') {
    *err = "snes: tiles.bin is not v2";
    return 0;
  }
  free(G_snes.hd_blob);
  G_snes.hd_blob = (uint8_t *)malloc(n);
  if (!G_snes.hd_blob) { *err = "snes: hd blob alloc failed"; return 0; }
  memcpy(G_snes.hd_blob, blob, n);
  memset(G_snes.hd, 0, sizeof(G_snes.hd));
  G_snes.hd_any = G_snes.hd_any_rgba = G_snes.hd_any_idx = 0;
  const uint8_t *p = G_snes.hd_blob;
  int count = p[2] | (p[3] << 8);
  size_t off = 4;
  int idxn = 0, rgban = 0;
  for (int i = 0; i < count; i++) {
    if (off + 3 > n) break;
    int id = p[off] | (p[off + 1] << 8);
    int mode = p[off + 2];
    off += 3;
    size_t need = mode == 1 ? 32 * 32 * 2 : 32 * 32 * 4;
    if (off + need > n) break;
    if (id < 256) {
      G_snes.hd[id].mode = (uint8_t)(mode + 1); /* 0 = absent, 1 = rgba, 2 = indexed */
      G_snes.hd[id].data = p + off;
      G_snes.hd_any = 1;
      if (mode == 1) { G_snes.hd_any_idx = 1; idxn++; }
      else { G_snes.hd_any_rgba = 1; rgban++; }
    }
    off += need;
  }
  if (G_snes.hd_any) {
    if (!G_snes.ov_rgba) G_snes.ov_rgba = (uint8_t *)malloc((size_t)4096 * 4096 * 4);
    if (!G_snes.ov_idx)  G_snes.ov_idx  = (uint8_t *)malloc((size_t)4096 * 4096 * 4);
    if (!G_snes.ov_rgba || !G_snes.ov_idx) {
      *err = "snes: overlay alloc failed";
      return 0;
    }
  }
  G_snes.vram_valid = 0;                       /* force full rebuild */
  *indexed_count = idxn;
  *rgba_count = rgban;
  return 1;
}

/* Rebuild plane (and overlays) rows for map row my from G_snes.vram. */
static void snes_build_row(int my) {
  const uint8_t *vram = G_snes.vram;
  for (int mx = 0; mx < 128; mx++) {
    int t = vram[(my * 128 + mx) * 2];
    /* plane: 8x8 indices at (mx*8, my*8) */
    for (int py = 0; py < 8; py++) {
      uint8_t *dst = G_snes.plane
                   + (((size_t)my * 8 + py) * 1024 + (size_t)mx * 8) * 4;
      const uint8_t *src = vram + (t * 64 + py * 8) * 2 + 1;
      for (int px = 0; px < 8; px++) {
        dst[px * 4] = src[px * 2];
        dst[px * 4 + 1] = 0;
        dst[px * 4 + 2] = 0;
        dst[px * 4 + 3] = 255;
      }
    }
    if (!G_snes.hd_any) continue;
    /* overlays: 32x32 at (mx*32, my*32) */
    int mode = G_snes.hd[t].mode;
    const uint8_t *hd = G_snes.hd[t].data;
    for (int py = 0; py < 32; py++) {
      size_t o = (((size_t)my * 32 + py) * 4096 + (size_t)mx * 32) * 4;
      uint8_t *dr = G_snes.ov_rgba + o;
      uint8_t *di = G_snes.ov_idx + o;
      if (mode == 1) {                        /* static RGBA art */
        memcpy(dr, hd + py * 32 * 4, 32 * 4);
        memset(di, 0, 32 * 4);
      } else if (mode == 2) {                 /* palette-indexed art */
        memset(dr, 0, 32 * 4);
        const uint8_t *s = hd + py * 32 * 2;
        for (int px = 0; px < 32; px++) {
          di[px * 4] = s[px * 2];
          di[px * 4 + 1] = 0;
          di[px * 4 + 2] = 0;
          di[px * 4 + 3] = s[px * 2 + 1];
        }
      } else {
        memset(dr, 0, 32 * 4);
        memset(di, 0, 32 * 4);
      }
    }
  }
}

/* VRAM tracking: memcmp whole 64KB; on change rebuild dirty 256B chunks
 * (one map row + the two tiles whose pixels share those words). Returns 1
 * if any texture was recreated. */
static int snes_update_plane(void) {
  if (ab_region_read(G_snes.r_vram, 0, G_snes.vram, 65536) != 65536) return 0;
  if (G_snes.vram_valid
      && memcmp(G_snes.vram, G_snes.vram_cache, 65536) == 0
      && G_snes.tex_plane)
    return 0;

  int have_tex = G_snes.tex_plane != 0;
  int dirty_rows[128], dn = 0;
  for (int my = 0; my < 128; my++) {
    if (G_snes.vram_valid &&
        memcmp(G_snes.vram + my * 256, G_snes.vram_cache + my * 256, 256) == 0)
      continue;
    snes_build_row(my);
    if (dn < 128) dirty_rows[dn++] = my;
  }
  /* a changed chunk also invalidates tiles 2my/2my+1 wherever they are
   * referenced; other rows referencing them are rebuilt when their own
   * chunk streams (self-heals). Full rebuild covers the first tick. */
  memcpy(G_snes.vram_cache, G_snes.vram, 65536);
  G_snes.vram_valid = 1;
  if (dn == 0 && have_tex) return 0;

  if (!have_tex) {
    /* first build: create everything */
    G_snes.tex_plane = ab_texture_create_rgba(G_snes.plane, 1024, 1024);
    ab_texture_filter(G_snes.tex_plane, 3 + 16);      /* palette bicubic + REPEAT */
    if (G_snes.hd_any_rgba) {
      G_snes.tex_ov_rgba = ab_texture_create_rgba(G_snes.ov_rgba, 4096, 4096);
      ab_texture_filter(G_snes.tex_ov_rgba, 2 + 16);  /* bicubic + REPEAT */
    }
    if (G_snes.hd_any_idx) {
      G_snes.tex_ov_idx = ab_texture_create_rgba(G_snes.ov_idx, 4096, 4096);
      ab_texture_filter(G_snes.tex_ov_idx, 3 + 16);   /* palette bicubic + REPEAT */
    }
    return 1;
  }

  /* streaming: patch ONLY the dirty row bands in place (glTexSubImage2D
   * under the hood). Re-creating the 4096x4096 overlays instead cost two
   * 64MB copies+uploads per streaming tick -- the whole reason the pure-Lua
   * bezel (and the first cut of this one) hitched. */
  for (int i = 0; i < dn; i++) {
    int my = dirty_rows[i];
    ab_texture_update(G_snes.tex_plane, 0, my * 8, 1024, 8,
                      G_snes.plane + (size_t)my * 8 * 1024 * 4);
    if (G_snes.tex_ov_rgba)
      ab_texture_update(G_snes.tex_ov_rgba, 0, my * 32, 4096, 32,
                        G_snes.ov_rgba + (size_t)my * 32 * 4096 * 4);
    if (G_snes.tex_ov_idx)
      ab_texture_update(G_snes.tex_ov_idx, 0, my * 32, 4096, 32,
                        G_snes.ov_idx + (size_t)my * 32 * 4096 * 4);
  }
  return 0;   /* handles unchanged; no palette re-association needed */
}

static void snes_emit_quad(int *n, double x0, double y0, double x1, double y1,
                           double u0, double v0, double u1, double v1) {
  if (*n + 6 > SNES_MAX_VERTS) return;
  ab_vertex *v = &G_snes.verts[*n];
  v[0] = (ab_vertex){ (float)x0, (float)y0, (float)u0, (float)v0, 0xFFFFFFFFu, 0 };
  v[1] = (ab_vertex){ (float)x1, (float)y0, (float)u1, (float)v0, 0xFFFFFFFFu, 0 };
  v[2] = (ab_vertex){ (float)x0, (float)y1, (float)u0, (float)v1, 0xFFFFFFFFu, 0 };
  v[3] = v[2];
  v[4] = v[1];
  v[5] = (ab_vertex){ (float)x1, (float)y1, (float)u1, (float)v1, 0xFFFFFFFFu, 0 };
  *n += 6;
}

/* one strip segment with independent corner UVs */
static void snes_emit_strip(int *n, double x0, double y0, double x1, double y1,
                            double au0, double av0, double au1, double av1,
                            double bu0, double bv0, double bu1, double bv1) {
  if (*n + 6 > SNES_MAX_VERTS) return;
  ab_vertex *v = &G_snes.verts[*n];
  v[0] = (ab_vertex){ (float)x0, (float)y0, (float)au0, (float)av0, 0xFFFFFFFFu, 0 };
  v[1] = (ab_vertex){ (float)x1, (float)y0, (float)au1, (float)av1, 0xFFFFFFFFu, 0 };
  v[2] = (ab_vertex){ (float)x0, (float)y1, (float)bu0, (float)bv0, 0xFFFFFFFFu, 0 };
  v[3] = v[2];
  v[4] = v[1];
  v[5] = (ab_vertex){ (float)x1, (float)y1, (float)bu1, (float)bv1, 0xFFFFFFFFu, 0 };
  *n += 6;
}

int ab_prof_snes_tick(int compare, ab_prof_snes_tick_result *r,
                      const char **err) {
  (void)err;
  snes_retire_pending();

  ab_region_read(G_snes.r_finfo, 0, G_snes.fi, 8);
  int width = G_snes.fi[0] | (G_snes.fi[1] << 8);
  int lines = G_snes.fi[2] | (G_snes.fi[3] << 8);
  if (width == 0 || lines == 0 || lines > 240 || width > 256)
    return AB_PROF_SNES_NOT_READY;             /* hi-res / not ready */

  ab_region_read(G_snes.r_lstate, 0, G_snes.lstate, lines * 16);
  ab_region_read(G_snes.r_m7, 0, G_snes.m7, lines * 16);
  ab_region_read(G_snes.r_depth, 0, G_snes.depth, lines * 512);
  ab_region_read(G_snes.r_clip, 0, G_snes.clip, lines * 16);
  ab_region_read(G_snes.r_lpx, 0, G_snes.lpx, lines * 1024);

  int rebuilt = snes_update_plane();

  /* palette: rebuild texture only when CGRAM changed; re-associate when
   * either side's texture was recreated */
  ab_region_read(G_snes.r_cgram, 0, G_snes.cgram, 512);
  int pal_new = 0;
  if (!G_snes.cgram_valid
      || memcmp(G_snes.cgram, G_snes.cgram_cache, 512) != 0
      || !G_snes.tex_pal) {
    ab_snes_palette(G_snes.cgram, G_snes.pal_rgba);
    snes_defer_free(G_snes.tex_pal);
    G_snes.tex_pal = ab_texture_create_rgba(G_snes.pal_rgba, 256, 1);
    memcpy(G_snes.cgram_cache, G_snes.cgram, 512);
    G_snes.cgram_valid = 1;
    pal_new = 1;
  }
  if (pal_new || rebuilt) {
    ab_texture_palette(G_snes.tex_plane, G_snes.tex_pal);
    if (G_snes.tex_ov_idx) ab_texture_palette(G_snes.tex_ov_idx, G_snes.tex_pal);
  }

  /* linepix texture (every tick: it IS the frame) */
  for (int y = 0; y < lines; y++)
    ab_snes_565_row(G_snes.lpx + y * 1024,
                    G_snes.lpx_rgba + (size_t)y * width * 4, width);
  snes_defer_free(G_snes.tex_lpx);
  G_snes.tex_lpx = ab_texture_create_rgba(G_snes.lpx_rgba, width, lines);

  /* m7 span */
  int m7start = -1, m7stop = -1;
  for (int y = 0; y < lines; y++) {
    if ((G_snes.lstate[y * 16 + 1] & 7) == 7) {
      if (m7start < 0) m7start = y;
      m7stop = y;
    }
  }

  /* layout */
  int scale, ox, oy, rx = 0;
  if (compare) {
    scale = 3;
    int vw = width * scale;
    int gap = (SNES_LOGICAL_W - vw * 2) / 3;
    ox = gap;
    rx = gap * 2 + vw;
    oy = (SNES_LOGICAL_H - lines * scale) / 2;
  } else {
    scale = 4;
    ox = (SNES_LOGICAL_W - width * scale) / 2;
    oy = (SNES_LOGICAL_H - lines * scale) / 2;
  }

  if (m7start < 0) {
    ab_draw_texture_rect(G_snes.tex_lpx, ox, oy, width * scale, lines * scale,
                         0, 0, width, lines);
    if (compare)
      ab_draw_texture_rect(G_snes.tex_lpx, rx, oy, width * scale, lines * scale,
                           0, 0, width, lines);
    return AB_PROF_SNES_PLAIN;
  }

  /* the plane mesh: strips per line pair, segmented by resolved clips */
  int n = 0;
  for (int y = m7start; y <= m7stop; y++) {
    double a[4], b[4];
    ab_snes_m7_uv(G_snes.m7, y, width, &a[0], &a[1], &a[2], &a[3]);
    if (y < m7stop) ab_snes_m7_uv(G_snes.m7, y + 1, width, &b[0], &b[1], &b[2], &b[3]);
    else memcpy(b, a, sizeof(a));
    double q[8] = { a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3] };
    ab_snes_wrap_align(q);

    double ty = oy + (double)y * scale;
    double by = ty + scale;

    const uint8_t *cl = G_snes.clip + y * 16;
    int cn = cl[0];
    int segs[8], sn = 0;
    if (cn == 0) { segs[0] = 0; segs[1] = width; sn = 1; }
    else {
      for (int ci = 0; ci < cn && ci < 3 && sn < 4; ci++) {
        int L = cl[2 + ci * 5] | (cl[3 + ci * 5] << 8);
        int R = cl[4 + ci * 5] | (cl[5 + ci * 5] << 8);
        if (R > width) R = width;
        if (R > L) { segs[sn * 2] = L; segs[sn * 2 + 1] = R; sn++; }
      }
    }
    for (int si = 0; si < sn; si++) {
      double f0 = (double)segs[si * 2] / width;
      double f1 = (double)segs[si * 2 + 1] / width;
      snes_emit_strip(&n,
        ox + f0 * width * scale, ty, ox + f1 * width * scale, by,
        q[0] + (q[2] - q[0]) * f0, q[1] + (q[3] - q[1]) * f0,
        q[0] + (q[2] - q[0]) * f1, q[1] + (q[3] - q[1]) * f1,
        q[4] + (q[6] - q[4]) * f0, q[5] + (q[7] - q[5]) * f0,
        q[4] + (q[6] - q[4]) * f1, q[5] + (q[7] - q[5]) * f1);
    }
  }
  if (n) {
    ab_mesh(G_snes.verts, n, G_snes.tex_plane);
    if (G_snes.tex_ov_idx)  ab_mesh(G_snes.verts, n, G_snes.tex_ov_idx);
    if (G_snes.tex_ov_rgba) ab_mesh(G_snes.verts, n, G_snes.tex_ov_rgba);
  }

  /* linepix-sourced quads: HUD rows above/below the span, sprite runs,
   * and the compare view -- ONE mesh on the linepix texture */
  n = 0;
  double iw = 1.0 / width, ih = 1.0 / lines;
  if (m7start > 0)
    snes_emit_quad(&n, ox, oy, ox + (double)width * scale,
                   oy + (double)m7start * scale,
                   0, 0, 1, (double)m7start * ih);
  if (m7stop < lines - 1)
    snes_emit_quad(&n, ox, oy + (double)(m7stop + 1) * scale,
                   ox + (double)width * scale, oy + (double)lines * scale,
                   0, (double)(m7stop + 1) * ih, 1, 1);
  int runs[64];
  for (int y = m7start; y <= m7stop; y++) {
    int rn = ab_snes_depth_runs(G_snes.depth + y * 512, width, runs, 32);
    for (int i = 0; i < rn; i++) {
      int x0 = runs[i * 2], x1 = runs[i * 2 + 1];
      snes_emit_quad(&n, ox + (double)x0 * scale, oy + (double)y * scale,
                     ox + (double)x1 * scale, oy + (double)(y + 1) * scale,
                     (double)x0 * iw, (double)y * ih,
                     (double)x1 * iw, (double)(y + 1) * ih);
    }
  }
  if (compare)
    snes_emit_quad(&n, rx, oy, rx + (double)width * scale,
                   oy + (double)lines * scale, 0, 0, 1, 1);
  if (n) ab_mesh(G_snes.verts, n, G_snes.tex_lpx);

  r->w = width;
  r->h = lines;
  r->m7start = m7start;
  r->m7stop = m7stop;
  r->plane_rebuilt = rebuilt;
  return AB_PROF_SNES_M7;
}

/* FAITHFUL reconstruction from the capture (snes_linepix widened + geometry
 * from snes_frameinfo), no Mode 7 re-projection, no substitution. This is
 * the corpus-certification path: it must be pixel-identical to the pure-Lua
 * reference bezel, the same contract ab_nes/ab_gb/ab_md hold on their
 * platforms. */
int ab_prof_snes_draw(double ox, double oy, double scale,
                      ab_prof_snes_draw_result *r, const char **err) {
  *err = NULL;
  snes_retire_pending();
  ab_region_read(G_snes.r_finfo, 0, G_snes.fi, 8);
  int width = G_snes.fi[0] | (G_snes.fi[1] << 8);
  int lines = G_snes.fi[2] | (G_snes.fi[3] << 8);
  /* hi-res/interlace carts reach 512x480; the capture buffer holds them */
  if (width == 0 || lines == 0 || width > 512 || lines > 480)
    return 0;
  /* Read the FULL capture plane, exactly like the pure-Lua oracle does.
   * Reading only lines*1024 leaves rows from a PREVIOUS, taller frame in
   * the tail of this buffer -- and the Mode 7 tick() path shares it, so a
   * res change between entry points sampled stale rows. */
  ab_region_read(G_snes.r_lpx, 0, G_snes.lpx, 480 * 1024);
  size_t need = (size_t)width * lines * 4;
  if (need > sizeof(G_snes.lpx_rgba)) {
    if (!G_snes.lpx_big || G_snes.lpx_big_size < need) {
      free(G_snes.lpx_big);
      G_snes.lpx_big = (uint8_t *)malloc(need);
      G_snes.lpx_big_size = G_snes.lpx_big ? need : 0;
      if (!G_snes.lpx_big) { *err = "snes: frame buffer alloc failed"; return 0; }
    }
  }
  uint8_t *dst = need > sizeof(G_snes.lpx_rgba) ? G_snes.lpx_big : G_snes.lpx_rgba;
  for (int y = 0; y < lines; y++)
    ab_snes_565_row(G_snes.lpx + y * 1024, dst + (size_t)y * width * 4, width);
  snes_defer_free(G_snes.tex_lpx);
  G_snes.tex_lpx = ab_texture_create_rgba(dst, width, lines);
  ab_draw_texture_rect(G_snes.tex_lpx, ox, oy, width * scale, lines * scale,
                       0, 0, width, lines);
  r->w = width;
  r->h = lines;
  r->quads = 1;
  return 1;
}

int ab_prof_snes_frame_size(int *w, int *h) {
  ab_region_read(G_snes.r_finfo, 0, G_snes.fi, 8);
  int width = G_snes.fi[0] | (G_snes.fi[1] << 8);
  int lines = G_snes.fi[2] | (G_snes.fi[3] << 8);
  if (width == 0 || lines == 0) return 0;
  *w = width;
  *h = lines;
  return 1;
}

void ab_prof_snes_shutdown(void) {
  free(G_snes.lpx_big);  G_snes.lpx_big = NULL;  G_snes.lpx_big_size = 0;
  free(G_snes.plane);    G_snes.plane = NULL;
  free(G_snes.ov_rgba);  G_snes.ov_rgba = NULL;
  free(G_snes.ov_idx);   G_snes.ov_idx = NULL;
  free(G_snes.hd_blob);  G_snes.hd_blob = NULL;
  memset(G_snes.hd, 0, sizeof(G_snes.hd));
  G_snes.hd_any = G_snes.hd_any_rgba = G_snes.hd_any_idx = 0;
  G_snes.vram_valid = G_snes.cgram_valid = 0;
  G_snes.tex_lpx = G_snes.tex_pal = G_snes.tex_plane = 0;
  G_snes.tex_ov_rgba = G_snes.tex_ov_idx = 0;
  G_snes.pending_n = 0;
  G_snes.bound = 0;
}
