#!/usr/bin/env python3
"""Generate a Playdate .fnt + table PNG covering ASCII + Cyrillic from a TTF, so notes render
Russian text. Playdate font format: RGBA PNG laid out in a 16-column grid of fixed WxH cells,
ink = opaque black, background = transparent; the .fnt lists `tracking`, a blank line, then one
`<glyph>\t<advance>` line per cell in the same order (drawText decodes UTF-8, so a Cyrillic
char is one glyph).

Run: tools/.venv/bin/python tools/make_pd_font.py
"""
import os
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(__file__), "..", "playdate", "Source", "fonts")
NAME = "Notes-14"
SIZE = 16  # native pixel grid of haxrcorp4089 -> crisp, matches the Flipper look
TTF = os.path.join(os.path.dirname(__file__), "haxrcorp4089.ttf")  # same font as the Flipper (u8g2 haxrcorp4089)
COLS = 16

# glyph set: ASCII printable, common punctuation, Cyrillic (Ё ё + А..я)
chars = [chr(c) for c in range(0x20, 0x7F)]
chars += ["—", "–", "«", "»", "“", "”", "…", "№"]  # — – « » " " … №
chars += ["Ё", "ё"]  # Ё ё
chars += [chr(c) for c in range(0x410, 0x450)]  # А..я


def main():
    font = ImageFont.truetype(TTF, SIZE)
    ascent, descent = font.getmetrics()
    cell_h = ascent + descent

    # glyphs missing from this TTF render as a .notdef box; substitute a readable ASCII glyph
    FALLBACK = {"\u0451": "\u0435", "\u0401": "\u0415", "\u2014": "-", "\u2013": "-",
                "\u00ab": "<", "\u00bb": ">", "\u201c": '"', "\u201d": '"',
                "\u2026": ".", "\u2116": "N"}
    DET_W = 32
    def det_bits(ch):  # render into a fixed cell for a size-independent .notdef comparison
        c = Image.new("L", (DET_W, cell_h), 0)
        ImageDraw.Draw(c).text((0, 0), ch, fill=255, font=font)
        return c.tobytes()
    ND = det_bits("\ue000")

    def eff(ch):  # effective render char: fall back when this codepoint is missing
        if det_bits(ch) == ND:
            return FALLBACK.get(ch, " ")
        return ch

    # advance widths come from the *rendered* glyph, so a fallback keeps its own spacing
    adv, cell_w = {}, 0
    for ch in chars:
        w = max(1, int(round(font.getlength(eff(ch)))))
        adv[ch] = w
        cell_w = max(cell_w, w)
    cell_w += 1

    rows = (len(chars) + COLS - 1) // COLS
    img = Image.new("RGBA", (COLS * cell_w, rows * cell_h), (0, 0, 0, 0))
    for i, ch in enumerate(chars):
        cx = (i % COLS) * cell_w
        cy = (i // COLS) * cell_h
        cell = Image.new("L", (cell_w, cell_h), 0)
        ImageDraw.Draw(cell).text((0, 0), eff(ch), fill=255, font=font)
        px = cell.load()
        for y in range(cell_h):
            for x in range(cell_w):
                if px[x, y] >= 128:
                    img.putpixel((cx + x, cy + y), (0, 0, 0, 255))

    os.makedirs(OUT, exist_ok=True)
    png = os.path.join(OUT, f"{NAME}-table-{cell_w}-{cell_h}.png")
    img.save(png)

    fnt = os.path.join(OUT, f"{NAME}.fnt")
    with open(fnt, "w", encoding="utf-8") as f:
        f.write("tracking=1\n\n")
        for ch in chars:
            key = "space" if ch == " " else ch
            f.write(f"{key}\t{adv[ch]}\n")
    print(f"wrote {png} ({COLS*cell_w}x{rows*cell_h}, cell {cell_w}x{cell_h}) and {fnt}, {len(chars)} glyphs")


if __name__ == "__main__":
    main()
