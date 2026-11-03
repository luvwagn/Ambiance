"""Rasterise Pixelify Sans Regular at its native 10px into 1-bit glyphs.

Usage: python3 -I extract.py <ttf> <out.json>
Writes {code: {"rows": [...strings of '#'/'.'...], "top": int, "left": int}}
where `top` is the ink's first row relative to the baseline (negative = above)
and `left` is the ink's x offset from the pen position.
"""
import json
import sys

from PIL import Image, ImageDraw, ImageFont

SIZE = 10.9  # native: one design pixel (about 92 units) = one screen pixel
CODES = list(range(32, 127)) + list(range(160, 256))


def glyph(font, ch):
    w, h, base, pen = 60, 40, 28, 10
    img = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(img)
    d.fontmode = "L"
    d.text((pen, base), ch, font=font, fill=255, anchor="ls")
    img = img.point(lambda v: 255 if v >= 128 else 0)
    box = img.getbbox()
    adv = font.getlength(ch)
    if box is None:
        return {"rows": [], "top": 0, "left": 0, "adv": adv}
    x0, y0, x1, y1 = box
    rows = []
    for y in range(y0, y1):
        rows.append("".join("#" if img.getpixel((x, y)) else "." for x in range(x0, x1)))
    return {"rows": rows, "top": y0 - base, "left": x0 - pen, "adv": adv}


def main():
    ttf, out = sys.argv[1], sys.argv[2]
    font = ImageFont.truetype(ttf, SIZE)
    data = {c: glyph(font, chr(c)) for c in CODES}
    with open(out, "w") as fh:
        json.dump(data, fh)


if __name__ == "__main__":
    main()
