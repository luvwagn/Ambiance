#!/usr/bin/env python3
"""Smooth-style artwork for AmigaMCE's RTG screens, one file per screen height.

Usage: python3 tools/mksmooth.py <Rubik-VF.ttf> <out dir> [heights...]

The layout is designed for a 256-line screen. An RTG screen of H lines draws
it H/RTG_LINES times larger (RTG_LINES = 384: a 1024x768 screen shows it at
2x, like a 384-line screen would). For each H this pre-renders, at exactly
that size and
with anti-aliased edges:

  - three text faces from Rubik: body (regular), bold (medium, for the menu
    and titles) and small (regular, for the clock, counts and hints), Latin-1
  - the nine menu icons (vector shapes of tools/hiicons.py)
  - the five list row markers (vector shapes of tools/markicons.py)
  - the logo, in full colour, reduced to a small palette

Glyphs, icons and markers are coverage maps, 4 bits a pixel (0 = none, 15 =
full); AmigaMCE blends them between the text colour and the background with
in-between pens. The logo is a palette image; index 255 is transparent.

File layout (big-endian), written to <out dir>/<H>:
  "AMSM", u16 version (1), u16 k (scale x 256), u16 cell_h, u16 baseline
  u16 nfaces (3), then per face:
      u16 lo, u16 hi, then per glyph lo..hi:
          u8 advance, s8 xoff, s8 yoff (from the cell top), u8 w, u8 h, u8 0, u32 offset
      u32 bytes, then the face's coverage data (each glyph byte aligned)
  u16 icon size, u16 count, then count icons (each byte aligned)
  u16 mark size, u16 count, then count markers (each byte aligned)
  u16 logo w, u16 logo h, u16 colours, colours x (r, g, b), w x h indices
"""
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hiicons      # noqa: E402
import markicons    # noqa: E402
from mkart import UI    # noqa: E402

LO, HI = 32, 255
RTG_LINES = 384     # the screen height the design is scaled to fit as if it had
# (design size in 256-line pixels, weight) per face: body, bold, small
FACES = [(11.0, 400), (11.5, 500), (10.0, 400)]
CELL = 12           # design height of a text cell (the pixel font's)
BASE = 9.6          # design baseline, from the cell top
ICONS = hiicons.ICONS
MARKS = markicons.ORDER
BAR, WHITE, HL, ACCENT, NAVY = UI[1], UI[5], UI[6], UI[8], UI[0]


def nibbles(values):
    """Pack 0..15 values two to a byte, high nibble first."""
    out = bytearray((len(values) + 1) // 2)
    for i, v in enumerate(values):
        out[i >> 1] |= (v << 4) if not (i & 1) else v
    return bytes(out)


def cov(img):
    """L image -> list of 0..15 (rounded, so faint fringes vanish)."""
    data = img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata()
    return [min(15, (v * 15 + 127) // 255) for v in data]


def face(ttf, size, wght, k, cell_h, base):
    font = ImageFont.truetype(ttf, max(1, round(size * k)))
    font.set_variation_by_axes([wght])
    glyphs, blob = [], bytearray()
    pad = cell_h
    for c in range(LO, HI + 1):
        ch = bytes([c]).decode("latin-1")
        adv = round(font.getlength(ch)) if c not in range(0x7F, 0xA0) else 0
        if c in range(0x7F, 0xA0) or c in (32, 160):
            glyphs.append((min(255, adv), 0, 0, 0, 0, 0))
            continue
        img = Image.new("L", (adv + 2 * pad, cell_h + 2 * pad), 0)
        ImageDraw.Draw(img).text((pad, pad + base), ch, font=font, fill=255, anchor="ls")
        box = img.point(lambda v: 255 if v >= 9 else 0).getbbox()   # ignore sub-1/16 fringes
        if not box:
            glyphs.append((min(255, adv), 0, 0, 0, 0, 0))
            continue
        x0, y0, x1, y1 = box
        g = img.crop(box)
        off = len(blob)
        blob += nibbles(cov(g))
        glyphs.append((min(255, adv), x0 - pad, y0 - pad, x1 - x0, y1 - y0, off))
    return glyphs, bytes(blob)


def shape(draw_fn, units, n, centre=True):
    """Draw a vector shape on a units x units design box (16 canvas units a
    design pixel), centre it horizontally on its ink, scale to n x n."""
    S = 16
    big = Image.new("L", (units * S, units * S), 0)
    draw_fn(ImageDraw.Draw(big))
    if centre:
        box = big.getbbox()
        if box:
            shift = (units * S - (box[2] - box[0])) // 2 - box[0]
            moved = Image.new("L", big.size, 0)
            moved.paste(big, (shift, 0))
            big = moved
    return big.resize((n, n), Image.LANCZOS)


def logo(ttf, k):
    """The logo drawn smooth at scale k, as (w, h, palette, indices)."""
    def f(size, wght):
        ft = ImageFont.truetype(ttf, round(size * k))
        ft.set_variation_by_axes([wght])
        return ft
    fa, fm = f(14, 700), f(11, 700)
    probe = ImageDraw.Draw(Image.new("RGB", (1, 1)))
    ax = 17 * k
    bx = ax + probe.textlength("Amiga", font=fa) + 3 * k
    tw = probe.textlength("MCE", font=fm)
    w, h = int(bx + tw + 6 * k) + 1, round(16 * k)
    ss = 4                                   # supersample for the shapes
    img = Image.new("RGB", (w * ss, h * ss), BAR)
    d = ImageDraw.Draw(img)
    K = k * ss
    d.rounded_rectangle([0, 1 * K, 14 * K, 15 * K], radius=2.2 * K, fill=HL)
    d.polygon([(4.5 * K, 4.5 * K), (10.8 * K, 8 * K), (4.5 * K, 11.5 * K)], fill=WHITE)
    d.rounded_rectangle([bx * ss, 1 * K, (bx + tw + 6 * k) * ss, 15 * K], radius=1.8 * K, fill=ACCENT)
    img = img.resize((w, h), Image.LANCZOS)
    d = ImageDraw.Draw(img)
    d.text((ax, 13.4 * k), "Amiga", font=fa, fill=WHITE, anchor="ls")
    d.text((bx + 3 * k, 12.5 * k), "MCE", font=fm, fill=NAVY, anchor="ls")
    q = img.quantize(colors=24, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()[:24 * 3]
    colours = [tuple(pal[i:i + 3]) for i in range(0, len(pal), 3)]
    idx = list(q.get_flattened_data() if hasattr(q, "get_flattened_data") else q.getdata())
    # the bar colour (and anything within a step of it) is left undrawn
    near_bar = {i for i, c in enumerate(colours) if sum(abs(c[j] - BAR[j]) for j in range(3)) <= 6}
    idx = [255 if v in near_bar else v for v in idx]
    used = sorted(set(v for v in idx if v != 255))
    remap = {v: i for i, v in enumerate(used)}
    idx = [255 if v == 255 else remap[v] for v in idx]
    return w, h, [colours[v] for v in used], bytes(idx)


def build(ttf, H):
    k256 = round(H * 256 / RTG_LINES)
    k = k256 / 256
    cell_h, base = round(CELL * k), round(BASE * k)
    out = bytearray(b"AMSM")
    out += struct.pack(">HHHHH", 1, k256, cell_h, base, len(FACES))
    for size, wght in FACES:
        glyphs, blob = face(ttf, size, wght, k, cell_h, base)
        out += struct.pack(">HH", LO, HI)
        for adv, xo, yo, w, h, off in glyphs:
            out += struct.pack(">BbbBBBI", adv, xo, yo, w, h, 0, off)
        out += struct.pack(">I", len(blob)) + blob
    n = round(16 * k)
    out += struct.pack(">HH", n, len(ICONS))
    for fn in ICONS:
        out += nibbles(cov(shape(fn, 16, n)))
    m = round(markicons.SIZE * k)
    out += struct.pack(">HH", m, len(MARKS))
    for name in MARKS:
        out += nibbles(cov(shape(lambda d, nm=name: markicons._hires_draw(nm, d),
                                 markicons.SIZE, m, centre=False)))
    lw, lh, pal, idx = logo(ttf, k)
    out += struct.pack(">HHH", lw, lh, len(pal))
    for c in pal:
        out += bytes(c)
    out += idx
    return bytes(out)


def main():
    ttf, outdir = sys.argv[1], sys.argv[2]
    heights = [int(v) for v in sys.argv[3:]] or [480, 600, 768]
    os.makedirs(outdir, exist_ok=True)
    for H in heights:
        data = build(ttf, H)
        with open(os.path.join(outdir, str(H)), "wb") as fh:
            fh.write(data)
        print(f"  SMOOTH {H} lines: scale {round(H * 256 / RTG_LINES) / 256:.2f}, {len(data)} bytes")


if __name__ == "__main__":
    main()
