"""Build Ambiance's Greek glyph pack (Fonts/amb-cjk/greek) for the pixel
style, and the Greek extract of Galmuri11 it is made from.

Usage: python3 -I mkgreek.py <Galmuri11.bdf> <out file>
       python3 -I mkgreek.py --extract <Galmuri11.bdf> <out .bdf>

The pack is mkcjk.py's format (see there), with Galmuri11's Greek letters,
U+0384-U+03CE: what modern Greek is written with. Ambiance keeps a Greek
letter as two bytes (src/cjk.c) and draws it from this pack on screens
whose own font has no Greek (the pixel style's, and smooth files made
without --greek). It is small and always installed: Greek is never shown
as a tag.

--extract writes those glyphs alone as a BDF (assets/fonts-src keeps that,
not the whole of Galmuri11, as it does its Latin-1 extracts).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkcjk    # noqa: E402

LO, HI = 0x384, 0x3CE


def greek(c):
    return LO <= c <= HI


def extract(src, dst):
    """The BDF's header and its Greek glyphs alone."""
    out, head, glyph, keep, n = [], True, [], False, 0
    with open(src, encoding="latin-1") as fh:
        for line in fh:
            t = line.split()
            if head:
                if t and t[0] == "CHARS":
                    head = False
                    continue
                out.append(line)
                continue
            if t and t[0] == "STARTCHAR":
                glyph, keep = [line], False
                continue
            if glyph:
                glyph.append(line)
                if t and t[0] == "ENCODING":
                    keep = greek(int(t[1]))
                if t and t[0] == "ENDCHAR":
                    if keep:
                        out += glyph
                        n += 1
                    glyph = []
    i = next(k for k, l in enumerate(out) if l.startswith("STARTCHAR"))
    out.insert(i, "CHARS %d\n" % n)
    out.append("ENDFONT\n")
    with open(dst, "w", encoding="latin-1") as fh:
        fh.writelines(out)
    print("%s: %d Greek glyphs" % (dst, n))


def main():
    if sys.argv[1] == "--extract":
        extract(sys.argv[2], sys.argv[3])
        return
    mkcjk.wanted = greek
    glyphs = mkcjk.read_bdf(sys.argv[1])
    missing = [c for c in range(LO, HI + 1) if c not in glyphs and c not in (0x385, 0x387,
               0x38B, 0x38D, 0x3A2)]
    if missing:
        sys.exit("no glyph for " + ", ".join("U+%04X" % c for c in missing))
    n, cw, ch, asc, size = mkcjk.write_pack(sys.argv[2], glyphs)
    print("greek: %d glyphs, cell %d x %d (%d above the baseline), %d bytes" %
          (n, cw, ch, asc, size))


if __name__ == "__main__":
    main()
