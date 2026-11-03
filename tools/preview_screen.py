#!/usr/bin/env python3
"""Render the Home Screen on the host the way AmigaMCE draws it.

Usage: python3 tools/preview_screen.py <art.c> <Fonts dir> <out.png> [lores|hires] [pal|ntsc]
                                       [games|about] [scroll pixels]

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
    i32 = re.search(r"icons32\[\]\[16\] = \{(.*?)\};", src, re.S).group(1)
    icons32 = [[int(v, 16) for v in re.findall(r"0x([0-9A-F]{8})", grp)] for grp in re.findall(r"\{([^}]*)\}", i32)]
    pal = arr("ui_palette32")[1:-1]
    palette = [(pal[i] >> 24, pal[i + 1] >> 24, pal[i + 2] >> 24) for i in range(0, len(pal), 3)]
    return arr("logo_lo"), arr("logo_hi"), dims, icons, icons32, palette


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
    page = sys.argv[6] if len(sys.argv) > 6 else "games"
    scroll = int(sys.argv[7]) if len(sys.argv) > 7 else 0
    logo_lo, logo_hi, dims, icons, icons32, pal = parse_art(art)
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
    BG, BAR, RULE, TEXT, DIM, WHITE, HL, SELDIM, ACCENT, KEYCAP = range(10)

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

    # module order in art.c (icon index) and the main menu order from build_menu()
    mods = ["Games", "Mods", "Demos", "Videos", "Pictures", "Music", "Internet Radio", "Settings", "About"]
    names = ["Games", "Mods", "Demos", "Music", "Pictures", "Internet Radio", "Videos", "Settings", "About"]
    msel = names.index("About") if page == "about" else 0
    for i, name in enumerate(names):
        y = top + i * row_h
        sel = i == msel
        if sel:
            s.fill(0, y, menu_w - 1, y + row_h - 1, SELDIM)
        fg = WHITE if sel else TEXT
        for r in range(16):
            if hires:
                bits = icons32[mods.index(name)][r]
                for b in range(32):
                    if bits & (0x80000000 >> b):
                        s.img.putpixel((3 * sx + b, y + 1 + r), pal[fg])
            else:
                bits = icons[mods.index(name)][r]
                for b in range(16):
                    if bits & (0x8000 >> b):
                        s.img.putpixel((3 + b, y + 1 + r), pal[fg])
        s.text(23 * sx, y + (row_h - fh) // 2, name, fg, menu_w - 24 * sx)
    s.fill(menu_w + 2 * sx, 25, menu_w + 2 * sx, footer_y - 2, RULE)

    if page == "about":
        about(s, pane_x, pane_w, sx, list_top, footer_y, scroll, (TEXT, DIM, WHITE, RULE, SELDIM))
    else:
        games(s, pane_x, pane_w, sx, list_top, list_row_h, footer_y, (TEXT, DIM, WHITE, HL, RULE))
    s.fill(0, footer_y - 1, W - 1, footer_y - 1, RULE)
    s.fill(0, footer_y, W - 1, H - 1, BAR)
    if page == "about":
        hint = "Up/Down  scroll      Left/Esc  back" if hires else "Up/Down scroll  Esc back"
    else:
        hint = ("Up/Down  move    Right/Fire  open    Return  play    Left/Esc  back" if hires
                else "Fire open  Ret play  Esc back")
    s.text(6 * sx, footer_y + (12 - fh) // 2, hint, DIM, W - 12 * sx)

    scale_x, scale_y = (3, 6) if hires else (6, 6)   # hires pixel is half as wide
    s.img.resize((W * scale_x // 2, H * scale_y // 2), Image.NEAREST).save(out)
    print("wrote", out)


def _version():
    try:
        return open(os.path.join(os.path.dirname(__file__), "..", "build", "version")).readline().strip()
    except OSError:
        return "0.0.0"


ABOUT = [("line", "AmigaMCE " + _version()), ("line", "Created by Brian King"), ("line", "(c) 2026 Brian King"),
         ("line", ""),
         ("para", "Fonts: Pixelify Sans (c) 2021 The Pixelify Sans Project Authors, and Rubik "
                  "(c) 2015 The Rubik Project Authors, both under the SIL Open Font License 1.1. "
                  "The amce-pixel fonts are Modified Versions of Pixelify Sans."),
         ("line", ""), ("para", "Code: libnix C runtime (public domain)."), ("line", ""),
         ("para", "Used if installed, not included, each under its own licence: Amiga Game "
                  "Selector, WHDLoad, AHI, MHI drivers, mpega.library, datatypes."),
         ("line", ""),
         ("para", "Amiga is a trademark of its owner. Games, demos, music and pictures belong "
                  "to their authors. Licence texts are in the Licences drawer.")]


def wrap(s, text, maxw):
    out, line = [], ""
    for word in text.split():
        trial = (line + " " + word) if line else word
        if line and s.textlen(trial) > maxw:
            out.append(line)
            line = word
        else:
            line = trial
    if line:
        out.append(line)
    return out


def about(s, pane_x, pane_w, sx, y0, footer_y, scroll, pens):
    """Mirror of src/textbox.c: text box, then the scroll bar beside it."""
    TEXT, DIM, WHITE, RULE, TRACK = pens
    s.text(pane_x, 28, "About", WHITE)
    s.fill(pane_x, 41, pane_x + pane_w - 1, 41, RULE)
    tbw = pane_w - 7 * sx
    lines = []
    for kind, t in ABOUT:
        lines += [t] if kind == "line" else wrap(s, t, tbw)
    lh, h = 13, footer_y - 2 - y0
    content = len(lines) * lh
    pos = max(0, min(scroll, content - h))
    clip = Image.new("RGB", (tbw, h), s.pal[0])
    sub = Screen(tbw, content + h, s.pal, s.font)
    sub.img.paste(s.pal[0], (0, 0, tbw, content + h))   # bitmap cleared to pen 0
    for i, t in enumerate(lines):
        sub.text(0, i * lh, t, TEXT, tbw)
    clip.paste(sub.img.crop((0, pos, tbw, pos + h)), (0, 0))
    s.img.paste(clip, (pane_x, y0))
    if content > h:
        bx, bw = pane_x + tbw + 2 * sx, 3 * sx
        th = max(8, h * h // content)
        ty = (h - th) * pos // (content - h)
        s.fill(bx, y0, bx + bw - 1, y0 + ty - 1, TRACK)
        s.fill(bx, y0 + ty, bx + bw - 1, y0 + ty + th - 1, DIM)
        s.fill(bx, y0 + ty + th, bx + bw - 1, y0 + h - 1, TRACK)


def games(s, pane_x, pane_w, sx, list_top, list_row_h, footer_y, pens):
    TEXT, DIM, WHITE, HL, RULE = pens
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


if __name__ == "__main__":
    main()
