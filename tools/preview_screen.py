#!/usr/bin/env python3
"""Render the Home Screen on the host the way AmigaMCE draws it.

Usage: python3 tools/preview_screen.py <art.c> <Fonts dir> <out.png> [lores|hires] [pal|ntsc]

Mirrors the layout constants in src/gfx.c and src/main.c, decodes the real
amce-pixel font files and parses the generated art.c, then scales the result
to the Amiga's pixel aspect (lowres 3x3, hires 1.5x3) for an honest look.
"""
import os
import re
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from amigafont import parse_size_file, render  # noqa: E402


def parse_art(path):
    src = open(path).read()

    def arr(name):
        m = re.search(r"%s\[\] = \{(.*?)\};" % name, src, re.S)
        return [int(v, 0) for v in re.findall(r"0x[0-9A-Fa-f]+|\d+", m.group(1))]
    dims = {k: int(v) for k, v in re.findall(r"(logo_\w+_[wh]) = (\d+)", src)}
    icons_src = re.search(r"icons16\[\]\[16\] = \{(.*?)\};", src, re.S).group(1)
    icons = [[int(v, 16) for v in re.findall(r"0x([0-9A-F]{4})", grp)] for grp in re.findall(r"\{([^}]*)\}", icons_src)]
    pal = arr("ui_palette32")[1:-1]
    palette = [(pal[i] >> 24, pal[i + 1] >> 24, pal[i + 2] >> 24) for i in range(0, len(pal), 3)]
    return arr("logo_lo"), arr("logo_hi"), dims, icons, palette


class Screen:
    def __init__(self, w, h, palette, font):
        self.img = Image.new("RGB", (w, h))
        self.pal = palette
        self.font = font
        self.w, self.h = w, h

    def fill(self, x0, y0, x1, y1, pen):
        for y in range(max(0, y0), min(self.h, y1 + 1)):
            for x in range(max(0, x0), min(self.w, x1 + 1)):
                self.img.putpixel((x, y), self.pal[pen])

    def textlen(self, s):
        return render(self.font, s).width - 2

    def text(self, x, y, s, pen, maxw=0):
        while maxw and s and self.textlen(s) > maxw:
            s = s[:-1]
        m = render(self.font, s)
        for yy in range(m.height):
            for xx in range(m.width):
                if m.getpixel((xx, yy)) and 0 <= x + xx < self.w and 0 <= y + yy < self.h:
                    self.img.putpixel((x + xx, y + yy), self.pal[pen])


def main():
    art, fonts, out = sys.argv[1], sys.argv[2], sys.argv[3]
    mode = sys.argv[4] if len(sys.argv) > 4 else "hires"
    video = sys.argv[5] if len(sys.argv) > 5 else "pal"
    logo_lo, logo_hi, dims, icons, pal = parse_art(art)
    hires = mode == "hires"
    sx = 2 if hires else 1
    W, H = (640 if hires else 320), (256 if video == "pal" else 200)
    fname = "amce-pixel-hi" if hires else "amce-pixel"
    font = parse_size_file(open(os.path.join(fonts, fname, "12"), "rb").read())
    s = Screen(W, H, pal, font)
    menu_w = 200 if hires else 104
    top, row_h, list_top, list_row_h, footer_y = 26, 18, 44, 14, H - 12
    pane_x = menu_w + 6 * sx
    pane_w = W - pane_x - 4 * sx
    fh = 12
    BG, BAR, RULE, TEXT, DIM, WHITE, HL, SELDIM, ACCENT = range(9)

    s.fill(0, 0, W - 1, H - 1, BG)
    s.fill(0, 0, W - 1, 23, BAR)
    s.fill(0, 24, W - 1, 24, RULE)
    lw, lh = (dims["logo_hi_w"], dims["logo_hi_h"]) if hires else (dims["logo_lo_w"], dims["logo_lo_h"])
    logo = logo_hi if hires else logo_lo
    for j in range(lh):
        for i in range(lw):
            pen = logo[j * lw + i]
            if pen != BAR:
                s.img.putpixel((6 * sx + i, 4 + j), pal[pen])
    clock = "Tue 03 Nov  00:05"
    s.text(W - 8 * sx - s.textlen(clock), (24 - fh) // 2, clock, TEXT)

    names = ["Games", "Mods", "Demos", "Videos", "Pictures", "Music", "Internet Radio", "Settings", "About"]
    for i, name in enumerate(names):
        y = top + i * row_h
        sel = i == 0
        if sel:
            s.fill(0, y, menu_w - 1, y + row_h - 1, SELDIM)
        fg = WHITE if sel else TEXT
        for r in range(16):
            bits = icons[i][r]
            for b in range(16):
                if bits & (0x8000 >> b):
                    for k in range(sx):
                        s.img.putpixel((3 * sx + b * sx + k, y + 1 + r), pal[fg])
        s.text(23 * sx, y + (row_h - fh) // 2, name, fg, menu_w - 24 * sx)
    s.fill(menu_w + 2 * sx, 25, menu_w + 2 * sx, footer_y - 2, RULE)

    s.text(pane_x, 28, "Games", WHITE)
    s.text(pane_x + pane_w - s.textlen("2/16"), 28, "2/16", DIM)
    s.fill(pane_x, 41, pane_x + pane_w - 1, 41, RULE)
    rows = ["Favourites", "All A-Z", "By Genre", "By Publisher", "By System", "By Theme", "By Year",
            "Multiplayer", "NTSC", "Non English", "Top 200", "AGA", "Beta", "Emulators"]
    vis = (footer_y - 2 - list_top) // list_row_h
    for r, label in enumerate(rows[:vis]):
        y = list_top + r * list_row_h
        sel = r == 1
        if sel:
            s.fill(pane_x - 2 * sx, y, pane_x + pane_w - 1, y + list_row_h - 1, HL)
        s.text(pane_x, y + 1, label, WHITE if sel else TEXT, pane_w - 12 * sx)
        s.text(pane_x + pane_w - 7 * sx, y + 1, ">", WHITE if sel else DIM)
    s.fill(0, footer_y - 1, W - 1, footer_y - 1, RULE)
    s.fill(0, footer_y, W - 1, H - 1, BAR)
    hint = ("Up/Down  move    Right/Fire  open    Return  play    Left/Esc  back" if hires
            else "Fire open  Ret play  Esc back")
    s.text(6 * sx, footer_y + (12 - fh) // 2, hint, DIM, W - 12 * sx)

    scale_x, scale_y = (3, 6) if hires else (6, 6)   # hires pixel is half as wide
    s.img.resize((W * scale_x // 2, H * scale_y // 2), Image.NEAREST).save(out)
    print("wrote", out)


if __name__ == "__main__":
    main()
