/*
 * ab_batteries.c -- see ab_batteries.h.
 *
 * The stb single-headers are compiled here, once, with their asserts stubbed:
 * emscripten's assert() writes to stderr, which drags fd_write/fd_seek/
 * fd_close WASI imports into the wasm and breaks the "imports only ab_host"
 * contract every runtime is built to hold.
 */
#include <stdlib.h>
#include <string.h>

#include "ab_batteries.h"
#include "../../sdk/active_bezel.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ASSERT(x) ((void)0)
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((void)0)
#include "stb_truetype.h"

/* --- assets -------------------------------------------------------------- */

unsigned char *ab_bat_asset_slurp(const char *name, int name_len,
                                  int *out_len, const char **out_err) {
  int32_t size = ab_asset_size_raw(name, name_len);
  if (size < 0) { if (out_err) *out_err = "no such asset"; return NULL; }
  unsigned char *bytes = (unsigned char *)malloc((size_t)size + 1);
  if (!bytes) { if (out_err) *out_err = "out of memory"; return NULL; }
  int32_t got = ab_asset_read_raw(name, name_len, bytes, size);
  if (got < 0) { free(bytes); if (out_err) *out_err = "asset read failed"; return NULL; }
  bytes[got] = 0;                      /* NUL so parsers can treat it as text */
  if (out_len) *out_len = (int)got;
  return bytes;
}

/* --- images -------------------------------------------------------------- */

int ab_bat_image_from_memory(const unsigned char *bytes, int len,
                             int32_t *out_texture, int *out_w, int *out_h,
                             const char **out_err) {
  int w = 0, h = 0, comp = 0;
  unsigned char *pixels = stbi_load_from_memory(bytes, len, &w, &h, &comp, 4);
  if (!pixels) { if (out_err) *out_err = stbi_failure_reason(); return 0; }
  int32_t texture = ab_texture_create_rgba(pixels, w, h);
  stbi_image_free(pixels);
  if (texture <= 0) { if (out_err) *out_err = "texture_create failed"; return 0; }
  if (out_texture) *out_texture = texture;
  if (out_w) *out_w = w;
  if (out_h) *out_h = h;
  return 1;
}

int ab_bat_image_from_asset(const char *name, int name_len,
                            int32_t *out_texture, int *out_w, int *out_h,
                            const char **out_err) {
  int len = 0;
  unsigned char *bytes = ab_bat_asset_slurp(name, name_len, &len, out_err);
  if (!bytes) return 0;
  int ok = ab_bat_image_from_memory(bytes, len, out_texture, out_w, out_h, out_err);
  free(bytes);
  return ok;
}

/* --- fonts ---------------------------------------------------------------
 * One WHITE atlas per (font, integer size); print emits a textured mesh and
 * the vertex colour does the tinting.
 */
#define FONT_MAX 8
#define FONT_SIZE_CACHE 16
#define ATLAS_PX 1024

typedef struct {
  int32_t px;
  int32_t texture;
  float scale;                 /* stbtt scale for this size, for metrics */
  stbtt_bakedchar baked[96];
} FontAtlas;

typedef struct {
  unsigned char *bytes;
  stbtt_fontinfo info;
  int ascent, descent, line_gap;   /* unscaled font units */
  FontAtlas sizes[FONT_SIZE_CACHE];
  int32_t used;
} Font;

static Font g_fonts[FONT_MAX];
static int32_t g_font_count = 0;

static Font *font_at(int32_t handle) {
  if (handle < 1 || handle > g_font_count) return NULL;
  Font *f = &g_fonts[handle - 1];
  return f->bytes ? f : NULL;
}

static FontAtlas *font_atlas(Font *font, int32_t px) {
  if (px < 6) px = 6;
  if (px > 256) px = 256;
  for (int32_t i = 0; i < font->used; i++)
    if (font->sizes[i].px == px) return &font->sizes[i];

  FontAtlas *slot;
  if (font->used < FONT_SIZE_CACHE) {
    slot = &font->sizes[font->used++];
  } else {
    /* Recycle the oldest slot -- but do NOT destroy its texture here: glyph
     * meshes emitted EARLIER THIS FRAME may still reference it, and the
     * compositor resolves handles at compose time. Destroying mid-frame
     * turned every already-drawn string into solid boxes (dead handle ==
     * colour-only quads). The stale texture leaks until shutdown; a bezel
     * cycling >16 live sizes per font is the pathological case and pays
     * with memory, not with corrupted text. */
    slot = &font->sizes[0];
  }

  unsigned char *alpha = (unsigned char *)malloc(ATLAS_PX * ATLAS_PX);
  unsigned char *rgba = (unsigned char *)malloc((size_t)ATLAS_PX * ATLAS_PX * 4);
  if (!alpha || !rgba) { free(alpha); free(rgba); return NULL; }
  stbtt_BakeFontBitmap(font->bytes, 0, (float)px, alpha, ATLAS_PX, ATLAS_PX, 32, 96, slot->baked);
  for (int32_t i = 0; i < ATLAS_PX * ATLAS_PX; i++) {
    rgba[i * 4 + 0] = 255; rgba[i * 4 + 1] = 255;
    rgba[i * 4 + 2] = 255; rgba[i * 4 + 3] = alpha[i];
  }
  slot->px = px;
  slot->scale = stbtt_ScaleForPixelHeight(&font->info, (float)px);
  slot->texture = ab_texture_create_rgba(rgba, ATLAS_PX, ATLAS_PX);
  free(alpha); free(rgba);
  return slot->texture > 0 ? slot : NULL;
}

int32_t ab_bat_font_load(const char *name, int name_len, const char **out_err) {
  if (g_font_count >= FONT_MAX) { if (out_err) *out_err = "too many fonts"; return 0; }
  int len = 0;
  unsigned char *bytes = ab_bat_asset_slurp(name, name_len, &len, out_err);
  if (!bytes) return 0;
  Font *font = &g_fonts[g_font_count];
  font->bytes = bytes;
  if (!stbtt_InitFont(&font->info, font->bytes, 0)) {
    free(font->bytes); font->bytes = NULL;
    if (out_err) *out_err = "not a TrueType font";
    return 0;
  }
  stbtt_GetFontVMetrics(&font->info, &font->ascent, &font->descent, &font->line_gap);
  font->used = 0;
  return ++g_font_count;               /* handles are 1-based */
}

double ab_bat_font_print(int32_t handle, const char *text, int len,
                         double x, double y, int32_t px, uint32_t rgba,
                         const char **out_err) {
  Font *font = font_at(handle);
  if (!font) { if (out_err) *out_err = "bad font handle"; return x; }
  FontAtlas *atlas = font_atlas(font, px);
  if (!atlas) { if (out_err) *out_err = "atlas bake failed"; return x; }
  if (len <= 0) return x;

  ab_vertex *verts = (ab_vertex *)malloc(sizeof(ab_vertex) * 6 * (size_t)len);
  if (!verts) { if (out_err) *out_err = "out of memory"; return x; }
  float fx = (float)x, fy = (float)y;
  int32_t count = 0;
  for (int i = 0; i < len; i++) {
    unsigned char c = (unsigned char)text[i];
    if (c < 32 || c > 126) c = '?';
    stbtt_aligned_quad q;
    stbtt_GetBakedQuad(atlas->baked, ATLAS_PX, ATLAS_PX, c - 32, &fx, &fy, &q, 1);
    /* ab_vertex is {x, y, u, v, rgba} -- positions, then UVs, then colour. */
    ab_vertex tl = { q.x0, q.y0, q.s0, q.t0, rgba };
    ab_vertex tr = { q.x1, q.y0, q.s1, q.t0, rgba };
    ab_vertex bl = { q.x0, q.y1, q.s0, q.t1, rgba };
    ab_vertex br = { q.x1, q.y1, q.s1, q.t1, rgba };
    verts[count * 6 + 0] = tl; verts[count * 6 + 1] = tr; verts[count * 6 + 2] = bl;
    verts[count * 6 + 3] = bl; verts[count * 6 + 4] = tr; verts[count * 6 + 5] = br;
    count++;
  }
  ab_mesh(verts, count * 6, atlas->texture);
  free(verts);
  return (double)fx;
}

double ab_bat_font_measure(int32_t handle, const char *text, int len,
                           int32_t px, const char **out_err) {
  Font *font = font_at(handle);
  if (!font) { if (out_err) *out_err = "bad font handle"; return 0; }
  FontAtlas *atlas = font_atlas(font, px);
  if (!atlas) { if (out_err) *out_err = "atlas bake failed"; return 0; }
  float fx = 0, fy = 0;
  for (int i = 0; i < len; i++) {
    unsigned char c = (unsigned char)text[i];
    if (c < 32 || c > 126) c = '?';
    stbtt_aligned_quad q;
    stbtt_GetBakedQuad(atlas->baked, ATLAS_PX, ATLAS_PX, c - 32, &fx, &fy, &q, 1);
  }
  return (double)fx;
}

int ab_bat_font_metrics(int32_t handle, int32_t px,
                        double *out_ascent, double *out_descent,
                        double *out_line_height, const char **out_err) {
  Font *font = font_at(handle);
  if (!font) { if (out_err) *out_err = "bad font handle"; return 0; }
  FontAtlas *atlas = font_atlas(font, px);
  if (!atlas) { if (out_err) *out_err = "atlas bake failed"; return 0; }
  double s = (double)atlas->scale;
  if (out_ascent) *out_ascent = font->ascent * s;
  if (out_descent) *out_descent = -font->descent * s;      /* positive below */
  if (out_line_height) *out_line_height = (font->ascent - font->descent + font->line_gap) * s;
  return 1;
}

void ab_bat_shutdown(void) {
  for (int32_t i = 0; i < g_font_count; i++) {
    for (int32_t s = 0; s < g_fonts[i].used; s++)
      if (g_fonts[i].sizes[s].texture > 0) ab_texture_destroy(g_fonts[i].sizes[s].texture);
    free(g_fonts[i].bytes);
    g_fonts[i].bytes = NULL;
    g_fonts[i].used = 0;
  }
  g_font_count = 0;
}

/* --- misc ---------------------------------------------------------------- */

uint32_t ab_bat_read_uint(int32_t region, int32_t offset, int bytes, int big_endian) {
  if (bytes < 1) bytes = 1;
  if (bytes > 4) bytes = 4;
  uint32_t value = 0;
  for (int i = 0; i < bytes; i++) {
    int32_t v = ab_region_read_u8(region, offset + i);
    if (v < 0) v = 0;
    value |= (uint32_t)(v & 0xff) << (8 * (big_endian ? bytes - 1 - i : i));
  }
  return value;
}
