#!/usr/bin/env python3
"""Bake a horizontally stretched font for tile-cell text substitution.

Retro text cells are square; text fonts are ~1.2x taller than wide, so an
unstretched glyph floats in its cell and the gaps dissolve word shapes.
Stretching in the FONT FILE keeps the runtime trivial: it renders an
ordinary TTF, no atlas and no per-draw transform.

    python3 make_tile_font.py RobotoMono-Bold.ttf tilefont.ttf --cell 32

K is derived so capitals fill the cell like the original tiles, then
CLAMPED so the advance stays inside the cell pitch: over 1.0 every glyph
bleeds into its neighbour (the "why does the i overlap the m" bug). The
clamp usually binds, because most faces carry sidebearings the tiles did
not. Pass --scale to force a factor.

Choose a face with plain stick i/l: monospace designs flag those with
serifs to fill the advance, and stretched, the flags collide. Roboto Mono
Bold at K=1.46 won a five-font bake-off for exactly this reason.

--specimen writes a PNG of real game strings laid out one glyph per cell,
which is the only honest way to judge a candidate. Look at it before
wiring anything up.
"""
import argparse

from fontTools.ttLib import TTFont
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.transformPen import TransformPen
from fontTools.misc.transform import Transform

SPECIMEN_LINES = [
    "A slim imp limps in!",      # i/l/m adjacency, the usual failure
    "Command?  HP  120",         # digits and punctuation
    "'Descendant of legends,'",  # quotes, comma, descenders
]


def stretch(src, dst, k):
    font = TTFont(src)
    glyphset = font.getGlyphSet()
    glyf, hmtx = font["glyf"], font["hmtx"]

    outlines = {}
    for name in font.getGlyphOrder():
        pen = TTGlyphPen(glyphset)
        try:
            glyphset[name].draw(TransformPen(pen, Transform(k, 0, 0, 1, 0, 0)))
        except Exception:
            continue          # composites/empties: leave as-is
        outlines[name] = pen.glyph()
    for name, glyph in outlines.items():
        glyf[name] = glyph

    # advances too, or the stretched ink overflows its own advance box
    for name in font.getGlyphOrder():
        adv, lsb = hmtx[name]
        hmtx[name] = (int(round(adv * k)), int(round(lsb * k)))
    font["hhea"].advanceWidthMax = int(round(font["hhea"].advanceWidthMax * k))

    font.save(dst)


def scale_for_cell(src, fill, max_advance):
    """K to fill the cell with ink, clamped to keep the advance inside it.

    Two constraints, and they fight: an 8-bit glyph inks ~7 of its 8 px
    (fill=0.875), but fonts add sidebearings, so stretching the INK that
    far pushes the ADVANCE past the cell pitch and neighbouring glyphs
    collide. The advance constraint wins -- overlap is a visible bug,
    slightly narrower ink is not.

    Returns (k, clamped).
    """
    from fontTools.pens.boundsPen import BoundsPen

    font = TTFont(src)
    upm = font["head"].unitsPerEm
    glyphset = font.getGlyphSet()

    pen = BoundsPen(glyphset)
    glyphset["H"].draw(pen)
    ink = (pen.bounds[2] - pen.bounds[0]) / upm       # xMax - xMin
    advance = font["hmtx"]["H"][0] / upm

    k_ink = fill / ink
    k_adv = max_advance / advance
    if k_ink <= k_adv:
        return k_ink, False
    return k_adv, True


def specimen(path, out, cell):
    from PIL import Image, ImageDraw, ImageFont

    cap_target = int(cell * 0.875)          # 7 of 8 px, like the tiles
    probe = ImageFont.truetype(path, 100)
    box = probe.getbbox("H")
    size = int(round(100 * cap_target / (box[3] - box[1])))
    font = ImageFont.truetype(path, size)
    cap_top = font.getbbox("H")[1]

    width = cell * max(len(s) for s in SPECIMEN_LINES)
    img = Image.new("RGB", (width, len(SPECIMEN_LINES) * (cell + 8) + 8),
                    (8, 8, 12))
    draw = ImageDraw.Draw(img)
    y = 6
    for line in SPECIMEN_LINES:
        for i, ch in enumerate(line):
            if ch == " ":
                continue
            adv = font.getlength(ch)
            draw.text((i * cell + (cell - adv) // 2, y - cap_top),
                      ch, font=font, fill=(255, 255, 255))
        y += cell + 8
    img.save(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--cell", type=int, default=32,
                    help="on-screen cell pitch in px (informational)")
    ap.add_argument("--fill", type=float, default=0.875,
                    help="capital ink width as a fraction of the cell; "
                         "0.875 matches an 8-bit font's 7-of-8 pixels")
    ap.add_argument("--max-advance", type=float, default=0.875,
                    help="advance as a fraction of the cell. 0.875 is the "
                         "validated value (28px of a 32px cell): it leaves "
                         "the gap that reads as separate letters. Anything "
                         "at or above 1.0 makes glyphs collide")
    ap.add_argument("--scale", type=float,
                    help="force K instead of deriving it")
    ap.add_argument("--specimen", help="also write a per-cell preview PNG")
    args = ap.parse_args()

    clamped = False
    if args.scale:
        k = args.scale
    else:
        k, clamped = scale_for_cell(args.src, args.fill, args.max_advance)
    stretch(args.src, args.dst, k)

    check = TTFont(args.dst)
    upm = check["head"].unitsPerEm
    from fontTools.pens.boundsPen import BoundsPen
    pen = BoundsPen(check.getGlyphSet())
    check.getGlyphSet()["H"].draw(pen)
    ink = (pen.bounds[2] - pen.bounds[0]) / upm
    adv = check["hmtx"]["H"][0] / upm
    print(f"wrote {args.dst}  K={k:.3f}  cap ink={ink:.3f} em  "
          f"advance={adv:.3f} em")
    if clamped:
        print(f"  advance-clamped: ink stops at {ink:.3f} rather than "
              f"{args.fill:.3f} em so glyphs cannot collide")
    if adv >= 1.0:
        print("  WARNING: advance >= the cell pitch; glyphs WILL bleed into "
              "their neighbours")

    if args.specimen:
        specimen(args.dst, args.specimen, args.cell)
        print(f"wrote {args.specimen} — judge it by eye before shipping")


if __name__ == "__main__":
    main()
