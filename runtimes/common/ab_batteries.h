/*
 * ab_batteries.h -- language-agnostic services every Active Bezel runtime
 * needs, so a new language binding is only the binding.
 *
 * WHAT LIVES HERE
 *   - image decode (PNG/JPG/GIF/BMP via stb_image) straight to a texture
 *   - TrueType text (stb_truetype): one WHITE atlas per (font, size), drawn
 *     as a textured mesh whose vertex colour carries the tint, so a colour
 *     costs nothing and a frame of text is a couple of draw calls
 *   - the WASI-elimination stubs every interpreter needs to import only
 *     ab_host (no env, no wasi_snapshot_preview1)
 *
 * WHAT DOES NOT
 *   Anything that touches a specific interpreter's value types. Each
 *   runtime's binding file converts its own strings/numbers and calls these.
 *
 * Include ab_batteries.c ONCE from the runtime's translation unit (or link
 * it separately); it pulls in the stb implementations.
 */
#ifndef AB_BATTERIES_H
#define AB_BATTERIES_H

#include <stdint.h>
#include <stddef.h>

/* --- images --------------------------------------------------------------
 * Decode bytes to an RGBA texture. Returns 1 on success and fills out_*,
 * 0 on failure with *out_err pointing at a static reason string.
 */
int ab_bat_image_from_memory(const unsigned char *bytes, int len,
                             int32_t *out_texture, int *out_w, int *out_h,
                             const char **out_err);

/* Same, reading the bytes from a package asset by name. */
int ab_bat_image_from_asset(const char *name, int name_len,
                            int32_t *out_texture, int *out_w, int *out_h,
                            const char **out_err);

/* --- fonts ---------------------------------------------------------------
 * Handles are 1-based; 0 means failure (check *out_err).
 */
int32_t ab_bat_font_load(const char *name, int name_len, const char **out_err);

/* Load a font from BYTES the caller already holds, rather than from a
 * package asset.
 *
 * This exists for the runtimes' built-in error panel. That panel is what a
 * developer reads when the package is broken, so it cannot depend on the
 * package shipping a font -- and the 3x5 debug bitmap it used instead is
 * close to unreadable at 1080p, which is the worst possible property for
 * the one surface that only ever appears when something has gone wrong.
 * The bytes are NOT copied; they must outlive the font (a static array
 * does). */
int32_t ab_bat_font_load_bytes(const unsigned char *bytes, int len,
                               const char **out_err);

/* Draw `text` at (x, y) baseline in `px`, tinted `rgba`. Returns the pen X
 * after the last glyph, so callers can chain or right-align. */
double ab_bat_font_print(int32_t font, const char *text, int len,
                         double x, double y, int32_t px, uint32_t rgba,
                         const char **out_err);

/* Width of `text` at `px`, without drawing. */
double ab_bat_font_measure(int32_t font, const char *text, int len,
                           int32_t px, const char **out_err);

/* Vertical metrics at `px`: ascent above the baseline, descent below (both
 * positive), and the recommended line advance. Any out_* may be NULL. */
int ab_bat_font_metrics(int32_t font, int32_t px,
                        double *out_ascent, double *out_descent,
                        double *out_line_height, const char **out_err);

/* Release every font texture. Call from the runtime's ab_shutdown. */
void ab_bat_shutdown(void);

/* --- misc ----------------------------------------------------------------
 * Read `bytes` (1..4) from a region as an unsigned integer.
 * `big_endian` selects byte order.
 */
uint32_t ab_bat_read_uint(int32_t region, int32_t offset, int bytes, int big_endian);

/* Pack 0xRRGGBBAA the way every ab command wants it. */
static inline uint32_t ab_bat_rgba(int r, int g, int b, int a) {
  if (r < 0) r = 0; if (r > 255) r = 255;
  if (g < 0) g = 0; if (g > 255) g = 255;
  if (b < 0) b = 0; if (b > 255) b = 255;
  if (a < 0) a = 0; if (a > 255) a = 255;
  return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | (uint32_t)a;
}

/* Read a whole asset into malloc'd memory. Caller frees. Returns NULL and
 * sets *out_err on failure; *out_len gets the byte count on success. */
unsigned char *ab_bat_asset_slurp(const char *name, int name_len,
                                  int *out_len, const char **out_err);

#endif /* AB_BATTERIES_H */
