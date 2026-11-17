"""Will the translations fit? Measures every string of each catalog in the
fonts Ambiance draws with, on the screens it opens, and flags where a
translation runs out of room that its English had.

Usage: python3 -I textfit.py <catalogs-dir> <fonts-dir> [language ...]

Checked on 320 x 256 (Pixelify Sans 12), 640 x 256 (the same) and 640 x 512
with Galmuri11 Bold (16-pixel lines), with the layout's own sums (gfx.c's
layout_metrics and fit_menu, main.c's footer hints):
  - the menu column: its widest label, and what the pane loses to it
  - the footer hints: the long set is used when its widest fits, else the
    short set, each of which must then fit
  - one-line text wider than the pane (paragraphs wrap, so are not checked),
    or for a status message (set_msg, errors) wider than the screen
Glyphs are measured as TextLength does: each character's kern + space; Far
East characters at their advance in the glyph pack (Fonts/amb-cjk/12),
Greek at its own (Fonts/amb-cjk/greek).
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
        for u in s:
            if ord(u) > 255 and ord(u) in CJK:
                w += CJK[ord(u)]                # cjk.c: the pack's advance
                continue
            ch = u.encode("latin-1", "replace")[0]
            i = ch - f["lo"] if f["lo"] <= ch <= f["hi"] else self.n - 1
            if f["cs"]:
                w += self._i16(f["cs"], i)
            else:
                w += f["xsize"]
            if f["ck"]:
                w += self._i16(f["ck"], i)
        return w


def read_pack(path):
    """{code point: advance} of a Far East glyph pack (tools/mkcjk.py)."""
    d = open(path, "rb").read()
    n = struct.unpack(">L", d[10:14])[0]
    codes = struct.unpack(">%dH" % n, d[14:14 + 2 * n])
    return dict(zip(codes, d[14 + 2 * n:14 + 3 * n]))


CJK = {}        # the 12 pack: all three screens' fonts are under 20 pixels

SCREENS = [
    # name, width, font, k (design units: x * k / 256)
    ("320x256", 320, "amb-pixel/12", 256),
    ("640x256", 640, "amb-pixel/12", 256),
    ("640x512 Galmuri11 Bold", 640, "amb-galmuri11b/16", 16 * 256 // 12),
]


def U(v, k):
    return (v * k + 128) >> 8


def status_strings(srcdir):
    """English strings shown in the footer's status line (set_msg, an error
    passed on to it): those have the screen's width, not the pane's."""
    out = set()
    lit = catalog.LIT
    for fn in sorted(os.listdir(srcdir)):
        if not fn.endswith(".c"):
            continue
        lines = open(os.path.join(srcdir, fn), encoding="latin-1").read().split("\n")
        for i, l in enumerate(lines):
            ctx = " ".join(lines[max(0, i - 2):i + 1])
            if not any(k in ctx for k in ("set_msg(", "err,", "errmax", "fail(", "why",
                                           "st.err", "amb_strlcpy(m,", "sprintf(m,")):
                continue
            for m in catalog.MARK.finditer(l):
                out.add("".join(catalog.c_unescape(x) for x in lit.findall(m.group(1))))
    return out


STATUS = set()


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
            room_s = w - U(12, k) if en in STATUS else pane
            room_e = w - U(12, k) if en in STATUS else pane_en
            if ws > room_s and we <= room_e:
                out.append("%s: %d px, %s %d: \"%s\" (English %d)" %
                           (sname, ws, "status line" if en in STATUS else "pane", room_s, s, we))
    return out


def main():
    catdir, fonts_dir = sys.argv[1:3]
    want = sys.argv[3:]
    CJK.update(read_pack(os.path.join(fonts_dir, "amb-cjk", "12")))
    CJK.update(read_pack(os.path.join(fonts_dir, "amb-cjk", "greek")))
    STATUS.update(status_strings(os.path.join(os.path.dirname(os.path.abspath(catdir)), "src")))
    cd = catalog.read_cd(os.path.join(catdir, "Ambiance.cd"))
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
