/*
 * ab_render.c -- see ab_render.h for the why. Platform-blind mechanics only.
 */
#include "ab_render.h"

#include <stdlib.h>
#include <string.h>

/* The host ABI. Runtimes include active_bezel.h before this in their TU; when
 * compiled standalone we declare just what we use. */
#ifndef AB_IMPORT
#define AB_IMPORT(module, name) __attribute__((import_module(module), import_name(name)))
typedef struct {
  float x, y;
  float u, v;
  uint32_t rgba;
  uint32_t _pad;
} ab_vertex;
AB_IMPORT("ab_host", "command_mesh") extern int32_t ab_mesh(const ab_vertex *, int32_t, int32_t);
AB_IMPORT("ab_host", "texture_create_rgba") extern int32_t ab_texture_create_rgba(const void *, int32_t, int32_t);
AB_IMPORT("ab_host", "texture_destroy") extern int32_t ab_texture_destroy(int32_t);
AB_IMPORT("ab_host", "region_read_u8") extern int32_t ab_region_read_u8(int32_t, int32_t);
AB_IMPORT("ab_host", "region_read") extern int32_t ab_region_read(int32_t, int32_t, void *, int32_t);
#endif

/* ------------------------------------------------------------------ atlas */

int ab_atlas_build_2bpp(ab_atlas *atlas, const unsigned char *chr,
                        int tile_count, const uint32_t *palette_rgba,
                        int palettes, uint64_t signature) {
  if (!atlas || !chr || !palette_rgba || tile_count <= 0 || palettes <= 0)
    return -1;

  /* Signature match: keep the existing texture. Bank switching makes this
   * load-bearing, not an optimisation -- but a zero signature always builds
   * so callers can force it. */
  if (atlas->texture && signature && atlas->signature == signature)
    return 0;

  const int cols = 16;
  const int rows = (tile_count + cols - 1) / cols;
  const int w    = cols * 8;
  const int h    = rows * 8 * palettes;

  uint32_t *px = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
  if (!px) return -1;

  for (int p = 0; p < palettes; p++) {
    const uint32_t *pal = palette_rgba + (size_t)p * 4;
    const int y_base = p * rows * 8;
    for (int t = 0; t < tile_count; t++) {
      const unsigned char *src = chr + (size_t)t * 16;
      const int cx = (t % cols) * 8;
      const int cy = y_base + (t / cols) * 8;
      for (int y = 0; y < 8; y++) {
        const unsigned lo = src[y];
        const unsigned hi = src[y + 8];
        uint32_t *row = px + (size_t)(cy + y) * w + cx;
        for (int x = 0; x < 8; x++) {
          const int bit = 7 - x;
          const int c = (int)(((lo >> bit) & 1u) | (((hi >> bit) & 1u) << 1));
          /* Colour 0 is the backdrop: leave it fully transparent. Painting it
           * breaks behind-background priority and hides real pixels. */
          row[x] = c ? pal[c] : 0u;
        }
      }
    }
  }

  if (atlas->texture) ab_texture_destroy(atlas->texture);
  atlas->texture   = ab_texture_create_rgba(px, w, h);
  atlas->tile_w    = 8;
  atlas->tile_h    = 8;
  atlas->cols      = cols;
  atlas->rows      = rows;
  atlas->palettes  = palettes;
  atlas->signature = signature;
  free(px);
  return atlas->texture ? 1 : -1;
}

void ab_atlas_free(ab_atlas *atlas) {
  if (!atlas) return;
  if (atlas->texture) ab_texture_destroy(atlas->texture);
  memset(atlas, 0, sizeof(*atlas));
}

/* ------------------------------------------------------------------ batch */

struct ab_batch {
  ab_vertex *v;
  int        n;      /* vertices used */
  int        cap;    /* vertices allocated */
};

ab_batch *ab_batch_new(int initial_quads) {
  ab_batch *b = (ab_batch *)calloc(1, sizeof(ab_batch));
  if (!b) return NULL;
  if (initial_quads < 64) initial_quads = 64;
  b->cap = initial_quads * 6;
  b->v = (ab_vertex *)malloc((size_t)b->cap * sizeof(ab_vertex));
  if (!b->v) { free(b); return NULL; }
  return b;
}

void ab_batch_free(ab_batch *b) {
  if (!b) return;
  free(b->v);
  free(b);
}

void ab_batch_reset(ab_batch *b) { if (b) b->n = 0; }

static int batch_room(ab_batch *b, int verts) {
  if (b->n + verts <= b->cap) return 1;
  int cap = b->cap ? b->cap * 2 : 384;
  while (cap < b->n + verts) cap *= 2;
  ab_vertex *nv = (ab_vertex *)realloc(b->v, (size_t)cap * sizeof(ab_vertex));
  if (!nv) return 0;
  b->v = nv;
  b->cap = cap;
  return 1;
}

static void push_quad(ab_batch *b, double x0, double y0, double x1, double y1,
                      float u0, float v0, float u1, float v1, uint32_t tint) {
  if (!batch_room(b, 6)) return;
  ab_vertex *q = b->v + b->n;
  const float fx0 = (float)x0, fy0 = (float)y0;
  const float fx1 = (float)x1, fy1 = (float)y1;
  q[0].x = fx0; q[0].y = fy0; q[0].u = u0; q[0].v = v0; q[0].rgba = tint; q[0]._pad = 0;
  q[1].x = fx1; q[1].y = fy0; q[1].u = u1; q[1].v = v0; q[1].rgba = tint; q[1]._pad = 0;
  q[2].x = fx1; q[2].y = fy1; q[2].u = u1; q[2].v = v1; q[2].rgba = tint; q[2]._pad = 0;
  q[3].x = fx0; q[3].y = fy0; q[3].u = u0; q[3].v = v0; q[3].rgba = tint; q[3]._pad = 0;
  q[4].x = fx1; q[4].y = fy1; q[4].u = u1; q[4].v = v1; q[4].rgba = tint; q[4]._pad = 0;
  q[5].x = fx0; q[5].y = fy1; q[5].u = u0; q[5].v = v1; q[5].rgba = tint; q[5]._pad = 0;
  b->n += 6;
}

void ab_batch_atlas_cell(ab_batch *b, const ab_atlas *atlas,
                         int tile, int palette,
                         double x, double y, double scale,
                         int flip, uint32_t tint) {
  if (!b || !atlas || !atlas->texture) return;
  if (palette < 0 || palette >= atlas->palettes) palette = 0;

  const int aw = atlas->cols * atlas->tile_w;
  const int ah = atlas->rows * atlas->tile_h * atlas->palettes;
  const int cx = (tile % atlas->cols) * atlas->tile_w;
  const int cy = (palette * atlas->rows + tile / atlas->cols) * atlas->tile_h;

  float u0 = (float)cx / (float)aw;
  float v0 = (float)cy / (float)ah;
  float u1 = (float)(cx + atlas->tile_w) / (float)aw;
  float v1 = (float)(cy + atlas->tile_h) / (float)ah;
  if (flip & 1) { float t = u0; u0 = u1; u1 = t; }
  if (flip & 2) { float t = v0; v0 = v1; v1 = t; }

  push_quad(b, x, y, x + atlas->tile_w * scale, y + atlas->tile_h * scale,
            u0, v0, u1, v1, tint);
}

void ab_batch_quad(ab_batch *b, double dx, double dy, double dw, double dh,
                   int tex_w, int tex_h,
                   int sx, int sy, int sw, int sh, uint32_t tint) {
  if (!b || tex_w <= 0 || tex_h <= 0) return;
  push_quad(b, dx, dy, dx + dw, dy + dh,
            (float)sx / (float)tex_w, (float)sy / (float)tex_h,
            (float)(sx + sw) / (float)tex_w, (float)(sy + sh) / (float)tex_h,
            tint);
}

void ab_batch_solid(ab_batch *b, double x, double y, double w, double h,
                    uint32_t rgba) {
  if (!b) return;
  /* UVs are ignored when the mesh is flushed with handle 0. */
  push_quad(b, x, y, x + w, y + h, 0.f, 0.f, 1.f, 1.f, rgba);
}

int ab_batch_flush(ab_batch *b, int32_t texture) {
  if (!b || b->n <= 0) return 0;
  ab_mesh(b->v, b->n, texture);
  const int quads = b->n / 6;
  b->n = 0;
  return quads;
}

/* --------------------------------------------------------------- registry */

enum { AB_REG_MAX_RULES = 64 };

struct ab_registry {
  ab_sub_rule rules[AB_REG_MAX_RULES];
  int         count;
  int         next_id;
  /* tile -> rule index + 1, so a match is one array read in the emit loop */
  unsigned char lookup[256];
};

ab_registry *ab_registry_new(void) {
  ab_registry *r = (ab_registry *)calloc(1, sizeof(ab_registry));
  if (r) r->next_id = 1;
  return r;
}

void ab_registry_free(ab_registry *r) { free(r); }

static void registry_reindex(ab_registry *r) {
  memset(r->lookup, 0, sizeof(r->lookup));
  for (int i = 0; i < r->count; i++) {
    if (!r->rules[i].active) continue;
    for (int t = 0; t < r->rules[i].tile_count; t++) {
      int tile = r->rules[i].tiles[t];
      if (tile >= 0 && tile < 256) r->lookup[tile] = (unsigned char)(i + 1);
    }
  }
}

void ab_registry_clear(ab_registry *r) {
  if (!r) return;
  r->count = 0;
  memset(r->lookup, 0, sizeof(r->lookup));
}

int ab_registry_add_sprite(ab_registry *r, const ab_sub_rule *rule) {
  if (!r || !rule || r->count >= AB_REG_MAX_RULES) return 0;
  if (rule->tile_count <= 0 || rule->tile_count > AB_SUB_MAX_TILES) return 0;
  ab_sub_rule *dst = &r->rules[r->count];
  *dst = *rule;
  dst->id = r->next_id++;
  dst->active = 1;
  r->count++;
  registry_reindex(r);
  return dst->id;
}

int ab_registry_remove(ab_registry *r, int id) {
  if (!r) return 0;
  for (int i = 0; i < r->count; i++) {
    if (r->rules[i].id == id) {
      for (int j = i; j < r->count - 1; j++) r->rules[j] = r->rules[j + 1];
      r->count--;
      registry_reindex(r);
      return 1;
    }
  }
  return 0;
}

const ab_sub_rule *ab_registry_match_tile(const ab_registry *r, int tile) {
  if (!r || tile < 0 || tile > 255) return NULL;
  const unsigned char idx = r->lookup[tile];
  if (!idx) return NULL;
  const ab_sub_rule *rule = &r->rules[idx - 1];
  return rule->active ? rule : NULL;
}

int ab_registry_tile_anchors(const ab_sub_rule *rule, int tile) {
  if (!rule) return 0;
  for (int i = 0; i < rule->exclude_count; i++)
    if (rule->anchor_exclude[i] == tile) return 0;
  return 1;
}

/* ---------------------------------------------------------------- targets */

int ab_target_build(ab_target *t, const ab_atlas *atlas,
                    const unsigned char *chr, const uint32_t *palette_rgba,
                    const int *placements, int count, int w, int h) {
  if (!t || !chr || !palette_rgba || !placements || w <= 0 || h <= 0) return -1;
  (void)atlas;   /* target composes from the SOURCE chr, not the GPU atlas:
                  * there is no host read-back, and a texture we cannot sample
                  * would silently produce a blank map. */

  uint32_t *px = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
  if (!px) return -1;

  for (int i = 0; i < count; i++) {
    const int tile = placements[i * 4 + 0];
    const int pal  = placements[i * 4 + 1];
    const int dx   = placements[i * 4 + 2];
    const int dy   = placements[i * 4 + 3];
    const unsigned char *src = chr + (size_t)tile * 16;
    const uint32_t *p = palette_rgba + (size_t)pal * 4;
    for (int y = 0; y < 8; y++) {
      const int ty = dy + y;
      if (ty < 0 || ty >= h) continue;
      const unsigned lo = src[y];
      const unsigned hi = src[y + 8];
      uint32_t *row = px + (size_t)ty * w;
      for (int x = 0; x < 8; x++) {
        const int tx = dx + x;
        if (tx < 0 || tx >= w) continue;
        const int bit = 7 - x;
        const int c = (int)(((lo >> bit) & 1u) | (((hi >> bit) & 1u) << 1));
        if (c) row[tx] = p[c];
      }
    }
  }

  if (t->texture) ab_texture_destroy(t->texture);
  t->texture = ab_texture_create_rgba(px, w, h);
  t->w = w;
  t->h = h;
  free(px);
  return t->texture ? 1 : -1;
}

void ab_target_free(ab_target *t) {
  if (!t) return;
  if (t->texture) ab_texture_destroy(t->texture);
  memset(t, 0, sizeof(*t));
}

/* ------------------------------------------------------------ bulk region */

int ab_region_slurp(int32_t region, int32_t offset, unsigned char *dst, int len) {
  if (!dst || len <= 0) return 0;

  /* Fast path: one host crossing for the whole span. Snapshotting the two
   * NES resolved planes was 122,880 per-byte calls a frame and dominated an
   * otherwise-C renderer. */
  const int got = ab_region_read(region, offset, dst, len);
  if (got == len) return got;

  /* Fallback for hosts without the import (it resolves to a stub returning
   * 0) and for short reads. Correct, just slower -- never silently wrong. */
  for (int i = got > 0 ? got : 0; i < len; i++)
    dst[i] = (unsigned char)ab_region_read_u8(region, offset + i);
  return len;
}
