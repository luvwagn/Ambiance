"""Will the translations fit? Measures every string of each catalog in the
fonts AmigaMCE draws with, on the screens it opens, and flags where a
translation runs out of room that its English had.

Usage: python3 -I textfit.py <catalogs-dir> <fonts-dir> [language ...]

Checked on 320 x 256 (Pixelify Sans 12), 640 x 256 (the same) and 640 x 512
with Galmuri11 Bold (16-pixel lines), with the layout's own sums (gfx.c's
layout_metrics and fit_menu, main.c's footer hints):
  - the menu column: its widest label, and what the pane loses to it
  - the footer hints: the long set is used when its widest fits, else the
    short set, each of which must then fit
  - one-line text wider than the pane (paragraphs wrap, so are not checked)
Glyphs are measured as TextLength does: each character's kern + space.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from amigafont import parse_size_file  # noqa: E402
import catalog  # noqa: E402

MENU = ["Games", "Mods", "Demos", "Videos", "Pictures", "Music", "Radio", "Settings", "About"]
# the footer: the widest long hint decides (main.c hint_text), then the rest
HINT_WIDEST = "Up/Down  move    Right/Fire  open    Return  play    Left/Esc  back"
HINT_PAIRS = [
    ("Up/Down  choose      Esc twice  quit", "Esc twice quit"),
    ("Up/Down  choose      Right/Fire  open      Esc twice  quit", "Fire open  Esc twice quit"),
    ("Up/Down  scroll      Fire/Return  view      Left/Esc  back", "Fire view  Esc back"),
    ("Up/Down  scroll      Fire/Return  play      Left/Esc  back", "Fire play  Esc back"),
    ("Up/Down  move    Fire/Return  view    Left/Esc  back", "Fire view  Esc back"),
    ("Up/Down  move    Fire  slideshow    Right  open    Left/Esc  back", "Fire show  Right open"),
    ("Up/Down  move    Fire/Right  open    Left/Esc  back", "Fire open  Esc back"),
    ("Up/Down  move    Fire/Return  play    Left/Esc  back", "Fire play  Esc back"),
    ("Up/Down  move    Fire  use this server    Left/Esc  back", "Fire use it  Esc back"),
    ("Up/Down  move    Fire/Return  play    Right  about it    Left/Esc  back",
     "Fire play  Right info"),
    ("Up/Down  move    Right/Fire  open    Left/Esc  back", "Fire open  Esc back"),
    ("Up/Down  move    Right  details    Fire/Return  play    Left/Esc  back", "Fire play  Esc back"),
    ("Up/Down  move    Right/Fire  open    Return  play    Left/Esc  back",
     "Fire open  Ret play  Esc back"),
    ("Fire/Return  play      Up/Down  scroll      Left/Esc  back", "Fire play  Esc back"),
    ("Left/Right  choose    Fire  press    Up/Down  volume    Space  pause    Esc  back",
     "Fire press  Up/Dn vol  Esc back"),
    ("Up/Down  scroll      Left/Esc  back", "Up/Down scroll  Esc back"),
]


class Font:
    def __init__(self, path):
        f = parse_size_file(open(path, "rb").read())
        self.f = f
        self.n = f["hi"] - f["lo"] + 2       # the last: for characters it lacks

    def _i16(self, off, i):
        return struct.unpack_from(">h", self.f["code"], off + 2 * i)[0]

    def width(self, s):
        f = self.f
        w = 0
        for ch in s.encode("latin-1", "replace"):
            i = ch - f["lo"] if f["lo"] <= ch <= f["hi"] else self.n - 1
            if f["cs"]:
                w += self._i16(f["cs"], i)
            else:
                w += f["xsize"]
            if f["ck"]:
                w += self._i16(f["ck"], i)
        return w


SCREENS = [
    # name, width, font, k (design units: x * k / 256)
    ("320x256", 320, "amce-pixel/12", 256),
    ("640x256", 640, "amce-pixel/12", 256),
    ("640x512 Galmuri11 Bold", 640, "amce-galmuri11b/16", 16 * 256 // 12),
]


def U(v, k):
    return (v * k + 128) >> 8


def check(lang, tr, cd_en, fonts_dir):
    def t(en):
        name = "MSG_%08X" % catalog.fnv(en.encode("latin-1"))
        return tr.get(name) or en
    out = []
    for sname, w, fpath, k in SCREENS:
        font = Font(os.path.join(fonts_dir, fpath))
        menu_en = U(23, k) + max(font.width(m) for m in MENU) + U(6, k)
        menu = U(23, k) + max(font.width(t(m)) for m in MENU) + U(6, k)
        pane = w - (menu + U(6, k)) - U(4, k)
        pane_en = w - (menu_en + U(6, k)) - U(4, k)
        if menu > menu_en:
            widest = max(MENU, key=lambda m: font.width(t(m)))
            out.append("%s: menu column %d px (English %d): \"%s\"; the pane loses %d px" %
                       (sname, menu, menu_en, t(widest), menu - menu_en))
        room = w - U(12, k)
        long_ok = font.width(t(HINT_WIDEST)) <= room
        long_ok_en = font.width(HINT_WIDEST) <= room
        for lng, sht in HINT_PAIRS:
            s = t(lng) if long_ok else t(sht)
            if font.width(s) > room:
                out.append("%s: footer %d px of %d: \"%s\"" % (sname, font.width(s), room, s))
        if long_ok_en and not long_ok:
            out.append("%s: footer falls back to the short hints (the long are %d px of %d)" %
                       (sname, font.width(t(HINT_WIDEST)), room))
        for name, en in cd_en.items():
            s = tr.get(name)
            if not s or len(en) > 90 or "\n" in en or en in MENU:
                continue
            if any(en in pair for pair in HINT_PAIRS) or en == HINT_WIDEST:
                continue
            ws, we = font.width(s), font.width(en)
            if ws > pane and we <= pane_en:
                out.append("%s: %d px, pane %d: \"%s\" (English %d)" % (sname, ws, pane, s, we))
    return out


def main():
    catdir, fonts_dir = sys.argv[1:3]
    want = sys.argv[3:]
    cd = catalog.read_cd(os.path.join(catdir, "AmigaMCE.cd"))
    total = 0
    for fn in sorted(os.listdir(catdir)):
        if not fn.endswith(".ct"):
            continue
        lang = fn[:-3]
        if want and lang not in want:
            continue
        tr, _ = catalog.read_ct(os.path.join(catdir, fn))
        res = check(lang, tr, cd, fonts_dir)
        print("%s: %s" % (lang, "fits" if not res else "%d to look at" % len(res)))
        for r in res:
            print("  " + r)
        total += len(res)
    return 0


if __name__ == "__main__":
    sys.exit(main())
