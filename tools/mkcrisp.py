"""Build amce-crisp: Rubik drawn as 1-bit pixels at one size, as an Amiga
diskfont, for the laced native screens (640 x 400/512).

Usage: python3 -I mkcrisp.py <Rubik-VF.ttf> <outdir> [px] [weight]

Pixelify Sans (amce-pixel) is an outline font on a pixel grid: it is only
clean at its own size and whole multiples, too small or too large for the
laced screens. Rubik (the smooth style's font) is hinted, and FreeType's
monochrome rendering at 14 px gives clean strokes. Each glyph keeps its
own advance and left bearing (charSpace, charKern), so spacing is the
font's. Writes <outdir>/amce-crisp.font and <outdir>/amce-crisp/<ysize>.
"""
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from amigafont import contents_file, size_file, parse_size_file  # noqa: E402

LO, HI = 32, 255
CODES = [c for c in range(LO, HI + 1) if not 127 <= c < 160]


def raster(font, ch):
    w, h, base, pen = 80, 60, 40, 20
    img = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(img)
    d.fontmode = "1"
    d.text((pen, base), ch, font=font, fill=255, anchor="ls")
    adv = int(round(font.getlength(ch)))
    box = img.getbbox()
    if box is None:
        return None, adv
    x0, y0, x1, y1 = box
    rows = ["".join("#" if img.getpixel((x, y)) else "." for x in range(x0, x1)) for y in range(y0, y1)]
    return (y0 - base, x0 - pen, rows), adv


def main():
    ttf, outdir = sys.argv[1], sys.argv[2]
    px = float(sys.argv[3]) if len(sys.argv) > 3 else 14
    weight = int(sys.argv[4]) if len(sys.argv) > 4 else 400
    font = ImageFont.truetype(ttf, px)
    try:
        font.set_variation_by_axes([weight])
    except OSError:
        pass
    glyphs = {}
    for c in CODES:
        glyphs[c] = raster(font, chr(c))
    inked = [g for g, a in glyphs.values() if g]
    top = min(t for t, l, r in inked)
    bottom = max(t + len(r) for t, l, r in inked)
    ysize = bottom - top
    baseline = -top - 1
    entries = []
    for c in range(LO, HI + 1):
        g, adv = glyphs.get(c, (None, 0))
        if not g:
            entries.append(([], 0, adv, 0))
            continue
        gt, left, rows = g
        w = max(len(r) for r in rows)
        rows = [r.ljust(w, ".") for r in rows]
        full = ["." * w] * (gt - top) + rows
        full += ["." * w] * (ysize - len(full))
        entries.append((full, w, adv, left))
    # the not-defined glyph: a box from the cap height to the baseline
    n_adv = entries[ord("n") - LO][2]
    box_w = max(n_adv - 2, 3)
    cap_row = glyphs[ord("H")][0][0] - top
    box = []
    for y in range(ysize):
        if y < cap_row or y > baseline:
            box.append("." * box_w)
        elif y in (cap_row, baseline):
            box.append("#" * box_w)
        else:
            box.append("#" + "." * (box_w - 2) + "#")
    entries.append((box, box_w, box_w + 2, 1))

    total = sum(e[1] for e in entries)
    modulo = ((total + 15) // 16) * 2
    bitmap = [bytearray(modulo) for _ in range(ysize)]
    locs, bit = [], 0
    for rows, width, space, kern in entries:
        locs.append((bit, width))
        for y, r in enumerate(rows):
            for x, ch in enumerate(r):
                if ch == "#":
                    p = bit + x
                    bitmap[y][p >> 3] |= 0x80 >> (p & 7)
        bit += width
    v = {
        "ysize": ysize, "baseline": baseline, "xsize": n_adv, "modulo": modulo,
        "chardata": b"".join(bytes(r) for r in bitmap),
        "charloc": b"".join(struct.pack(">HH", o, w) for o, w in locs),
        "charspace": b"".join(struct.pack(">h", e[2]) for e in entries),
        "charkern": b"".join(struct.pack(">h", e[3]) for e in entries),
    }
    name = "amce-crisp"
    os.makedirs(os.path.join(outdir, name), exist_ok=True)
    data = size_file(name + ".font", v)
    with open(os.path.join(outdir, name, str(ysize)), "wb") as fh:
        fh.write(data)
    with open(os.path.join(outdir, name + ".font"), "wb") as fh:
        fh.write(contents_file(name, [ysize]))
    f = parse_size_file(data)
    print(f"{name}/{ysize}: baseline {f['baseline']} xsize {f['xsize']} modulo {f['modulo']} "
          f"chars {f['lo']}-{f['hi']} file {len(data)} bytes")


if __name__ == "__main__":
    main()
