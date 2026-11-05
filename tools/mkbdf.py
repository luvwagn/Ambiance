"""Build an Amiga diskfont from a BDF bitmap font (Galmuri, for the laced
native screens: 640 x 400/512).

Usage: python3 -I mkbdf.py <font.bdf> <outdir> <name> [--bold]

The BDF's glyphs are copied pixel for pixel: they are drawn for screens,
so nothing is scaled or rendered. Each glyph keeps its own bearing and
advance (the Amiga moves the pen by charKern, draws, then by charSpace).
--bold thickens every stroke a pixel to the right and widens the advance
by one, as the Amiga's own algorithmic bold does (for sizes the font has
no bold of). Writes <outdir>/<name>.font and <outdir>/<name>/<ysize>.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from amigafont import contents_file, size_file, parse_size_file  # noqa: E402

LO, HI = 32, 255


def read_bdf(path):
    glyphs, asc, desc = {}, 0, 0
    cur = bm = None
    with open(path, encoding="latin-1") as fh:
        for line in fh:
            t = line.split()
            if not t:
                continue
            if t[0] == "FONT_ASCENT":
                asc = int(t[1])
            elif t[0] == "FONT_DESCENT":
                desc = int(t[1])
            elif t[0] == "ENCODING":
                cur = int(t[1])
            elif t[0] == "DWIDTH":
                dw = int(t[1])
            elif t[0] == "BBX":
                bbx = [int(v) for v in t[1:5]]
            elif t[0] == "BITMAP":
                bm = []
            elif t[0] == "ENDCHAR":
                w, h, ox, oy = bbx
                bits = ((w + 7) // 8) * 8
                rows = ["".join("#" if int(r, 16) & (1 << (bits - 1 - x)) else "."
                                for x in range(w)) for r in bm]
                glyphs[cur] = (dw, w, h, ox, oy, rows)
                bm = None
            elif bm is not None:
                bm.append(t[0].ljust(2, "0"))
    return glyphs, asc, desc


def main():
    bdf, outdir, name = sys.argv[1], sys.argv[2], sys.argv[3]
    bold = "--bold" in sys.argv[4:]
    glyphs, asc, desc = read_bdf(bdf)
    ysize, baseline = asc + desc, asc - 1
    entries = []
    for c in list(range(LO, HI + 1)) + [None]:
        g = glyphs.get(c) if c is not None else None
        if c is not None and (127 <= c < 160 or not g):
            entries.append(None)                # not in the font: the not-defined glyph
            continue
        if c is None:                           # the not-defined glyph: a box
            n = glyphs[ord("n")]
            box_w = max(n[0] - 2, 3)
            cap = glyphs[ord("H")][2]
            rows = []
            for y in range(ysize):
                if y < baseline + 1 - cap or y > baseline:
                    rows.append("." * box_w)
                elif y in (baseline + 1 - cap, baseline):
                    rows.append("#" * box_w)
                else:
                    rows.append("#" + "." * (box_w - 2) + "#")
            entries.append((rows, box_w, box_w + 1, 1))
            continue
        dw, w, h, ox, oy, rows = g
        if not w or not h:
            entries.append(([], 0, dw + (1 if bold and c != 32 else 0), 0))
            continue
        if bold:
            rows = ["".join("#" if r[x:x + 1] == "#" or (x > 0 and r[x - 1] == "#") else "."
                            for x in range(w + 1)) for r in rows]
            w, dw = w + 1, dw + 1
        top = asc - (oy + h)                    # its first row in the font's rows
        full = ["." * w] * top + rows
        full = (full + ["." * w] * ysize)[:ysize]
        entries.append((full, w, dw - ox, ox))
    notdef = entries[-1]
    entries = [e if e is not None else notdef for e in entries[:-1]] + [notdef]

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
        "ysize": ysize, "baseline": baseline, "xsize": glyphs[ord("n")][0] + (1 if bold else 0),
        "modulo": modulo,
        "chardata": b"".join(bytes(r) for r in bitmap),
        "charloc": b"".join(struct.pack(">HH", o, w) for o, w in locs),
        "charspace": b"".join(struct.pack(">h", e[2]) for e in entries),
        "charkern": b"".join(struct.pack(">h", e[3]) for e in entries),
    }
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
