/*
 * ab_render.h -- GPU-native tile/sprite renderer shared by every Active Bezel
 * runtime (Lua / JS / Python / Ruby).
 *
 * WHY THIS EXISTS (measured, NES RPG bezel session 2026-08-02)
 *   The exact NES reconstruction is right (1229/1229 carts) but cost ~14ms of
 *   guest time per frame while the host compositor sat at 3.2ms: the GPU was
 *   idle while the guest melted. The cost is inherent -- a scripting language
 *   assembling a 256x224 RGBA blob is ~57k string/table operations a frame.
 *
 * THE RULE
 *   Stop producing pixels in the guest. Emit GPU work.
 *   - Character/tile graphics upload ONCE as a texture atlas.
 *   - Drawing a layer = ONE ab_mesh() of textured quads (ab_vertex carries
 *     u,v and a texture handle, so this is a single host command per layer).
 *   - A substitution is therefore "point this quad at a different texture",
 *     not "erase the old pixels and paint over them".
 *
 * SUBSTITUTION GRANULARITY: per TILE and per SPRITE. Never per pixel.
 * The emulator already resolved which tile and which OAM sprite produced
 * what; scripts REGISTER what to swap and this code consults the registry
 * inside its emit loop. A per-sprite callback into the guest would
 * reintroduce exactly the FFI cliff this file exists to remove.
 *
 * WHAT LIVES HERE: platform-blind mechanics (atlas, quad batching, registry,
 * render targets). Per-console specifics (region schema, palette LUTs,
 * priority predicates) belong in a thin profile layer above -- see
 * the GPU renderer plan this kit was built from.
 *
 * BUDGET: 1ms of tick, tops, on a desktop. Above that, profile -- do not
 * rationalise.
 */
#ifndef AB_RENDER_H
#define AB_RENDER_H

#include <stdint.h>
#include <stddef.h>

/* --- atlas ---------------------------------------------------------------
 * A tile atlas is indexed 2bpp/4bpp character data expanded ONCE into an RGBA
 * texture, laid out as a grid of `tile_w` x `tile_h` cells. Palette is applied
 * at build time into `palettes` sub-rows: cell (tile, pal) lives at
 * (tile % cols, pal * tile_rows + tile / cols).
 *
 * Rebuild only when the source actually changes. Callers pass a signature
 * (CHR + palette bytes hashed however the profile likes); an unchanged
 * signature is a no-op and returns the existing handle. Mid-frame bank
 * switching is REAL (the originating RPG swaps sprite/BG banks), so this is
 * required, not an
 * optimisation.
 */
typedef struct {
  int32_t texture;     /* host texture handle, 0 if not built */
  int      tile_w, tile_h;
  int      cols, rows; /* grid of cells for ONE palette */
  int      palettes;   /* number of palette variants stacked vertically */
  uint64_t signature;  /* source hash; 0 forces a rebuild */
} ab_atlas;

/* Expand `chr` (2bpp NES-style planar, 16 bytes per 8x8 tile) into an RGBA
 * atlas of `tile_count` tiles x `palettes` palettes.
 *
 * `palette_rgba` is `palettes * 4` entries of 0xRRGGBBAA; entry 0 of each
 * palette is treated as TRANSPARENT (colour index 0 is the backdrop and must
 * not be painted, or behind-background priority breaks).
 *
 * Returns 1 if the atlas was (re)built, 0 if the signature matched and the
 * existing texture was kept, -1 on failure.
 */
int ab_atlas_build_2bpp(ab_atlas *atlas, const unsigned char *chr,
                        int tile_count, const uint32_t *palette_rgba,
                        int palettes, uint64_t signature);

void ab_atlas_free(ab_atlas *atlas);

/* --- quad batch ----------------------------------------------------------
 * Accumulate textured quads and submit them as ONE mesh command. The vertex
 * pool is reused across frames; ab_batch_reset() rewinds without freeing.
 *
 * All quads in a batch share a texture handle (that is what makes it one
 * command). Draw order is submission order.
 */
typedef struct ab_batch ab_batch;

ab_batch *ab_batch_new(int initial_quads);
void      ab_batch_free(ab_batch *b);
void      ab_batch_reset(ab_batch *b);

/* Add a quad drawing atlas cell (tile, palette) at logical (x, y) scaled by
 * `scale`. `flip` bit0 = horizontal, bit1 = vertical. `tint` multiplies the
 * sampled colour; pass 0xFFFFFFFF for none. */
void ab_batch_atlas_cell(ab_batch *b, const ab_atlas *atlas,
                         int tile, int palette,
                         double x, double y, double scale,
                         int flip, uint32_t tint);

/* Add an arbitrary textured quad (source rect in texture pixels). Used for
 * replacement art, which is already a GPU texture. */
void ab_batch_quad(ab_batch *b, double dx, double dy, double dw, double dh,
                   int tex_w, int tex_h,
                   int sx, int sy, int sw, int sh, uint32_t tint);

/* Add a flat colour quad (no texture). Flush these with handle 0, which the
 * host draws from vertex colour -- the run-coalesced layer path. */
void ab_batch_solid(ab_batch *b, double x, double y, double w, double h,
                    uint32_t rgba);

/* Submit everything accumulated. Returns the number of quads drawn. */
int ab_batch_flush(ab_batch *b, int32_t texture);

/* --- substitution registry ----------------------------------------------
 * Scripts register intent; the emit loop consults this. Declarative on
 * purpose: no guest callback per sprite.
 *
 * A sprite rule matches by TILE ID. The HD-monster case drove the shape:
 *   - every tile in `tiles` is SUPPRESSED (the original never renders),
 *   - bounds for anchoring the replacement come only from tiles NOT in
 *     `anchor_exclude` -- shadow/filler tiles ($FE/$FF there) must be
 *     suppressed but must not stretch the art.
 */
enum { AB_SUB_MAX_TILES = 64 };

typedef struct {
  int      id;                       /* caller's handle, >0 */
  int      tiles[AB_SUB_MAX_TILES];
  int      tile_count;
  int      anchor_exclude[AB_SUB_MAX_TILES];
  int      exclude_count;
  int32_t  texture;                  /* replacement art */
  int      tex_w, tex_h;
  double   ring;                     /* transparent overhang, source px */
  int      base_w, base_h;           /* footprint the art was cut for */
  int      active;
} ab_sub_rule;

typedef struct ab_registry ab_registry;

ab_registry *ab_registry_new(void);
void         ab_registry_free(ab_registry *r);
void         ab_registry_clear(ab_registry *r);

/* Returns the rule id, or 0 on failure. */
int  ab_registry_add_sprite(ab_registry *r, const ab_sub_rule *rule);
int  ab_registry_remove(ab_registry *r, int id);

/* Is this tile suppressed by any active rule? Returns the rule, or NULL.
 * O(rules * tiles) with tiny constants; a 256-entry lookup table is built
 * internally so this is O(1) in practice. */
const ab_sub_rule *ab_registry_match_tile(const ab_registry *r, int tile);

/* Should this tile contribute to the anchor bounds of `rule`? */
int ab_registry_tile_anchors(const ab_sub_rule *rule, int tile);

/* --- render targets ------------------------------------------------------
 * A bezel-AUTHORED texture (a minimap: a whole 120x120 overworld
 * stitched once and cached, then GPU-scaled at draw). Same primitive as
 * substitution underneath -- draw an atlas cell into a target -- which is the
 * sign the abstraction is right.
 *
 * Building one is a CPU expand today (there is no host render-to-texture
 * import yet); the win is that it happens ONCE and the per-frame cost is a
 * single draw. When a host-side target lands, only this call changes.
 */
typedef struct {
  int32_t texture;
  int     w, h;
} ab_target;

/* Compose `count` tiles into an RGBA buffer and upload it as one texture.
 * `placements` is count * 4 ints: {tile, palette, x, y} in target pixels. */
/* `placements` is count * 4 ints: {tile, palette, x, y} in target pixels.
 * Composes from the SOURCE character data (there is no host texture
 * read-back, so sampling the atlas would silently yield a blank target). */
int  ab_target_build(ab_target *t, const ab_atlas *atlas,
                     const unsigned char *chr, const uint32_t *palette_rgba,
                     const int *placements, int count, int w, int h);
void ab_target_free(ab_target *t);

/* --- bulk region read ----------------------------------------------------
 * The host exposes only region_read_u8 today, so this loops. It is isolated
 * here so that when a host-side memcpy import lands (the GB agent named it
 * the single thing they most wanted) exactly one function changes.
 * Returns bytes read.
 */
int ab_region_slurp(int32_t region, int32_t offset, unsigned char *dst, int len);

#endif /* AB_RENDER_H */
