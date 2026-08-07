#include <string.h>
#include <stdint.h>

#include "../../sdk/active_bezel.h"
#include "ab_batteries.h"
#include "ab_panel.h"
#include "ab_panel_font.h"

/* Laid out on the 1920x1080 logical canvas the ABI guarantees. */
#define PANEL_W   1920.0
#define PANEL_H    540.0            /* the top HALF: unmissable, and leaves
                                     * the game visible below so you can see
                                     * WHEN it broke */
#define MARGIN      64.0
#define TITLE_PX    52
#define BODY_PX     34
#define HINT_PX     26
#define LINE_GAP     8.0

static int32_t g_panel_font = 0;

static int32_t panel_font(void) {
  if (!g_panel_font) {
    const char *err = NULL;
    g_panel_font = ab_bat_font_load_bytes(ab_panel_font,
                                          (int)ab_panel_font_len, &err);
  }
  return g_panel_font;
}

/* Draw `text` wrapped to `max_w`, returning the y after the last line. The
 * message is the part that matters and it is frequently long (a Lua trace
 * with a file name and a line number), so wrapping rather than clipping is
 * the difference between a usable panel and a teaser. */
static double wrapped(int32_t font, const char *text, double x, double y,
                      double max_w, int32_t px, uint32_t rgba) {
  const char *err = NULL;
  double ascent = 0, descent = 0, line_h = 0;
  ab_bat_font_metrics(font, px, &ascent, &descent, &line_h, &err);
  if (line_h <= 0) line_h = px * 1.3;

  const char *p = text;
  while (*p) {
    /* Longest prefix that fits, broken at a space where possible. */
    int len = 0, last_space = -1;
    while (p[len]) {
      if (p[len] == ' ') last_space = len;
      double w = ab_bat_font_measure(font, p, len + 1, px, &err);
      if (w > max_w) break;
      len++;
    }
    if (!p[len]) {
      ab_bat_font_print(font, p, len, x, y + ascent, px, rgba, &err);
      return y + line_h;
    }
    int cut = (last_space > 0) ? last_space : len;
    if (cut <= 0) cut = 1;                    /* always make progress */
    ab_bat_font_print(font, p, cut, x, y + ascent, px, rgba, &err);
    y += line_h;
    p += cut;
    while (*p == ' ') p++;
  }
  return y;
}

void ab_error_panel(const char *lang, const char *message, const char *hint) {
  const int32_t font = panel_font();

  /* The game still runs underneath: seeing it keeps the failure in context
   * (which frame, which screen) instead of replacing the world with a wall
   * of text. */
  ab_clear(0x000000ffu);
  ab_draw_game_fit(0, 0.5, 0.80, 0);

  /* Opaque backing. Text drawn straight over a bright game is unreadable at
   * exactly the moment it matters most. */
  ab_fill_rect(0, 0, PANEL_W, PANEL_H, 0x000000f2u);
  ab_fill_rect(0, PANEL_H - 4, PANEL_W, 4, 0xff4040ffu);

  if (!font) {
    /* No font: fall back to the bitmap rather than showing nothing. */
    ab_text("bezel error (no panel font)", MARGIN, 60, 40, 0xff8080ffu);
    ab_text(message, MARGIN, 120, 28, 0xffd0d0ffu);
    return;
  }

  const char *err = NULL;
  char title[64];
  int n = 0;
  while (lang[n] && n < 40) { title[n] = lang[n]; n++; }
  const char *suffix = " bezel error";
  for (int i = 0; suffix[i] && n < 60; i++) title[n++] = suffix[i];
  title[n] = 0;

  double ascent = 0, descent = 0, line_h = 0;
  ab_bat_font_metrics(font, TITLE_PX, &ascent, &descent, &line_h, &err);
  double y = MARGIN;
  ab_bat_font_print(font, title, (int)strlen(title), MARGIN, y + ascent,
                    TITLE_PX, 0xff5555ffu, &err);
  y += (line_h > 0 ? line_h : TITLE_PX * 1.3) + LINE_GAP * 2;

  y = wrapped(font, message, MARGIN, y, PANEL_W - MARGIN * 2,
              BODY_PX, 0xffffffffu);
  y += LINE_GAP * 2;

  wrapped(font, hint, MARGIN, y, PANEL_W - MARGIN * 2, HINT_PX, 0x9aa4c0ffu);
}
