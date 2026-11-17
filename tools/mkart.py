#!/usr/bin/env python3
"""Generate src/art.c: UI palette, pixel-art logos and 16x16 menu icons.

Usage: python3 tools/mkart.py <Rubik-Bold.ttf> <out art.c>

The logo is hand-drawn pixel art in palette pens (pixel_logo), stored as
chunky pen indices and enlarged on screens with bigger pixels. Icons
are 1-bit masks drawn in the current text colour: 16x16 hand-drawn for
lowres, 32x16 drawn from vector shapes for hires (tools/hiicons.py).
"""
import sys

from PIL import Image, ImageDraw

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
    (0xFF, 0xDD, 0x33),  # 11 PEN_YELLOW   what Fire presses (the focus frame)
]
while len(UI) < 16:
    UI.append((0, 0, 0))

BG, WHITE, BLUE, ORANGE, NAVY = UI[1], UI[5], UI[6], UI[8], UI[0]


def mix(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def ocs(c):
    return tuple(round(v / 17) * 17 for v in c)


# Only pens 0-15 are Ambiance's. Pens 16-31 keep the screen's own colours:
# 17-19 are the mouse pointer's, set by Intuition from the user's pointer
# preferences, so the pointer stays the default Amiga one.
PALETTE = UI                               # 16 entries

# ------------------------------------------------------------ pixel logo
# Hand-drawn: 2-pixel stems on one grid, a symmetric play arrow. Pens:
# 1 bar (left undrawn), 0 navy, 5 white, 6 blue, 8 orange.
LOGO_GLYPHS = {   # rows from the top of the text area; '#' = ink
 "A": (3, ["...###...",        # pointed, like the font's
           "...###...",
           "..##.##..",
           "..##.##..",
           ".##...##.",
           ".##...##.",
           ".#######.",
           "##.....##",
           "##.....##",
           "##.....##"]),
 "m": (6, ["#########.",
           "##..##..##",
           "##..##..##",
           "##..##..##",
           "##..##..##",
           "##..##..##",
           "##..##..##"]),
 "i": (3, ["##", "##", "..", "##", "##", "##", "##", "##", "##", "##"]),
 "b": (3, ["##......",
           "##......",
           "##......",
           "#######.",
           "##....##",
           "##....##",
           "##....##",
           "##....##",
           "##....##",
           "#######."]),
 "a": (6, [".######.",
           "......##",
           "......##",
           ".#######",
           "##....##",
           "##....##",
           ".#######"]),
 "n": (6, ["#######.",
           "##....##",
           "##....##",
           "##....##",
           "##....##",
           "##....##",
           "##....##"]),
 "c": (6, [".######",
           "##.....",
           "##.....",
           "##.....",
           "##.....",
           "##.....",
           ".######"]),
 "e": (6, [".######.",
           "##....##",
           "##....##",
           "########",
           "##......",
           "##......",
           ".#######"]),
}
LP_BAR, LP_NAVY, LP_WHITE, LP_BLUE, LP_GLOW = 1, 0, 5, 6, 7
LOGO_H = 18     # 16 rows of art and a row of halo above and below


def pixel_logo():
    """The logo as pixel art: the Play badge and "Ambiance", hand-drawn
    (a font rasterised this small came out jagged and uneven), with a
    one-pixel glow around them in pen 7 (the scheme's dim selection
    colour) and navy at the glow's corners. The letters' insides stay
    dark, so they read; the gaps between them glow."""
    H = 16
    cols, boxes = [], []        # columns of H pens; each letter's ink box
    def blank(n):
        for _ in range(n):
            cols.append([LP_BAR] * H)
    # play box: 14 wide, rows 1..14, corners rounded, triangle centred
    # 10 rows, one step a row, nudged right of centre (a triangle's weight
    # sits left of its middle)
    tri = {3 + i: (5, 5 + w - 1) for i, w in enumerate([2, 3, 4, 5, 6, 6, 5, 4, 3, 2])}
    for x in range(14):
        c = [LP_BAR] * H
        for y in range(1, 15):
            corner = (x in (0, 13)) and (y in (1, 14))
            if not corner:
                c[y] = LP_BLUE
            if y in tri and tri[y][0] <= x <= tri[y][1]:
                c[y] = LP_WHITE
        cols.append(c)
    blank(4)
    for k, ch in enumerate("Ambiance"):
        top, rows = LOGO_GLYPHS[ch]
        x0 = len(cols)
        for x in range(len(rows[0])):
            c = [LP_BAR] * H
            for j, r in enumerate(rows):
                if r[x] == "#":
                    c[top + j] = LP_WHITE
            cols.append(c)
        boxes.append((x0, len(cols) - 1, top, top + len(rows) - 1))
        if k < 7:
            blank(2)
    # the glow: one pixel all round, so one more column each side and a
    # row above and below
    w = len(cols) + 2
    art = [[LP_BAR] * w for _ in range(LOGO_H)]
    for x, c in enumerate(cols):
        for y, p in enumerate(c):
            art[y + 1][x + 1] = p
    def ink(x, y):
        return 0 <= x < w and 0 <= y < LOGO_H and art[y][x] != LP_BAR
    def inside(x, y):           # a letter's own space: its counters and gaps
        return any(x0 + 1 <= x <= x1 + 1 and t + 1 <= y <= b + 1 for x0, x1, t, b in boxes)
    glow = []
    for y in range(LOGO_H):
        for x in range(w):
            if art[y][x] != LP_BAR or inside(x, y):
                continue
            if any(ink(x + dx, y + dy) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                glow.append((x, y, LP_GLOW))
            elif any(ink(x + dx, y + dy) for dx, dy in ((1, 1), (1, -1), (-1, 1), (-1, -1))):
                glow.append((x, y, LP_NAVY))
    for x, y, p in glow:
        art[y][x] = p
    return w, LOGO_H, [art[y][x] for y in range(LOGO_H) for x in range(w)]


# ------------------------------------------------------------ icons
# Hand-drawn 16x16 pixel art, one string per row ('#' = ink).
ICON_ART = {
    "games": [
        "................",
        "......####......",
        ".....######.....",
        ".....######.....",
        "......####......",
        ".......##.......",
        ".......##.......",
        ".......##.......",
        ".......##.......",
        "..###..##.......",
        ".##############.",
        ".#............#.",
        ".#............#.",
        ".##############.",
        "................",
        "................",
    ],
    "mods": [
        "................",
        "................",
        "....##########..",
        "....##########..",
        "....#........#..",
        "....#........#..",
        "....#........#..",
        "....#........#..",
        "....#........#..",
        "..###......###..",
        ".####.....####..",
        ".####.....####..",
        "..##.......##...",
        "................",
        "................",
        "................",
    ],
    "demos": [
        "................",
        ".....##########.",
        "....##.......##.",
        "...#.#......#.#.",
        "..#..#.....#..#.",
        ".##########...#.",
        ".#...#....#...#.",
        ".#...#....#...#.",
        ".#...#....#...#.",
        ".#...#....#...#.",
        ".#...##########.",
        ".#..#.....#..#..",
        ".#.#......#.#...",
        ".##.......##....",
        ".##########.....",
        "................",
    ],
    "videos": [
        "................",
        "................",
        ".##############.",
        ".#.#.#.##.#.#.#.",
        ".##############.",
        ".#............#.",
        ".#.....#......#.",
        ".#.....##.....#.",
        ".#.....###....#.",
        ".#.....##.....#.",
        ".#.....#......#.",
        ".##############.",
        ".#.#.#.##.#.#.#.",
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
        ".#...#...######.",
        ".#..###.#######.",
        ".#.############.",
        ".##############.",
        "................",
        "................",
        "................",
    ],
    "music": [
        "................",
        "................",
        "......#####.....",
        "....##....##....",
        "...##......##...",
        "..##........##..",
        "..#..........#..",
        "..#..........#..",
        ".##..........##.",
        ".####......####.",
        ".####......####.",
        ".####......####.",
        ".####......####.",
        ".####......####.",
        ".###........###.",
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
        "................",
        "....##..........",
        "###############.",
        "....##..........",
        "................",
        "..........##....",
        "###############.",
        "..........##....",
        "................",
        ".......##.......",
        "###############.",
        ".......##.......",
        "................",
        "................",
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
    lw, lh, lo = pixel_logo()
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
        fh.write("};\n\n/* List row markers (MARK_ARROW..MARK_PLAY), 11 rows each */\n")
        fh.write("const UWORD marks16[][11] = {\n")
        for n in markicons.ORDER:
            fh.write("    { " + ", ".join(f"0x{v:04X}" for v in markicons.lores(n)) + " },\n")
        fh.write("};\n\n/* LoadRGB32 table: count<<16|first, then R,G,B as 32-bit values, then 0 */\n")
        fh.write("ULONG ui_palette32[] = {      /* (the scheme's colours put in: gfx.c ui_scheme_apply) */\n    " + pal[0] + ",\n")
        for i in range(1, len(pal) - 1, 3):
            fh.write("    " + ", ".join(pal[i:i + 3]) + ",\n")
        fh.write("    0\n};\n")
    # The true-aspect preview is tools/preview_screen.py (make preview).
    print(f"logo {lw}x{lh}, {len(icons)} icons, {len(PALETTE)} pens")


if __name__ == "__main__":
    main()
