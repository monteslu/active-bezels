# HD text: replacing a game's 8-bit font

An enhancement recipe, not a runtime feature. It needs nothing that the
prebuilt runtimes do not already have: `ab.draw_text`, a TTF asset, and the
tilemap region a bezel can already read.

Retro games draw text as 8x8 tiles. Scaled to 1080p those are chunky
blocks, and reading them is measurably more work than reading clean type.
This replaces them with real letterforms while keeping the game's own
layout exactly.

Before and after, from the NES RPG bezel this recipe came out of:
`The wild imp's Hit Points have been reduced by 1.` in 8x8 tiles
versus the same line in stretched Roboto Mono at the same size.

## The whole idea: substitute per TILE, never per string

Each frame, walk the tilemap. For every cell holding a font tile, mask it
and print one glyph in its place. Nothing in the layer understands words,
sentences or layout — and that is the point. Because a glyph lands
exactly where its tile was:

- the typewriter reveal just works (the game adds tiles, you follow)
- menu columns, centered titles and indentation are automatically right
- a selection cursor still points at what it pointed at
- window slides and scrolling need no special handling beyond passing
  the right tilemap coordinates

Word-level reflow was tried and reverted. Tightening the spacing inside
words reads beautifully in dialogue and *breaks menus*, whose column
positions carry meaning. Per-cell is not a compromise, it is the model.

```lua
-- the entire renderer, minus the charset table
for _, cell in ipairs(cells) do          -- pass 1: mask EVERY cell first
  ab.fill_rect(cell.x, cell.y, CELL, CELL, bg)
end
for _, cell in ipairs(cells) do          -- pass 2: then print
  local adv = ab.measure(font, cell.glyph, PX)
  ab.draw_text(font, cell.glyph, cell.x + (CELL - adv) // 2, cell.y + BASELINE,
           PX, color)
end
```

Two passes, not one: lowercase descenders reach into the row below, and a
single loop erases the tail of the line above on consecutive dialogue
rows.

## The font must be stretched

This is the part that makes or breaks it. Tile cells are square; every
text font is roughly 1.2x taller than wide. Drop an ordinary font in and
the letters float in their cells with gaps that dissolve word shapes —
which is *worse* to read than the blocks you replaced.

So bake a horizontal stretch into the TTF itself, offline. The runtime
then renders an ordinary font with no tricks:

```python
# tools/make_tile_font.py — fontTools transform over glyf + hmtx
from fontTools.ttLib import TTFont
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.transformPen import TransformPen
from fontTools.misc.transform import Transform

f = TTFont(src)
gs, glyf, hmtx = f.getGlyphSet(), f['glyf'], f['hmtx']
for name in f.getGlyphOrder():
    pen = TTGlyphPen(gs)
    gs[name].draw(TransformPen(pen, Transform(K, 0, 0, 1, 0, 0)))
    glyf[name] = pen.glyph()
for name in f.getGlyphOrder():
    aw, lsb = hmtx[name]
    hmtx[name] = (round(aw * K), round(lsb * K))   # advances too, or it clips
f.save(dst)
```

Two constraints fight here, and it is worth knowing which wins. An 8-bit
glyph inks about 7 of its 8 pixels, so you want ink at ~0.875 of the cell.
But fonts carry sidebearings the tiles never had, so stretching the *ink*
that far pushes the *advance* past the cell pitch and neighbouring glyphs
collide — the symptom is an `i` that looks glued to the `m` beside it.

**The advance constraint wins.** Overlap is a visible bug; slightly
narrower ink is not. `make_tile_font.py` derives K from the ink target and
then clamps it, reporting when the clamp bound. For Roboto Mono Bold that
lands on K=1.46: advance 0.875 em (28px of a 32px cell), which leaves
exactly the gap that reads as separate letters.

Choose a face with **plain stick `i` and `l`**. Monospace designs flag
those with serifs to fill their advance; stretched, the flags collide.
Roboto Mono Bold at `K=1.46` won a five-font bake-off against JetBrains
Mono, IBM Plex Mono, Overpass Mono and Martian Mono on exactly this.

Never use a pixel font. Re-rendering pixels as pixels defeats the purpose.

**Judge candidates by eye, in cells, before wiring anything.** Render each
stretched candidate into a per-cell specimen PNG with real game strings
(`A slim imp limps in!` exercises `i`/`l`/`m` adjacency well):

```
python3 tools/make_tile_font.py RobotoMono-Bold.ttf tilefont.ttf \
        --cell 32 --specimen preview.png
```

## Decoding the charset

Per game you supply one table: tile id to string. Find text you can see
on screen, dump the tilemap, and align:

```
row 23: 61 37 11 0e 5f 20 12 15 0d 5f 12 16 19
        |  T  h  e     w  i  l  d     i  m  p
```

The RPG this came out of fell out in twenty minutes: digits `$00-$09`,
lowercase `$0A-$23`, uppercase `$24-$3D`, space `$5F`.

**Verify punctuation by rendering the tiles, not by guessing.** This is
where charsets bite. Dump the id range at 10x and look at it. That same
game packs `.` plus a closing quote into one tile (`$52`) but has a
plain apostrophe two ids away (`$53`) — guessing produced `imp,'s` in
live battle text. Some tiles also lie: its `$4C` is drawn as a slanted
stroke that reads as `/`, but the game only ever uses it as `!`.

Map only ids you have verified. Unmapped tiles keep their original art,
so cursors and arrows stay themselves and unknown punctuation degrades
gracefully instead of turning into garbage.

## Getting color right

Text color is the tile's attribute palette line, **color index 1**,
resolved live through the palette and the core's RGB table. "Brightest
color in the line" is a tempting shortcut that renders white text pink.

Reading it live also means palette animation and per-region palette swaps
follow for free.

## Scroll and screen mapping

The caller owns the cell-to-tilemap mapping, which is where per-platform
differences live:

- **Fixed HUD** (fixed top-strip adventure games): read the nametable directly, but track the
  game's whole-frame scroll variable so the layer rides menu slides.
- **Free-scrolling** (the RPG overworld): world position is base nametable
  origin + scroll + screen position, resolved through the live mirroring
  table. Skip frames with fine scroll — text only appears at rest.

One more artifact worth knowing: during a dialogue scroll-up a game may
hold the same line in two adjacent rows for a frame or two (copy up, then
clear). If a row's tiles are identical to the row above it, print only the
upper copy — otherwise the line visibly doubles.

## Cost

Per-cell drawing is cheap. Scanning a full tilemap in script each frame is
the only real cost and it is small: the source bezel composes in
~6.5ms per frame at 61fps with text substitution, HD monster art and a
full overworld minimap all running.

## Worked example

`examples/hd-text/` is a self-contained bezel: charset table, two-pass
renderer, the stretched font, and the font-baking script.
