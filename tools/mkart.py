#!/usr/bin/env python3
"""Generate src/art.c: UI palette, pixel-art logos and 16x16 menu icons.

Usage: python3 tools/mkart.py <Rubik-Bold.ttf> <out art.c>

The logo is drawn with anti-aliasing at 8x, scaled down to the screen's real
pixel grid and snapped to fixed palette pens, then stored as chunky pen
indices. Icons are 1-bit 16x16 masks drawn in the current text colour.
"""
import sys

from PIL import Image, ImageDraw, ImageFont

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


EXTRA = [ocs(c) for c in [
    mix(BG, WHITE, 1 / 3), mix(BG, WHITE, 2 / 3), mix(BG, BLUE, 0.5), mix(BLUE, WHITE, 0.5),
    mix(BG, ORANGE, 1 / 3), mix(BG, ORANGE, 2 / 3), mix(ORANGE, NAVY, 1 / 3), mix(ORANGE, NAVY, 2 / 3),
]]
PALETTE = UI + EXTRA                       # 24 entries; pens 24..31 spare
while len(PALETTE) < 32:
    PALETTE.append((0, 0, 0))
LOGO_PENS = [1, 5, 6, 8, 0] + list(range(16, 24))   # pens a logo pixel may use

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
    hi = []
    for y in range(h):
        for xx in range(w):
            pen = lo[y * w + xx]
            hi += [pen, pen]
    return (w, h, lo), (w * 2, h, hi)


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
    rows = []
    for line in ICON_ART[name]:
        assert len(line) == 16, (name, line)
        v = 0
        for x, ch in enumerate(line):
            if ch == "#":
                v |= 0x8000 >> x
        rows.append(v)
    return rows


def c_bytes(name, data, per=24):
    lines = []
    for i in range(0, len(data), per):
        lines.append("    " + ",".join(str(v) for v in data[i:i + per]) + ",")
    return f"const UBYTE {name}[] = {{\n" + "\n".join(lines) + "\n};\n"


def main():
    ttf, out = sys.argv[1], sys.argv[2]
    (lw, lh, lo), (hw, hh, hi) = logo(ttf)
    pal = [f"0x{(len(PALETTE)) << 16:08X}"]
    for (r, g, b) in PALETTE:
        pal += [f"0x{r * 0x01010101:08X}", f"0x{g * 0x01010101:08X}", f"0x{b * 0x01010101:08X}"]
    pal.append("0")
    icons = [icon(n) for n in ICON_ORDER]
    with open(out, "w") as fh:
        fh.write("/* Generated by tools/mkart.py - do not edit. */\n#include \"amce.h\"\n\n")
        fh.write(f"const UWORD logo_lo_w = {lw}, logo_lo_h = {lh};\n")
        fh.write(f"const UWORD logo_hi_w = {hw}, logo_hi_h = {hh};\n\n")
        fh.write(c_bytes("logo_lo", lo))
        fh.write(c_bytes("logo_hi", hi))
        fh.write("\nconst UWORD icons16[][16] = {\n")
        for rows in icons:
            fh.write("    { " + ", ".join(f"0x{v:04X}" for v in rows) + " },\n")
        fh.write("};\n\n/* LoadRGB32 table: count<<16|first, then R,G,B as 32-bit values, then 0 */\n")
        fh.write("const ULONG ui_palette32[] = {\n    " + pal[0] + ",\n")
        for i in range(1, len(pal) - 1, 3):
            fh.write("    " + ", ".join(pal[i:i + 3]) + ",\n")
        fh.write("    0\n};\n")
    # The true-aspect preview is tools/preview_screen.py (make preview).
    print(f"logo lo {lw}x{lh}, hi {hw}x{hh}, {len(icons)} icons, {len(PALETTE)} pens")


if __name__ == "__main__":
    main()
