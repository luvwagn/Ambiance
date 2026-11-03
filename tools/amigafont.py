"""Build Amiga diskfont files (contents .font + size file) from 1-bit glyphs.

Usage:
  python3 -I amigafont.py <glyphs.json> <outdir>

Writes, for each variant (lowres and hires):
  <outdir>/FONTS/<name>.font        FontContentsHeader with one entry
  <outdir>/FONTS/<name>/<ysize>     AmigaDOS load file holding DiskFontHeader + TextFont
and preview PNGs rendered by decoding those binaries again.

Format notes (graphics/text.h, libraries/diskfont.h):
  size file = HUNK_HEADER, one HUNK_CODE, HUNK_RELOC32, HUNK_END.
  code hunk = moveq #-1,d0; rts; DiskFontHeader { Node, FileID 0x0f80, Revision,
  Segment, Name[32], TextFont }, then CharData, CharLoc, CharSpace, CharKern.
"""
import json
import os
import struct
import sys

from PIL import Image

# Hand-drawn replacements. Rows are listed top to bottom; `top` is the first
# row relative to the baseline (cap height is rows -7..-1).
OVERRIDES = {
    "C": (-7, [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."]),
    "Z": (-7, ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"]),
    "0": (-7, [".###.", "#...#", "#...#", "#.#.#", "#...#", "#...#", ".###."]),
    "2": (-7, [".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"]),
    "5": (-7, ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."]),
    "j": (-7, ["...#", "....", "...#", "...#", "...#", "...#", "...#", "#..#", ".##."]),
}
NOTDEF = (-7, ["#####", "#...#", "#...#", "#...#", "#...#", "#...#", "#####"])
LO, HI = 32, 255
SPACE_ADV = 3          # word space, lowres pixels
GAP = 1                # gap after every glyph, lowres pixels


def load_glyphs(path):
    raw = json.load(open(path))
    glyphs = {}
    for c in range(LO, HI + 1):
        g = raw.get(str(c))
        if g and g["rows"]:
            glyphs[c] = (g["top"], g["rows"])
        else:
            glyphs[c] = None
    for ch, g in OVERRIDES.items():
        glyphs[ord(ch)] = g
    # The uppercase C override also applies to accented forms built on C.
    if glyphs.get(0xC7):
        top, rows = glyphs[0xC7]
        cap = OVERRIDES["C"][1]
        # keep the cedilla rows below the cap height, swap in the open C body
        body_start = -7 - top
        rows = rows[:body_start] + cap + rows[body_start + 7:]
        glyphs[0xC7] = (top, [r.ljust(5, ".")[:5] for r in rows])
    return glyphs


def build_variant(glyphs, hires):
    """Return dict with TextFont fields and byte arrays."""
    xs = 2 if hires else 1
    inked = [g for g in glyphs.values() if g]
    top = min(t for t, r in inked)
    bottom = max(t + len(r) for t, r in inked)
    top = min(top, NOTDEF[0])
    ysize = bottom - top
    baseline = -1 - top          # row index of the last row above the baseline

    def scaled(rows):
        return ["".join(ch * xs for ch in r) for r in rows]

    entries = []  # (rows padded to ysize, width, space, kern)
    for c in range(LO, HI + 1):
        g = glyphs[c]
        if c in (32, 160):
            entries.append(([], 0, SPACE_ADV * xs + (0 if not hires else -1), 0))
            continue
        if g is None:
            entries.append(([], 0, 0, 0))
            continue
        gtop, rows = g
        w = max(len(r) for r in rows)
        rows = [r.ljust(w, ".") for r in rows]
        full = ["." * w] * (gtop - top) + rows
        full += ["." * w] * (ysize - len(full))
        full = scaled(full)
        width = w * xs
        space = width + GAP          # hires: doubled glyph, single hires-pixel gap
        entries.append((full, width, space, 0))
    # not-defined glyph, appended after HI as the Amiga expects
    ntop, nrows = NOTDEF
    full = ["....."] * (ntop - top) + nrows
    full += ["....."] * (ysize - len(full))
    full = scaled(full)
    entries.append((full, 5 * xs, 5 * xs + GAP, 0))

    total_bits = sum(e[1] for e in entries)
    modulo = ((total_bits + 15) // 16) * 2
    bitmap = [bytearray(modulo) for _ in range(ysize)]
    locs = []
    bit = 0
    for rows, width, space, kern in entries:
        locs.append((bit, width))
        for y, r in enumerate(rows):
            for x, ch in enumerate(r):
                if ch == "#":
                    p = bit + x
                    bitmap[y][p >> 3] |= 0x80 >> (p & 7)
        bit += width
    n_adv = entries[ord("n") - LO][2]
    return {
        "ysize": ysize, "baseline": baseline, "xsize": n_adv, "modulo": modulo,
        "chardata": b"".join(bytes(r) for r in bitmap),
        "charloc": b"".join(struct.pack(">HH", o, w) for o, w in locs),
        "charspace": b"".join(struct.pack(">h", e[2]) for e in entries),
        "charkern": b"".join(struct.pack(">h", e[3]) for e in entries),
    }


def size_file(name, v):
    """Assemble the AmigaDOS load file for one font size."""
    FPF_DISKFONT, FPF_PROPORTIONAL, FPF_DESIGNED = 0x02, 0x20, 0x40
    NT_FONT = 12
    tf_off = 4 + 14 + 2 + 2 + 4 + 32            # = 58
    hdr_end = tf_off + 20 + 2 + 1 + 1 + 2 + 2 + 2 + 2 + 1 + 1 + 4 + 2 + 4 + 4 + 4  # = 110
    def pad4(b):
        return b + b"\0" * (-len(b) % 4)
    chardata_off = hdr_end
    charloc_off = chardata_off + len(v["chardata"])
    charspace_off = charloc_off + len(v["charloc"])
    charkern_off = charspace_off + len(v["charspace"])
    end = charkern_off + len(v["charkern"])
    name_off = 4 + 14 + 2 + 2 + 4               # dfh_Name = 26

    code = bytearray()
    code += struct.pack(">HH", 0x70FF, 0x4E75)  # moveq #-1,d0 ; rts
    # dfh_DF: struct Node (succ, pred, type, pri, name)
    code += struct.pack(">IIbbI", 0, 0, NT_FONT, 0, name_off)
    code += struct.pack(">HHI", 0x0F80, 1, 0)   # FileID, Revision, Segment
    code += name.encode("latin-1")[:31].ljust(32, b"\0")
    assert len(code) == tf_off
    # tf_Message: Node + ReplyPort + Length
    code += struct.pack(">IIbbIIH", 0, 0, NT_FONT, 0, name_off, 0, end - tf_off)
    code += struct.pack(">HBBHHHH", v["ysize"], 0,
                        FPF_DISKFONT | FPF_PROPORTIONAL | FPF_DESIGNED,
                        v["xsize"], v["baseline"], 1, 0)
    code += struct.pack(">BBIHIII", LO, HI, chardata_off, v["modulo"],
                        charloc_off, charspace_off, charkern_off)
    assert len(code) == hdr_end
    code += v["chardata"] + v["charloc"] + v["charspace"] + v["charkern"]
    assert len(code) == end
    code = pad4(bytes(code))
    relocs = [4 + 10, tf_off + 10, tf_off + 34, tf_off + 40, tf_off + 44, tf_off + 48]

    out = bytearray()
    nlongs = len(code) // 4
    out += struct.pack(">IIIII", 0x3F3, 0, 1, 0, 0) + struct.pack(">I", nlongs)
    out += struct.pack(">II", 0x3E9, nlongs) + code
    out += struct.pack(">III", 0x3EC, len(relocs), 0)
    out += b"".join(struct.pack(">I", r) for r in relocs) + struct.pack(">I", 0)
    out += struct.pack(">I", 0x3F2)
    return bytes(out)


def contents_file(dirname, ysize):
    FPF_DISKFONT, FPF_PROPORTIONAL, FPF_DESIGNED = 0x02, 0x20, 0x40
    entry = f"{dirname}/{ysize}".encode("latin-1").ljust(256, b"\0")
    entry += struct.pack(">HBB", ysize, 0, FPF_DISKFONT | FPF_PROPORTIONAL | FPF_DESIGNED)
    return struct.pack(">HH", 0x0F00, 1) + entry


# --- decoder, used only to check the files we wrote ---------------------------

def parse_size_file(data):
    pos = 0
    def rd():
        nonlocal pos
        v = struct.unpack_from(">I", data, pos)[0]
        pos += 4
        return v
    assert rd() == 0x3F3 and rd() == 0
    tab, first, last = rd(), rd(), rd()
    sizes = [rd() for _ in range(last - first + 1)]
    assert rd() == 0x3E9
    n = rd()
    code = data[pos:pos + n * 4]
    pos += n * 4
    assert rd() == 0x3EC
    relocs = []
    while True:
        cnt = rd()
        if cnt == 0:
            break
        rd()
        relocs += [rd() for _ in range(cnt)]
    assert rd() == 0x3F2 and pos == len(data)
    assert struct.unpack_from(">H", code, 18)[0] == 0x0F80
    tf = 58
    ysize, style, flags, xsize, baseline, smear, acc = struct.unpack_from(">HBBHHHH", code, tf + 20)
    lo, hi, cd, modulo, cl, cs, ck = struct.unpack_from(">BBIHIII", code, tf + 32)
    for r in relocs:
        assert struct.unpack_from(">I", code, r)[0] < len(code), "reloc points outside hunk"
    return dict(code=code, ysize=ysize, baseline=baseline, lo=lo, hi=hi, cd=cd,
                modulo=modulo, cl=cl, cs=cs, ck=ck, xsize=xsize, flags=flags,
                name=code[26:58].split(b"\0")[0].decode())


def render(font, text, scale_x=1):
    n = font["hi"] - font["lo"] + 2
    code = font["code"]
    def entry(c):
        i = c - font["lo"] if font["lo"] <= c <= font["hi"] else n - 1
        off, w = struct.unpack_from(">HH", code, font["cl"] + 4 * i)
        sp = struct.unpack_from(">h", code, font["cs"] + 2 * i)[0]
        kern = struct.unpack_from(">h", code, font["ck"] + 2 * i)[0]
        return off, w, sp, kern
    width = sum(entry(ord(c))[2] for c in text) + 2
    img = Image.new("1", (max(width, 1), font["ysize"]), 0)
    x = 0
    for c in text:
        off, w, sp, kern = entry(ord(c))
        for y in range(font["ysize"]):
            row = font["cd"] + y * font["modulo"]
            for i in range(w):
                p = off + i
                if code[row + (p >> 3)] & (0x80 >> (p & 7)):
                    img.putpixel((x + kern + i, y), 1)
        x += sp
    return img


def main():
    glyph_path, outdir = sys.argv[1], sys.argv[2]
    glyphs = load_glyphs(glyph_path)
    results = {}
    for name, hires in (("amce-pixel", False), ("amce-pixel-hi", True)):
        v = build_variant(glyphs, hires)
        fdir = os.path.join(outdir, "FONTS", name)
        os.makedirs(fdir, exist_ok=True)
        data = size_file(name + ".font", v)
        with open(os.path.join(fdir, str(v["ysize"])), "wb") as fh:
            fh.write(data)
        with open(os.path.join(outdir, "FONTS", name + ".font"), "wb") as fh:
            fh.write(contents_file(name, v["ysize"]))
        results[name] = (parse_size_file(data), v, len(data))
    for name, (font, v, n) in results.items():
        print(f"{name}: ysize {font['ysize']} baseline {font['baseline']} xsize {font['xsize']} "
              f"modulo {font['modulo']} chars {font['lo']}-{font['hi']} file {n} bytes")
    json.dump({k: {"ysize": r[0]["ysize"], "baseline": r[0]["baseline"]} for k, r in results.items()},
              open(os.path.join(outdir, "metrics.json"), "w"))


if __name__ == "__main__":
    main()
