/*
 * Image decoding for shader lookup textures, host-side.
 *
 * A `.glslp` preset can reference LUT images -- phosphor masks, dither
 * patterns, bezel overlays, palettes. Over 200 of the presets RetroArch ships
 * need at least one, so without a decoder they are unavailable.
 *
 * This is the SAME stb_image the four language runtimes already link for
 * ab.image(), built as a standalone module the host can call. Guests decode
 * inside their own wasm; the host had no way to decode anything, and rather
 * than add a second implementation (or a native dependency) this reuses the
 * decoder already vendored in runtimes/common.
 *
 * Output is 8-bit RGBA, top row first, whatever the input format -- stb
 * normalises palette, grayscale, 16-bit and JPEG for us.
 */
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
/* Formats worth carrying: PNG covers the LUTs, JPEG appears in a handful of
 * overlay presets, TGA/BMP are cheap. The rest is dead weight. */
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ASSERT(x) ((void)0)
#include "stb_image.h"

static unsigned char *g_pixels;
static int g_w, g_h;

__attribute__((export_name("ab_img_alloc")))
void *ab_img_alloc(int size) { return malloc((size_t)size); }

__attribute__((export_name("ab_img_free")))
void ab_img_free(void *p) { free(p); }

/* 1 on success; dimensions and pixels come from the accessors below, since a
 * wasm export returns a single scalar. */
__attribute__((export_name("ab_img_decode")))
int ab_img_decode(const unsigned char *data, int len) {
  if (g_pixels) { stbi_image_free(g_pixels); g_pixels = NULL; }
  g_w = g_h = 0;
  int comp = 0;
  g_pixels = stbi_load_from_memory(data, len, &g_w, &g_h, &comp, 4);
  if (!g_pixels) { g_w = g_h = 0; return 0; }
  return 1;
}

__attribute__((export_name("ab_img_width")))  int ab_img_width(void)  { return g_w; }
__attribute__((export_name("ab_img_height"))) int ab_img_height(void) { return g_h; }
__attribute__((export_name("ab_img_pixels"))) void *ab_img_pixels(void) { return g_pixels; }
