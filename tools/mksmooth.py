#!/usr/bin/env python3
"""Smooth-style artwork for AmigaMCE's RTG screens, one file per screen height.

Usage: python3 tools/mksmooth.py <Rubik-VF.ttf> <out dir> [heights...] [--greek <Geologica-VF.ttf>]

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
  - the list overlay's big letters A-Z and '#', drawn from the outlines at
    exactly the size the overlay uses on this screen, one bit a pixel
  - with --greek: the three faces' Greek letters (U+0384-U+03CE), which
    Rubik lacks, from Geologica at Rubik's weights, scaled so its letters
    sit between Rubik's x-height and cap height

Glyphs, icons and markers are coverage maps, 4 bits a pixel (0 = none, 15 =
full); AmigaMCE blends them between the text colour and the background with
in-between pens. The logo is a palette image; index 255 is transparent.

File layout (big-endian), written to <out dir>/<H>:
  "AMSM", u16 version (3), u16 k (scale x 256), u16 cell_h, u16 baseline
  u16 nfaces (3), then per face:
      u16 lo, u16 hi, then per glyph lo..hi:
          u16 advance (1/16 px), s8 xoff, s8 yoff (from the cell top), u8 w, u8 h,
          u32 offset: PHASES coverage maps of w x h, for the glyph placed at
          0, 1/4, 1/2 and 3/4 of a pixel, each byte aligned
      u32 bytes, then the face's coverage data
  u16 icon size, u16 count, then count icons (each byte aligned)
  u16 mark size, u16 count, then count markers (each byte aligned)
  u16 logo w, u16 logo h, u16 colours, colours x (r, g, b), w x h indices
  u16 letters (27: A-Z, #), u16 cell_w (widest), then per letter:
      u16 w, u16 h, u32 bytes, then each row as run lengths (u8),
      alternately clear and set, starting with clear, summing to w; a run
      over 255 is written 255, 0, rest
  with --greek: "GREK", then per face u16 lo, u16 hi and its glyphs and
      coverage data as above (src/smooth.c reads a file with or without it)
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
LETTERS = [chr(c) for c in range(ord("A"), ord("Z") + 1)] + ["#"]
# (design size in 256-line pixels, weight) per face: body, bold, small
FACES = [(11.0, 400), (11.5, 500), (10.0, 400)]
CELL = 12           # design height of a text cell (the pixel font's)
BASE = 9.0          # design baseline, from the cell top (the pixel font's)
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


SS = 8              # oversampling: glyphs are drawn 8x and averaged down
PHASES = 4          # quarter-pixel horizontal positions each glyph is stored at


GREEK_LO, GREEK_HI = 0x384, 0x3CE    # tonos to omega with tonos: modern Greek
# Geologica's letters, scaled to sit with Rubik's: Rubik's x-height is 0.52
# of its size and its caps 0.70, Geologica's 0.48 and 0.70; between the two
GREEK_SCALE = 1.05


def face(ttf, size, wght, k, cell_h, base, codes=None, axes=None, scale=1.0):
    """Glyphs drawn unhinted (from 8x oversampling) at each quarter-pixel
    position, so text can be laid out at fractional advances: no snapped,
    uneven gaps at small sizes. Advances are in 1/16 pixel. codes: the code
    points (Latin-1's by default); a font without one gets an empty entry."""
    font = ImageFont.truetype(ttf, max(1, round(size * k * SS * scale)))
    font.set_variation_by_axes(axes(wght) if axes else [wght])
    have = None
    if codes is not None:
        from fontTools.ttLib import TTFont
        have = TTFont(ttf).getBestCmap()
    glyphs, blob = [], bytearray()
    pad = cell_h
    for c in (codes if codes is not None else range(LO, HI + 1)):
        ch = chr(c)
        if c in range(0x7F, 0xA0) or (have is not None and c not in have):
            glyphs.append((0, 0, 0, 0, 0, 0))
            continue
        adv = font.getlength(ch) / SS
        adv16 = round(adv * 16)
        if c in (32, 160):
            glyphs.append((adv16, 0, 0, 0, 0, 0))
            continue
        W, H = int(adv) + 2 * pad + 2, cell_h + 2 * pad
        phases = []
        for p in range(PHASES):
            big = Image.new("L", (W * SS, H * SS), 0)
            ImageDraw.Draw(big).text(((pad + p / PHASES) * SS, (pad + base) * SS), ch,
                                     font=font, fill=255, anchor="ls")
            phases.append(big.resize((W, H), Image.BOX))
        boxes = [im.point(lambda v: 255 if v >= 9 else 0).getbbox() for im in phases]
        boxes = [bx for bx in boxes if bx]
        if not boxes:
            glyphs.append((adv16, 0, 0, 0, 0, 0))
            continue
        x0 = min(bx[0] for bx in boxes)
        y0 = min(bx[1] for bx in boxes)
        x1 = max(bx[2] for bx in boxes)
        y1 = max(bx[3] for bx in boxes)
        off = len(blob)
        for im in phases:                   # each phase byte aligned, same box
            blob += nibbles(cov(im.crop((x0, y0, x1, y1))))
        glyphs.append((adv16, x0 - pad, y0 - pad, x1 - x0, y1 - y0, off))
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


def gu(v, k256):
    """GU() of amce.h: design units to pixels."""
    return (v * k256 + 128) >> 8


def overlay_ink(H, k256):
    """The overlay letter's ink height, as main.c's ov_build works it out:
    2/5 of the list's visible rows."""
    list_top, row_h = gu(44, k256), gu(14, k256)
    footer_y = H - gu(12, k256)
    vis = (footer_y - 2 - list_top) // row_h
    return vis * row_h * 2 // 5


def big_letters(ttf, ink_h):
    """The overlay letters, ink ink_h tall for a capital, thresholded at half
    coverage: [(w, h, rows of runs)], widest w."""
    def font(size):
        ft = ImageFont.truetype(ttf, size)
        ft.set_variation_by_axes([FACES[1][1]])        # the bold face
        return ft
    probe = font(200)
    cap = probe.getbbox("H", anchor="ls")
    size = max(8, round(200 * ink_h / (cap[3] - cap[1])))
    ft = font(size)
    out = []
    for ch in LETTERS:
        pad = size
        img = Image.new("L", (size * 2 + pad, size * 2 + pad), 0)
        ImageDraw.Draw(img).text((pad // 2, size + pad // 2), ch, font=ft, fill=255, anchor="ls")
        bits = img.point(lambda v: 255 if v >= 128 else 0)
        box = bits.getbbox()
        bits = bits.crop(box)
        w, h = bits.size
        data = bits.get_flattened_data() if hasattr(bits, "get_flattened_data") else bits.getdata()
        data = list(data)
        rows = []
        for y in range(h):
            runs, x, on = [], 0, False
            while x < w:
                n = 0
                while x < w and (data[y * w + x] != 0) == on:
                    n += 1
                    x += 1
                while n > 255:
                    runs += [255, 0]
                    n -= 255
                runs.append(n)
                on = not on
            rows.append(runs)
        out.append((w, h, rows))
    return out, max(w for w, h, r in out)


def build(ttf, H, greek=None):
    k256 = round(H * 256 / RTG_LINES)
    k = k256 / 256
    cell_h, base = round(CELL * k), round(BASE * k)
    out = bytearray(b"AMSM")
    out += struct.pack(">HHHHH", 3, k256, cell_h, base, len(FACES))
    for size, wght in FACES:
        glyphs, blob = face(ttf, size, wght, k, cell_h, base)
        out += struct.pack(">HH", LO, HI)
        for adv16, xo, yo, w, h, off in glyphs:
            out += struct.pack(">HbbBBI", adv16, xo, yo, w, h, off)
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
    letters, cell_w = big_letters(ttf, overlay_ink(H, k256))
    out += struct.pack(">HH", len(letters), cell_w)
    for w, h, rows in letters:
        data = bytes(v for runs in rows for v in runs)
        out += struct.pack(">HHI", w, h, len(data)) + data
    if greek:
        # Geologica's axes: weight, cursive, sharpness, slant
        out += b"GREK"
        for size, wght in FACES:
            glyphs, blob = face(greek, size, wght, k, cell_h, base,
                                codes=range(GREEK_LO, GREEK_HI + 1),
                                axes=lambda w: [w, 0, 0, 0], scale=GREEK_SCALE)
            out += struct.pack(">HH", GREEK_LO, GREEK_HI)
            for adv16, xo, yo, w, h, off in glyphs:
                out += struct.pack(">HbbBBI", adv16, xo, yo, w, h, off)
            out += struct.pack(">I", len(blob)) + blob
    return bytes(out)


def logo_file(ttf, scale):
    """The smooth logo alone at a whole scale, for the pixel style on deep
    RTG screens: "AMLG", u16 version (1), u16 w, u16 h, u16 colours,
    colours x (r, g, b), w x h indices (255 transparent)."""
    w, h, pal, idx = logo(ttf, scale)
    out = bytearray(b"AMLG") + struct.pack(">HHHH", 1, w, h, len(pal))
    for c in pal:
        out += bytes(c)
    return bytes(out + idx)


def main():
    args = sys.argv[1:]
    greek = None
    if "--greek" in args:
        i = args.index("--greek")
        greek = args[i + 1]
        del args[i:i + 2]
    ttf, outdir = args[0], args[1]
    heights = [int(v) for v in args[2:]] or [480, 600, 768]
    os.makedirs(outdir, exist_ok=True)
    for H in heights:
        data = build(ttf, H, greek)
        with open(os.path.join(outdir, str(H)), "wb") as fh:
            fh.write(data)
        print(f"  SMOOTH {H} lines: scale {round(H * 256 / RTG_LINES) / 256:.2f}, {len(data)} bytes")
    for scale in (1, 2, 3):
        data = logo_file(ttf, scale)
        with open(os.path.join(outdir, f"logo{scale}"), "wb") as fh:
            fh.write(data)
        print(f"  SMOOTH logo at {scale}x: {len(data)} bytes")


if __name__ == "__main__":
    main()
