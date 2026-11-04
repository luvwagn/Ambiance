#!/usr/bin/env python3
"""Generate src/art.c: UI palette, pixel-art logos and 16x16 menu icons.

Usage: python3 tools/mkart.py <Rubik-Bold.ttf> <out art.c>

The lowres logo is drawn crisp at the screen's real pixel size and snapped
to fixed palette pens; the hires logo is redrawn from the same shapes on the
hires grid (see logo_hires). Both are stored as chunky pen indices. Icons
are 1-bit masks drawn in the current text colour: 16x16 hand-drawn for
lowres, 32x16 drawn from vector shapes for hires (tools/hiicons.py).
"""
import sys

from PIL import Image, ImageDraw, ImageFont

import hiicons
import markicons

# ------------------------------------------------------------ palette (pens)
UI = [
    (0x00, 0x11, 0x33),  # 0 PEN_BG       content background
    (0x00, 0x00, 0x22),  # 1 PEN_BAR      header / footer bar
    (0x33, 0x44, 0x66),  # 2 PEN_RULE
    (0xCC, 0xDD, 0xEE),  # 3 PEN_TEXT
    (0x88, 0x99, 0xBB),  # 4 PEN_DIM
    (0xFF, 0xFF, 0xFF),  # 5 PEN_WHITE
    (0x22, 0x66, 0xCC),  # 6 PEN_HL       focused selection
    (0x11, 0x33, 0x66),  # 7 PEN_SELDIM   unfocused selection
    (0xFF, 0xAA, 0x00),  # 8 PEN_ACCENT
    (0x22, 0x33, 0x55),  # 9 PEN_KEYCAP
    (0x66, 0x77, 0x99),  # 10 PEN_FAINT
]
while len(UI) < 16:
    UI.append((0, 0, 0))

BG, WHITE, BLUE, ORANGE, NAVY = UI[1], UI[5], UI[6], UI[8], UI[0]


def mix(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def ocs(c):
    return tuple(round(v / 17) * 17 for v in c)


# Only pens 0-15 are AmigaMCE's. Pens 16-31 keep the screen's own colours:
# 17-19 are the mouse pointer's, set by Intuition from the user's pointer
# preferences, so the pointer stays the default Amiga one.
PALETTE = UI                               # 16 entries
LOGO_PENS = [1, 5, 6, 8, 0]                # pens a logo pixel may use

# ------------------------------------------------------------ logo
# Rendered at the screen's real pixel size with crisp (non-smoothed) edges:
# smoothing a 14-pixel wordmark only makes it fuzzy. The hires logo doubles
# every column, so it has the same shape on screen as the lowres one.


def snap(img):
    px = img.load()
    out = []
    for y in range(img.height):
        for x in range(img.width):
            c = px[x, y][:3]
            pen = min(LOGO_PENS, key=lambda p: sum((PALETTE[p][i] - c[i]) ** 2 for i in range(3)))
            out.append(pen)
    return out


def logo(ttf, h=16, size_a=14, size_m=11):
    img = Image.new("RGB", (160, h), BG)
    d = ImageDraw.Draw(img)
    d.fontmode = "1"
    d.rectangle([0, 1, 13, 14], fill=BLUE)
    for xy in [(0, 1), (13, 1), (0, 14), (13, 14)]:
        img.putpixel(xy, BG)
    d.polygon([(4, 4), (10, 7.5), (4, 11)], fill=WHITE)
    fa = ImageFont.truetype(ttf, size_a)
    x = 17
    d.text((x, 13), "Amiga", font=fa, fill=WHITE, anchor="ls")
    x += int(d.textlength("Amiga", font=fa)) + 3
    fm = ImageFont.truetype(ttf, size_m)
    tw = int(d.textlength("MCE", font=fm))
    d.rectangle([x, 1, x + tw + 5, 14], fill=ORANGE)
    d.text((x + 3, 12), "MCE", font=fm, fill=NAVY, anchor="ls")
    img = img.crop((0, 0, x + tw + 6, h))
    lo = snap(img)
    w = img.width
    return (w, h, lo), logo_hires(ttf, w, h, size_a, size_m)


# The hires logo is redrawn from the same vector shapes on the real hires
# grid (twice as many columns), not column-doubled. Each layer is drawn
# anti-aliased on a fine canvas, averaged down to whole hires pixels and
# thresholded at 50% coverage, so edges stay crisp but fall on half-lowres
# positions: smoother curves and truer stems, same size on screen.
S = 8   # canvas units per lowres pixel, both axes


def layer_mask(w, h, draw_fn):
    big = Image.new("L", (w * S, h * S), 0)
    draw_fn(ImageDraw.Draw(big))
    small = big.resize((w * 2, h), Image.BOX)
    return [v >= 128 for v in small.get_flattened_data()] if hasattr(small, "get_flattened_data") \
        else [v >= 128 for v in small.getdata()]


def logo_hires(ttf, w, h, size_a, size_m):
    fa = ImageFont.truetype(ttf, size_a * S)
    fm = ImageFont.truetype(ttf, size_m * S)
    fa_lo = ImageFont.truetype(ttf, size_a)
    fm_lo = ImageFont.truetype(ttf, size_m)
    probe = ImageDraw.Draw(Image.new("RGB", (4, 4)))
    x_badge = 17 + int(probe.textlength("Amiga", font=fa_lo)) + 3
    tw = int(probe.textlength("MCE", font=fm_lo))

    def box(d):
        d.rounded_rectangle([0, 1 * S, 14 * S - 1, 15 * S - 1], radius=int(1.6 * S), fill=255)

    def tri(d):
        d.polygon([(4 * S, 4 * S), (10.5 * S, 8 * S), (4 * S, 12 * S)], fill=255)

    def amiga(d):
        d.text((17 * S, 13 * S), "Amiga", font=fa, fill=255, anchor="ls")

    def badge(d):
        d.rounded_rectangle([x_badge * S, 1 * S, (x_badge + tw + 6) * S - 1, 15 * S - 1],
                            radius=int(1.2 * S), fill=255)

    def mce(d):
        d.text(((x_badge + 3) * S, 12 * S), "MCE", font=fm, fill=255, anchor="ls")

    pens = [1] * (w * 2 * h)                # PEN_BAR background
    for fn, pen in ((box, 6), (tri, 5), (amiga, 5), (badge, 8), (mce, 0)):
        for i, on in enumerate(layer_mask(w, h, fn)):
            if on:
                pens[i] = pen
    return (w * 2, h, pens)




# ------------------------------------------------------------ icons
# Hand-drawn 16x16 pixel art, one string per row ('#' = ink).
ICON_ART = {
    "games": [
        "................",
        "................",
        "................",
        "...##########...",
        "..#..........#..",
        ".#............#.",
        ".#..#.....##..#.",
        ".#.###........#.",
        ".#..#......##.#.",
        ".#............#.",
        ".#....####....#.",
        "..#..#....#..#..",
        "...##......##...",
        "................",
        "................",
        "................",
    ],
    "mods": [
        "................",
        "..###########...",
        "..#.........#...",
        "..#.###.##..#...",
        "..#.........#...",
        "..#.##.####.#...",
        "..#.........#...",
        "..#.####.##.#...",
        "..#.........#...",
        "..#.##.###..#...",
        "..#.........#...",
        "..#.###.##..#...",
        "..#.........#...",
        "..###########...",
        "................",
        "................",
    ],
    "demos": [
        "................",
        ".......#........",
        ".......#........",
        "......###.......",
        "......###.......",
        ".....#####......",
        "##############..",
        "..##########....",
        "....######......",
        "....###.###.....",
        "...###...###....",
        "...##.....##....",
        "..##.......##...",
        "..#.........#...",
        "................",
        "................",
    ],
    "videos": [
        "................",
        "................",
        ".##############.",
        ".#.#.#.#.#.#.#.#",
        ".##############.",
        ".#............#.",
        ".#.....#......#.",
        ".#.....##.....#.",
        ".#.....###....#.",
        ".#.....##.....#.",
        ".#.....#......#.",
        ".##############.",
        ".#.#.#.#.#.#.#.#",
        ".##############.",
        "................",
        "................",
    ],
    "pictures": [
        "................",
        "................",
        ".##############.",
        ".#............#.",
        ".#..##........#.",
        ".#.####.......#.",
        ".#..##........#.",
        ".#.........#..#.",
        ".#........###.#.",
        ".#...#...#####.#",
        ".#..###.######.#",
        ".#.###########.#",
        ".##############.",
        "................",
        "................",
        "................",
    ],
    "music": [
        "................",
        "........######..",
        "........#....#..",
        "........######..",
        "........#....#..",
        "........#....#..",
        "........#....#..",
        "........#....#..",
        "........#....#..",
        "........#....#..",
        "......###..###..",
        ".....####.####..",
        ".....####.####..",
        "......##...##...",
        "................",
        "................",
    ],
    "radio": [
        "................",
        "..........##....",
        "........##......",
        "......##........",
        "....##..........",
        ".##############.",
        ".#............#.",
        ".#.###....###.#.",
        ".#.......#...##.",
        ".#.###...#...##.",
        ".#.......#...##.",
        ".#.###....###.#.",
        ".#............#.",
        ".##############.",
        "................",
        "................",
    ],
    "settings": [
        "................",
        "......####......",
        "..##..####..##..",
        "..############..",
        "...##########...",
        "..####....####..",
        "#####......#####",
        "#####......#####",
        "#####......#####",
        "#####......#####",
        "..####....####..",
        "...##########...",
        "..############..",
        "..##..####..##..",
        "......####......",
        "................",
    ],
    "about": [
        "................",
        ".....######.....",
        "...##......##...",
        "..#....##....#..",
        ".#.....##.....#.",
        ".#............#.",
        "#.....###......#",
        "#......##......#",
        "#......##......#",
        "#......##......#",
        ".#.....##.....#.",
        ".#....####....#.",
        "..#..........#..",
        "...##......##...",
        ".....######.....",
        "................",
    ],
}
ICON_ORDER = ["games", "mods", "demos", "videos", "pictures", "music", "radio", "settings", "about"]


def icon(name):
    """16x16 icon as row bitmasks, centred horizontally on its drawn pixels
    so the menu's icons line up on one axis whatever their width (an odd
    width leans half a pixel left)."""
    lines = ICON_ART[name]
    for line in lines:
        assert len(line) == 16, (name, line)
    cols = [x for x in range(16) if any(line[x] == "#" for line in lines)]
    shift = (16 - (cols[-1] - cols[0] + 1)) // 2 - cols[0] if cols else 0
    rows = []
    for line in lines:
        v = 0
        for x, ch in enumerate(line):
            if ch == "#":
                v |= 0x8000 >> (x + shift)
        rows.append(v)
    return rows


def c_bytes(name, data, per=24):
    lines = []
    for i in range(0, len(data), per):
        lines.append("    " + ",".join(str(v) for v in data[i:i + per]) + ",")
    return f"const UBYTE {name}[] = {{\n" + "\n".join(lines) + "\n};\n"


def main():
    ttf, out = sys.argv[1], sys.argv[2]
    (lw, lh, lo), _hires = logo(ttf)
    pal = [f"0x{(len(PALETTE)) << 16:08X}"]
    for (r, g, b) in PALETTE:
        pal += [f"0x{r * 0x01010101:08X}", f"0x{g * 0x01010101:08X}", f"0x{b * 0x01010101:08X}"]
    pal.append("0")
    icons = [icon(n) for n in ICON_ORDER]
    with open(out, "w") as fh:
        fh.write("/* Generated by tools/mkart.py - do not edit. */\n#include \"amce.h\"\n\n")
        fh.write(f"const UWORD logo_lo_w = {lw}, logo_lo_h = {lh};\n\n")
        fh.write(c_bytes("logo_lo", lo))
        fh.write("\nconst UWORD icons16[][16] = {\n")
        for rows in icons:
            fh.write("    { " + ", ".join(f"0x{v:04X}" for v in rows) + " },\n")
        fh.write("};\n\n/* List row markers (MARK_ARROW..MARK_RADIO_ON), 11 rows each */\n")
        fh.write("const UWORD marks16[][11] = {\n")
        for n in markicons.ORDER:
            fh.write("    { " + ", ".join(f"0x{v:04X}" for v in markicons.lores(n)) + " },\n")
        fh.write("};\n\n/* LoadRGB32 table: count<<16|first, then R,G,B as 32-bit values, then 0 */\n")
        fh.write("const ULONG ui_palette32[] = {\n    " + pal[0] + ",\n")
        for i in range(1, len(pal) - 1, 3):
            fh.write("    " + ", ".join(pal[i:i + 3]) + ",\n")
        fh.write("    0\n};\n")
    # The true-aspect preview is tools/preview_screen.py (make preview).
    print(f"logo {lw}x{lh}, {len(icons)} icons, {len(PALETTE)} pens")


if __name__ == "__main__":
    main()
