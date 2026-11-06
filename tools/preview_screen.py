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
    i32 = re.search(r"icons32\[\]\[16\] = \{(.*?)\};", src, re.S)    # no longer generated
    icons32 = [[int(v, 16) for v in re.findall(r"0x([0-9A-F]{8})", grp)]
               for grp in re.findall(r"\{([^}]*)\}", i32.group(1))] if i32 else []
    pal = arr("ui_palette32")[1:-1]
    palette = [(pal[i] >> 24, pal[i + 1] >> 24, pal[i + 2] >> 24) for i in range(0, len(pal), 3)]
    global MARKS16, MARKS32
    MARKS16 = [[int(v, 16) for v in re.findall(r"0x([0-9A-F]{4})", grp)] for grp in
               re.findall(r"\{([^}]*)\}", re.search(r"marks16\[\]\[11\] = \{(.*?)\};", src, re.S).group(1))]
    m32 = re.search(r"marks32\[\]\[11\] = \{(.*?)\};", src, re.S)
    MARKS32 = [[int(v, 16) for v in re.findall(r"0x([0-9A-F]{8})", grp)] for grp in
               re.findall(r"\{([^}]*)\}", m32.group(1))] if m32 else []
    logo_hi = arr("logo_hi") if "logo_hi[]" in src else []
    return arr("logo_lo"), logo_hi, dims, icons, icons32, palette


MARKS16, MARKS32 = [], []


def draw_mark(s, mark, x, y, pen, sx):
    """Mirror of gfx_mark(): MARK_ARROW = 1 ... MARK_RADIO_ON = 5."""
    rows = (MARKS32 if sx == 2 else MARKS16)[mark - 1]
    width, top = 11 * sx, 1 << (31 if sx == 2 else 15)
    for r, bits in enumerate(rows):
        for b in range(width):
            if bits & (top >> b):
                s.img.putpixel((x + b, y + r), s.pal[pen])


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
    # hires: 640 wide, tall pixels. lores, laced and rtg have
    # square pixels and use the lowres artwork 1:1 (laced/rtg: dense).
    hires = mode == "hires"
    # Every mode now draws the lowres artwork 1:1 (src/gfx.c), hires too;
    # set hiart to preview the unused two-columns-per-pixel hires artwork.
    hiart = False
    sx = 2 if hiart else 1
    W, H = {"hires": (640, 256), "lores": (320, 256), "laced": (640, 512), "rtg": (800, 600)}[mode]
    if video == "ntsc" and mode != "rtg":
        H = {256: 200, 512: 400}[H]
    fname = "amce-pixel-hi" if hiart else "amce-pixel"
    # laced screens: 1.25x the layout and their own font (src/gfx.c pixel_k)
    k = 1.25 if mode == "laced" and os.environ.get("OLD_LACED") is None else 1.0
    def GU(v):
        return (int(v * k * 256) + 128) >> 8
    font = parse_size_file(open(os.path.join(fonts, fname, "15" if k > 1 else "12"), "rb").read())
    s = Screen(W, H, pal, font)
    # menu column fits the longest label (src/gfx.c fit_menu)
    labels = ["Games", "Mods", "Demos", "Videos", "Pictures", "Music", "Radio", "Settings", "About"]
    menu_w = GU(23) * sx + max(s.textlen(n) for n in labels) + GU(6) * sx
    top, row_h, list_top, list_row_h, footer_y = GU(26), GU(18), GU(44), GU(14), H - GU(12)
    pane_x = menu_w + 6 * sx
    pane_w = W - pane_x - 4 * sx
    fh = font["ysize"]
    BG, BAR, RULE, TEXT, DIM, WHITE, HL, SELDIM, ACCENT, KEYCAP = range(10)  # noqa: F841

    s.fill(0, 0, W - 1, H - 1, BG)
    s.fill(0, 0, W - 1, 23, BAR)
    s.fill(0, 24, W - 1, 24, RULE)
    lw, lh = (dims["logo_hi_w"], dims["logo_hi_h"]) if hiart else (dims["logo_lo_w"], dims["logo_lo_h"])
    logo = logo_hi if hiart else logo_lo
    for j in range(lh):
        for i in range(lw):
            pen = logo[j * lw + i]
            if pen != BAR:
                s.img.putpixel((6 * sx + i, 4 + j), pal[pen])
    clock = "Tue 03 Nov  00:05"
    s.text(W - 8 * sx - s.textlen(clock), (24 - fh) // 2, clock, TEXT)

    # module order in art.c (icon index) and the main menu order from build_menu()
    mods = ["Games", "Mods", "Demos", "Videos", "Pictures", "Music", "Radio", "Settings", "About"]
    names = ["Games", "Demos", "Mods", "Music", "Radio", "Pictures", "Videos", "Settings", "About"]
    msel = names.index("About") if page == "about" else 0
    for i, name in enumerate(names):
        y = top + i * row_h
        sel = i == msel
        if sel:
            s.fill(0, y, menu_w - 1, y + row_h - 1, SELDIM)
        fg = WHITE if sel else TEXT
        for r in range(16):
            if hiart:
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
    elif page == "glide":
        glide(s, pane_x, pane_w, sx, list_top, list_row_h, footer_y, (TEXT, DIM, WHITE, HL, RULE, SELDIM))
    else:
        games(s, pane_x, pane_w, sx, list_top, list_row_h, footer_y, (TEXT, DIM, WHITE, HL, RULE))
    s.fill(0, footer_y - 1, W - 1, footer_y - 1, RULE)
    s.fill(0, footer_y, W - 1, H - 1, BAR)
    if page == "about":
        hint = "Up/Down  scroll      Left/Esc  back" if W >= 640 else "Up/Down scroll  Esc back"
    else:
        hint = ("Up/Down  move    Right/Fire  open    Return  play    Left/Esc  back" if W >= 640
                else "Fire open  Ret play  Esc back")
    s.text(6 * sx, footer_y + (12 - fh) // 2, hint, DIM, W - 12 * sx)

    # true aspect: a lowres pixel is 6x6 here, hires 3x6, laced 3x3, RTG 3x3
    scale_x, scale_y = {"hires": (3, 6), "lores": (6, 6), "laced": (3, 3), "rtg": (3, 3)}[mode]
    s.img.resize((W * scale_x // 2, H * scale_y // 2), Image.NEAREST).save(out)  # 1.5x for viewing
    print("wrote", out)


def _version():
    try:
        return open(os.path.join(os.path.dirname(__file__), "..", "build", "version")).readline().strip()
    except OSError:
        return "0.0.0"


ABOUT = [("line", "AmigaMCE " + _version()), ("line", "(c) 2026 Brian King, under the MIT licence"),
         ("line", ""),
         ("para", "Fonts: Pixelify Sans (c) 2021 The Pixelify Sans Project Authors, Galmuri "
                  "(c) 2019-2025 Lee Minseo, and Rubik (c) 2015 The Rubik Project Authors, all "
                  "under the SIL Open Font License 1.1. The amce-pixel and amce-galmuri fonts are "
                  "Modified Versions of Pixelify Sans and Galmuri."),
         ("line", ""), ("para", "Code: libnix C runtime (public domain). The EaglePlayer host follows "
                  "HippoPlayer's (MIT licence, (c) 2017 Kari-Pekka Koljonen)."), ("line", ""),
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
    s.text(pane_x, 28, "About > Credits", WHITE)
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


GLIDE = ["Daffy Duck", "Dalek Attack", "Damocles", "Dangerous Streets", "Dark Seed",
         "Darkmere", "Day of the Tentacle", "Death Mask", "Defender of the Crown",
         "Deluxe Galaga", "Desert Strike", "Deuteros", "Diggers", "Disposable Hero",
         "Dizzy Collection", "Dragon's Lair", "Dune II", "Dungeon Master"]


def glide(s, pane_x, pane_w, sx, list_top, row_h, footer_y, pens):
    """Mirror of the letter overlay in src/main.c (ov_build / ov_slice)."""
    TEXT, DIM, WHITE, HL, RULE, SELDIM = pens
    s.text(pane_x, 28, "Games > By Genre > Strategy", WHITE)
    s.fill(pane_x, 41, pane_x + pane_w - 1, 41, RULE)
    vis = (footer_y - 2 - list_top) // row_h
    rows_h = vis * row_h
    g = render(s.font, "D")
    ink = [y for y in range(g.height) if any(g.getpixel((x, y)) for x in range(g.width))]
    cols = [x for x in range(g.width) if any(g.getpixel((x, y)) for y in range(g.height))]
    top, bot, gw = ink[0], ink[-1], cols[-1] + 1
    sc = max(2, min(16, rows_h * 2 // 5 // (bot - top + 1)))
    ow, oh = gw * sc, (bot - top + 1) * sc
    ox, oy = pane_x + pane_w * 9 // 10 - ow, list_top + (rows_h - oh) // 2
    strip = pane_x + pane_w * 9 // 10 - (s.font["xsize"] + 1) * sc - 6 * sx
    sel = 6
    for r, label in enumerate(GLIDE[:vis]):     # rows stop before the letter's strip
        y = list_top + r * row_h
        hl = r == sel
        s.fill(pane_x - 2 * sx, y, strip - 1, y + row_h - 1, HL if hl else 0)
        s.text(pane_x, y + 1, label, WHITE if hl else TEXT, strip - pane_x - 4 * sx)
    for yy in range(oh):                        # the letter, stamped once in its strip
        for xx in range(ow):
            if g.getpixel((xx // sc, top + yy // sc)):
                s.img.putpixel((ox + xx, oy + yy), s.pal[DIM])


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
        col = (11 + 5) * sx                      # fixed marker column, as in mark_column()
        draw_mark(s, 1, pane_x, y + (list_row_h - 11) // 2, WHITE if sel else DIM, sx)
        s.text(pane_x + col, y + 1, label, WHITE if sel else TEXT, pane_w - col - 2 * sx)


if __name__ == "__main__":
    main()
