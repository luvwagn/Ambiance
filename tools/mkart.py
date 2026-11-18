#!/usr/bin/env python3
"""Generate src/art.c: UI palette, pixel-art logos and 16x16 menu icons.

Usage: python3 tools/mkart.py <Rubik-Bold.ttf> <out art.c>

The logo is hand-drawn pixel art in palette pens (pixel_logo), stored as
chunky pen indices and enlarged on screens with bigger pixels. Icons
are 1-bit masks drawn in the current text colour: 16x16 hand-drawn for
lowres, 32x16 drawn from vector shapes for hires (tools/hiicons.py).
"""
import os
import subprocess
import sys
import tempfile

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
    (0xEB, 0xAF, 0x00),  # 12 PEN_PLEX     Plex's yellow (its logo, Credits)
    (0x80, 0x88, 0x99),  # 13 PEN_PLEX_W2  the logo's white edges: white and PEN_BG halved
    (0x76, 0x60, 0x1A),  # 14 PEN_PLEX_Y2  its yellow edges: Plex's yellow and PEN_BG halved
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


# ------------------------------------------------------------ Plex's logo
# Plex's wordmark (assets/plex/plex.svg, Simple Icons' path, CC0; the mark
# is Plex's), drawn as Plex's guidelines have it on a dark ground: white,
# the x's chevron in Plex's yellow. Rasterised by rsvg-convert, then
# reduced to the size asked for with two levels of edge: full pens where a
# pixel is mostly covered, the halved pens (13, 14) where partly.
PLEX_SVG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "plex", "plex.svg")
PLEX_CHEVRON = [(19.142, 15.29), (16.752, 15.29), (18.999, 11.958), (16.752, 8.623), (19.142, 8.623),
                (21.39, 11.958)]                # the path's fifth part, in its units
LP_PLEX, LP_PLEX_W2, LP_PLEX_Y2, LP_NONE = 12, 13, 14, 255


def plex_logo(height=20):
    sc = 100                                    # rasterised at 100 px a unit
    with tempfile.TemporaryDirectory() as d:
        png = os.path.join(d, "plex.png")
        subprocess.run(["rsvg-convert", "-w", str(24 * sc), "-h", str(24 * sc), PLEX_SVG, "-o", png],
                       check=True)
        full = Image.open(png).convert("RGBA").split()[-1]
    box = full.getbbox()
    full = full.crop(box)
    chev = Image.new("L", full.size, 0)
    ImageDraw.Draw(chev).polygon([(x * sc - box[0], y * sc - box[1]) for x, y in PLEX_CHEVRON], fill=255)
    w = round(full.width * height / full.height)
    f = full.resize((w, height), Image.BOX).load()
    c = chev.resize((w, height), Image.BOX).load()
    out = []
    for y in range(height):
        for x in range(w):
            a, ch = f[x, y], c[x, y]
            yellow = ch * 2 > a
            if a >= 150:
                out.append(LP_PLEX if yellow else LP_WHITE)
            elif a >= 60:
                out.append(LP_PLEX_Y2 if yellow else LP_PLEX_W2)
            else:
                out.append(LP_NONE)
    return w, height, out


def plex_logo_smooth(height=64):
    """The wordmark as coverage, for the layouts' smooth screens: a byte a
    pixel, its ink's coverage (high nibble, 0-15) and the yellow chevron's
    (low nibble), made smaller with an area average where it is shown."""
    sc = 100
    with tempfile.TemporaryDirectory() as d:
        png = os.path.join(d, "plex.png")
        subprocess.run(["rsvg-convert", "-w", str(24 * sc), "-h", str(24 * sc), PLEX_SVG, "-o", png],
                       check=True)
        full = Image.open(png).convert("RGBA").split()[-1]
    box = full.getbbox()
    full = full.crop(box)
    chev = Image.new("L", full.size, 0)
    ImageDraw.Draw(chev).polygon([(x * sc - box[0], y * sc - box[1]) for x, y in PLEX_CHEVRON], fill=255)
    w = round(full.width * height / full.height)
    f = full.resize((w, height), Image.BOX).load()
    c = chev.resize((w, height), Image.BOX).load()
    out = []
    for y in range(height):
        for x in range(w):
            a = f[x, y]
            ch = min(c[x, y], a)
            out.append((round(a * 15 / 255) << 4) | round(ch * 15 / 255))
    return w, height, out


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
        fh.write("/* Generated by tools/mkart.py - do not edit. */\n#include \"amb.h\"\n\n")
        fh.write(f"const UWORD logo_lo_w = {lw}, logo_lo_h = {lh};\n\n")
        fh.write(c_bytes("logo_lo", lo))
        pw, ph, pl = plex_logo()
        fh.write("\n/* Plex's logo (pens; 255: none), for Credits: Plex's, used under its\n"
                 " * guidelines (assets/plex) */\n")
        fh.write(f"const UWORD plex_lo_w = {pw}, plex_lo_h = {ph};\n\n")
        fh.write(c_bytes("plex_lo", pl))
        sw, sh, sl = plex_logo_smooth()
        fh.write("\n/* ...and as coverage for the layouts (smooth): each byte its ink's\n"
                 " * coverage 0-15 (high nibble), the yellow chevron's (low) */\n")
        fh.write(f"const UWORD plex_hi_w = {sw}, plex_hi_h = {sh};\n\n")
        fh.write(c_bytes("plex_hi", sl))
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
