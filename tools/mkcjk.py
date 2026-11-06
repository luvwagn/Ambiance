"""Build AmigaMCE's Far East glyph packs (Fonts/amce-cjk/<name>).

Usage: python3 -I mkcjk.py <galmuri-dist> <ark-pixel-font> <outdir>

  12   Galmuri11 (kana, Hangul, the JIS X 0208 kanji, CJK punctuation,
       full-width forms), with the Chinese characters it lacks (GB2312's,
       Big5's common ones) from Ark Pixel 12px, simplified forms
  15   Galmuri14; what it lacks comes from 12
  (Galmuri11 Bold's own Hangul is left out: all of a name's characters in
  one weight, and the release floppy has no room for it.)
  In 15 Hangul is KS X 1001's 2,350 syllables (nearly all that is
  ever written); the other 8,822 come from 12, at its size.

The glyphs are copied pixel for pixel, never scaled. Every glyph of a pack
sits in one cell (cell_w x cell_h, its top `asc` rows above the baseline),
a row a 16-bit word, its bearing kept in the bits.

The file, big-endian (the 68k's own):
  "ACJK", UWORD version 1, UBYTE cell_w, cell_h, asc, 0, ULONG count,
  count UWORDs (the code points, sorted), count UBYTEs (advances),
  a pad byte if count is odd, then count x cell_h UWORDs (the glyphs).

Only code points in the ranges AmigaMCE keeps as Far East text are taken
(src/cjk.c: cjk_wanted, keep in step).
"""
import glob
import os
import struct
import sys

from PIL import Image

RANGES = [(0x1100, 0x11FF), (0x3000, 0x30FF), (0x3130, 0x318F), (0x31F0, 0x31FF),
          (0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0xAC00, 0xD7A3), (0xFF00, 0xFFEF)]


def wanted(c):
    return any(a <= c <= b for a, b in RANGES)


def read_bdf(path):
    """{code: (advance, xoff, yoff, w, h, rows as ints, w bits each)}"""
    glyphs = {}
    enc = dw = bbx = None
    rows = None
    with open(path, encoding="latin-1") as fh:
        for line in fh:
            t = line.split()
            if not t:
                continue
            if t[0] == "ENCODING":
                enc = int(t[1])
            elif t[0] == "DWIDTH":
                dw = int(t[1])
            elif t[0] == "BBX":
                bbx = [int(v) for v in t[1:5]]
            elif t[0] == "BITMAP":
                rows = []
            elif t[0] == "ENDCHAR":
                if enc is not None and wanted(enc):
                    w, h, xo, yo = bbx
                    nb = len(rows[0]) * 4 if rows else 0
                    glyphs[enc] = (dw, xo, yo, w, h, [int(r, 16) >> (nb - w) if nb else 0
                                                      for r in rows])
                rows = None
                enc = None
            elif rows is not None:
                rows.append(t[0])
    return glyphs


def chinese_wanted():
    """GB2312's hanzi and Big5's common ones (level 1)."""
    s = set()
    for b1 in range(0xB0, 0xF8):
        for b2 in range(0xA1, 0xFF):
            try:
                s.add(ord(bytes([b1, b2]).decode("gb2312")))
            except UnicodeDecodeError:
                pass
    for b1 in range(0xA4, 0xC7):
        for b2 in list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)):
            try:
                s.add(ord(bytes([b1, b2]).decode("big5")))
            except UnicodeDecodeError:
                pass
    return {c for c in s if 0x4E00 <= c <= 0x9FFF}


def ksx1001_hangul():
    s = set()
    for b1 in range(0xB0, 0xC9):
        for b2 in range(0xA1, 0xFF):
            try:
                s.add(ord(bytes([b1, b2]).decode("euc_kr")))
            except UnicodeDecodeError:
                pass
    return s


def common_hangul_only(glyphs):
    ks = ksx1001_hangul()
    return {c: g for c, g in glyphs.items() if not (0xAC00 <= c <= 0xD7A3) or c in ks}


def read_ark(root, codes):
    """Ark Pixel 12px glyphs (12 x 12 PNGs, ink rows 1-11 = Galmuri11's
    11 rows above the baseline), default (simplified Chinese) forms."""
    glyphs = {}
    base = os.path.join(root, "assets/glyphs/12/cmap/common/4E00-9FFF CJK Unified Ideographs")
    for f in glob.glob(os.path.join(base, "*", "*.png")):
        name = os.path.basename(f)[:-4]
        if " " in name:
            continue                        # another language's form
        c = int(name, 16)
        if c not in codes:
            continue
        im = Image.open(f).convert("RGBA")
        px = im.load()
        w, h = im.size
        rows = []
        for y in range(1, h):
            v = 0
            for x in range(w):
                p = px[x, y]
                v = v << 1 | (1 if p[3] > 128 and p[0] < 128 else 0)
            rows.append(v)
        # as a BDF glyph: 12 wide, 11 rows, its bottom on the baseline
        glyphs[c] = (12, 0, 0, w, h - 1, rows)
    return glyphs


def write_pack(path, glyphs):
    codes = sorted(glyphs)
    asc = max(g[2] + g[4] for g in glyphs.values())
    desc = max(-g[2] for g in glyphs.values())
    desc = max(desc, 0)
    cell_h = asc + desc
    cell_w = max(g[1] + g[3] for g in glyphs.values())
    assert cell_w <= 16, cell_w
    out = bytearray(b"ACJK")
    out += struct.pack(">HBBBBL", 1, cell_w, cell_h, asc, 0, len(codes))
    for c in codes:
        out += struct.pack(">H", c)
    for c in codes:
        out.append(min(glyphs[c][0], 255))
    if len(codes) & 1:
        out.append(0)
    for c in codes:
        dw, xo, yo, w, h, rows = glyphs[c]
        cell = [0] * cell_h
        top = asc - (yo + h)                # the glyph's first row in the cell
        for i, r in enumerate(rows):
            y = top + i
            if 0 <= y < cell_h:
                cell[y] = (r << (16 - w - xo)) & 0xFFFF if xo >= 0 else (r << (16 - w)) & 0xFFFF
        for v in cell:
            out += struct.pack(">H", v)
    with open(path, "wb") as fh:
        fh.write(out)
    return len(codes), cell_w, cell_h, asc, len(out)


def main():
    gdist, ark, outdir = sys.argv[1:4]
    os.makedirs(outdir, exist_ok=True)
    g11 = read_bdf(os.path.join(gdist, "Galmuri11.bdf"))
    g14 = read_bdf(os.path.join(gdist, "Galmuri14.bdf"))
    zh = chinese_wanted() - set(g11)
    ak = read_ark(ark, zh)
    p12 = dict(ak)
    p12.update(g11)
    for name, glyphs in (("12", p12), ("15", common_hangul_only(g14))):
        n, cw, ch, asc, size = write_pack(os.path.join(outdir, name), glyphs)
        print("%s: %d glyphs, cell %d x %d (%d above the baseline), %d bytes" %
              (name, n, cw, ch, asc, size))
    print("Chinese from Ark Pixel: %d (of %d wanted)" % (len(ak), len(zh)))


if __name__ == "__main__":
    main()
