#!/usr/bin/env python3
"""Write Workbench .info icons: classic 4-colour images (OS 1.x-3.x), each
with a selected image of its own (complement highlighting inverts all of a
deeper Workbench's bitplanes: colours from anywhere in its palette), and
for the program and the drawer a GlowIcon too (OS 3.5+ shows it instead:
256 colours).

Usage: python3 tools/mkicon.py <outdir>
Writes:
  AmigaMCE.info   tool icon for the program (stack 32768): the logo's play
                  badge, a classic image and a GlowIcon (FORM ICON: FACE and
                  two IMAG chunks, run-length coded, after the classic data)
  drawer.info     drawer icon for the AmigaMCE drawer in the LHA
  readme.info     project icon for readme.txt (default tool SYS:Utilities/MultiView)
  install.info    project icon for the Installer script (default tool Installer,
                  found on the Workbench's path: C: or SYS:Utilities)

The format is the DiskObject file layout from workbench/workbench.h:
DiskObject (78 bytes), optional OldDrawerData (56 bytes), the Image
structure (20 bytes) and its bitplanes, then the default tool and tool types.
Pens use the standard Workbench 2.x/3.x palette: 0 grey, 1 black, 2 white,
3 blue.
"""
import os
import struct
import sys

from PIL import Image, ImageDraw

# ------------------------------------------------------------ the badge
# The logo's play badge (tools/mksmooth.py): a rounded square in the UI's
# highlight blue, a white play triangle in it. In units of a 14 x 14 box.
HL = (0x22, 0x66, 0xCC)
HL_LIGHT = (0x44, 0x88, 0xEE)
HL_DARK = (0x11, 0x44, 0x99)
WHITE = (0xFF, 0xFF, 0xFF)


def badge(size, sx=1.0, sy=1.0, ss=8, light=False):
    """RGBA image of the badge, size x size units scaled by sx, sy (pixels a
    unit), rendered ss times over and shrunk (anti-aliased)."""
    w, h = round(14 * sx), round(14 * sy)
    big = Image.new("RGBA", (w * ss, h * ss), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    kx, ky = sx * ss, sy * ss
    r = 2.6 * min(kx, ky)
    top, bot = (HL_LIGHT, HL) if not light else ((0x66, 0xAA, 0xFF), HL_LIGHT)
    # a soft top-to-bottom shade, then the rim and the triangle
    mask = Image.new("L", big.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, big.size[0] - 1, big.size[1] - 1], radius=r,
                                           fill=255)
    grad = Image.new("RGBA", big.size)
    gd = ImageDraw.Draw(grad)
    for y in range(big.size[1]):
        t = y / max(1, big.size[1] - 1)
        c = tuple(round(top[i] + (bot[i] - top[i]) * t) for i in range(3))
        gd.line([(0, y), (big.size[0], y)], fill=c + (255,))
    big.paste(grad, (0, 0), mask)
    d.rounded_rectangle([0, 0, big.size[0] - 1, big.size[1] - 1], radius=r, outline=HL_DARK + (255,),
                        width=max(1, round(0.6 * min(kx, ky))))
    d.polygon([(4.7 * kx, 3.9 * ky), (11.0 * kx, 7.0 * ky), (4.7 * kx, 10.1 * ky)],
              fill=WHITE + (255,))
    return big.resize((w, h), Image.LANCZOS)


def badge_art():
    """The classic 4-colour tool icon: the badge on Workbench's hires grid
    (pixels twice as tall as wide), 36 x 18, a black rim, snapped to pens."""
    im = badge(14, sx=36 / 14, sy=18 / 14, ss=8)
    px = im.load()
    w, h = im.size
    rows = []
    for y in range(h):
        row = ""
        for x in range(w):
            r, g, b, a = px[x, y]
            if a < 110:
                row += "."
            elif r > 150 and g > 160:          # half white or more: white
                row += "w"
            else:
                row += "b"
        rows.append(row)
    # a black rim around the badge, as Workbench icons have
    out = [list("." * (w + 4)) for _ in range(h + 2)]
    for y in range(h):
        for x in range(w):
            out[y + 1][x + 2] = rows[y][x]
    rim = [r[:] for r in out]
    for y in range(h + 2):
        for x in range(w + 4):
            if out[y][x] == "." and any(
                    0 <= y + dy < h + 2 and 0 <= x + dx < w + 4 and out[y + dy][x + dx] != "."
                    for dy, dx in ((0, -1), (0, 1), (-1, 0), (1, 0))):
                rim[y][x] = "k"
    # the triangle by hand: a row is twice as tall as a pixel is wide, so a
    # traced one is all steps; this one widens by 3 a row to a single tip
    for y in range(len(rim)):
        for x in range(len(rim[y])):
            if rim[y][x] == "w":
                rim[y][x] = "b"
    for i, wd in enumerate((2, 5, 8, 11, 14, 11, 8, 5, 2)):
        for x in range(15, 15 + wd):
            rim[5 + i][x] = "w"
    return ["".join(r) for r in rim]


# ------------------------------------------------------------ GlowIcon
def rle(values, depth):
    """GlowIcon run-length coding: a bit stream of 8-bit controls (0..127:
    that many + 1 values follow; 129..255: the next value 257 - n times)
    and depth-bit values."""
    bits = []

    def put(v, n):
        for i in range(n - 1, -1, -1):
            bits.append((v >> i) & 1)
    i = 0
    n = len(values)
    while i < n:
        j = i
        while j + 1 < n and values[j + 1] == values[i] and j - i < 126:
            j += 1
        if j - i >= 2:
            put(257 - (j - i + 1), 8)
            put(values[i], depth)
            i = j + 1
            continue
        k = i
        lit = []
        while k < n and len(lit) < 128:
            if k + 2 < n and values[k] == values[k + 1] == values[k + 2]:
                break
            lit.append(values[k])
            k += 1
        put(len(lit) - 1, 8)
        for v in lit:
            put(v, depth)
        i = k
    while len(bits) % 8:
        bits.append(0)
    return bytes(int("".join(map(str, bits[b:b + 8])), 2) for b in range(0, len(bits), 8))


def imag(im):
    """An IMAG chunk for RGBA im: colour 0 transparent, the rest its colours."""
    w, h = im.size
    rgb = Image.new("RGB", (w, h), (0, 0, 0))
    rgb.paste(im, (0, 0), im)
    q = rgb.quantize(colors=63, method=Image.MEDIANCUT)
    pal = q.getpalette()[:63 * 3]
    qp, ap = q.load(), im.load()
    vals = []
    used = 0
    for y in range(h):
        for x in range(w):
            if ap[x, y][3] < 128:
                vals.append(0)
            else:
                v = qp[x, y] + 1
                vals.append(v)
                used = max(used, v)
    ncol = used + 1
    depth = max(1, (ncol - 1).bit_length())
    data = rle(vals, depth)
    palette = bytes([0, 0, 0]) + bytes(pal[:(ncol - 1) * 3])
    pdata = rle(list(palette), 8)
    head = struct.pack(">BBBBBBHH", 0, ncol - 1, 0x03, 1, 1, depth, len(data) - 1, len(pdata) - 1)
    body = head + data + pdata
    return b"IMAG" + struct.pack(">I", len(body)) + body + (b"\0" if len(body) & 1 else b""), ncol


def glowicon(normal, selected):
    w, h = normal.size
    i1, n1 = imag(normal)
    i2, n2 = imag(selected)
    face = struct.pack(">BBBBH", w - 1, h - 1, 0, 0x11, max(n1, n2) * 3 - 1)
    body = b"ICON" + b"FACE" + struct.pack(">I", len(face)) + face + i1 + i2
    return b"FORM" + struct.pack(">I", len(body)) + body


def tool_glow():
    """46 x 46: the badge, 42 x 42, centred; selected, lighter (Workbench
    adds its own glow)."""
    out = []
    for light in (False, True):
        canvas = Image.new("RGBA", (46, 46), (0, 0, 0, 0))
        canvas.alpha_composite(badge(14, sx=42 / 14, sy=42 / 14, light=light), (2, 2))
        out.append(canvas)
    return out

def drawer_glow():
    """46 x 40: a drawer, its tab on the left, the badge on its front;
    selected, lighter (Workbench adds its own glow)."""
    out = []
    for light in (False, True):
        ss = 8
        big = Image.new("RGBA", (46 * ss, 40 * ss), (0, 0, 0, 0))
        d = ImageDraw.Draw(big)
        body = (0xC8, 0xC8, 0xC8) if not light else (0xE4, 0xE4, 0xE4)
        tab = (0xB0, 0xB0, 0xB0) if not light else (0xD0, 0xD0, 0xD0)
        edge = (0x33, 0x33, 0x33)
        lw = 2 * ss // 2 + ss // 2
        d.rounded_rectangle([3 * ss, 2 * ss, 18 * ss, 10 * ss], radius=2 * ss, fill=tab + (255,),
                            outline=edge + (255,), width=lw)
        d.rounded_rectangle([1 * ss, 6 * ss, 45 * ss - 1, 39 * ss - 1], radius=3 * ss,
                            fill=body + (255,), outline=edge + (255,), width=lw)
        d.line([(3 * ss, 8 * ss), (43 * ss, 8 * ss)], fill=WHITE + (255,), width=ss)
        im = big.resize((46, 40), Image.LANCZOS)
        im.alpha_composite(badge(14, sx=22 / 14, sy=22 / 14, light=light), (12, 12))
        out.append(im)
    return out


WB_DISKMAGIC = 0xE310
WBDISK, WBDRAWER, WBTOOL, WBPROJECT = 1, 2, 3, 4
NO_ICON_POSITION = 0x80000000

# 4-colour pixel art: '.' grey (0), 'k' black (1), 'w' white (2), 'b' blue (3).
# Drawn for hires Workbench, where a row is twice as tall as a pixel is
# wide.
# The tool icon is drawn from the logo's play badge instead (badge_art).
DRAWER_ART = [
    "..........................................",
    "...kkkkkkkkkkk............................",
    "..kwwwwwwwwwwwk...........................",
    ".kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk.",
    ".kwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwk.",
    ".kw....................................wk.",
    ".kw..............bb....................wk.",
    ".kw..............bbbb..................wk.",
    ".kw..............bbbbbb................wk.",
    ".kw..............bbbbbbbb..............wk.",
    ".kw..............bbbbbb................wk.",
    ".kw..............bbbb..................wk.",
    ".kw..............bb....................wk.",
    ".kw....................................wk.",
    ".kw....................................wk.",
    ".kwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwk.",
    ".kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk.",
    "..........................................",
]

README_ART = [
    "......................",
    "..kkkkkkkkkkkkkkk.....",
    "..kwwwwwwwwwwwwwkk....",
    "..kwwwwwwwwwwwwwkwk...",
    "..kwkkkkkkkkkwwwkkkk..",
    "..kwwwwwwwwwwwwwwwwk..",
    "..kwkkkkkkkkkkkkkwwk..",
    "..kwwwwwwwwwwwwwwwwk..",
    "..kwkkkkkkkkkkkkwwwk..",
    "..kwwwwwwwwwwwwwwwwk..",
    "..kwkkkkkkkkkkkkkwwk..",
    "..kwwwwwwwwwwwwwwwwk..",
    "..kwkkkkkkkwwwwwwwwk..",
    "..kwwwwwwwwwwwwwwwwk..",
    "..kkkkkkkkkkkkkkkkkk..",
    "......................",
]

PEN = {".": 0, "k": 1, "w": 2, "b": 3}


def planes(art):
    w = max(len(r) for r in art)
    h = len(art)
    words = (w + 15) // 16
    out = bytearray()
    for plane in range(2):
        for row in art:
            row = row.ljust(w, ".")
            bits = 0
            for x in range(words * 16):
                bits <<= 1
                if x < w and (PEN[row[x]] >> plane) & 1:
                    bits |= 1
            out += bits.to_bytes(words * 2, "big")
    return w, h, bytes(out)


def selected_art(art):
    """The selected image: what complement highlighting showed on a 4-colour
    Workbench (each pen p as 3 - p), drawn as an image of its own, the
    background round the shape left as it is. Complement itself inverts
    every bitplane of the screen: on a deeper Workbench, colours from
    anywhere in its palette."""
    w = max(len(r) for r in art)
    g = [list(r.ljust(w, ".")) for r in art]
    h = len(g)
    outside = set()
    todo = [(x, y) for y in range(h) for x in (0, w - 1)] + [(x, y) for x in range(w) for y in (0, h - 1)]
    while todo:
        x, y = todo.pop()
        if (x, y) in outside or not (0 <= x < w and 0 <= y < h) or g[y][x] != ".":
            continue
        outside.add((x, y))
        todo += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
    inv = {".": "b", "k": "w", "w": "k", "b": "."}
    return ["".join(c if (x, y) in outside else inv[c] for x, c in enumerate(row))
            for y, row in enumerate(g)]


def image(art):
    w, h, data = planes(art)
    # LeftEdge, TopEdge, Width, Height, Depth, ImageData(ptr), PlanePick, PlaneOnOff, NextImage
    return struct.pack(">hhhhhIBBI", 0, 0, w, h, 2, 1, 3, 0, 0) + data, w, h


def bstr(s):
    b = s.encode("latin-1") + b"\0"
    return struct.pack(">I", len(b)) + b


def diskobject(kind, art, default_tool=None, tooltypes=None, stack=0, glow=None):
    img, w, h = image(art)
    simg, _, _ = image(selected_art(art))
    gadget = struct.pack(">IhhhhHHHIIIiIHI",
                         0,            # NextGadget
                         0, 0, w, h,   # LeftEdge, TopEdge, Width, Height
                         0x0006,       # Flags: GFLG_GADGIMAGE, GFLG_GADGHIMAGE (its own selected image)
                         0x0003,       # Activation: RELVERIFY | GADGIMMEDIATE
                         0x0001,       # GadgetType: BOOLGADGET
                         1,            # GadgetRender (non-zero: image follows)
                         1,            # SelectRender (non-zero: its image follows the first)
                         0,            # GadgetText
                         0,            # MutualExclude
                         0,            # SpecialInfo
                         0,            # GadgetID
                         0)            # UserData: 0 = classic layout, no extended drawer data
    assert len(gadget) == 44
    do = struct.pack(">HH", WB_DISKMAGIC, 1) + gadget + struct.pack(
        ">BBIIIIIII", kind, 0,
        1 if default_tool else 0,
        1 if tooltypes else 0,
        NO_ICON_POSITION, NO_ICON_POSITION,
        1 if kind in (WBDISK, WBDRAWER) else 0,
        0,                                   # ToolWindow
        stack)
    assert len(do) == 78, len(do)
    out = bytearray(do)
    if kind in (WBDISK, WBDRAWER):
        # OldDrawerData: NewWindow (48 bytes) + CurrentX/Y
        nw = struct.pack(">hhhhBBIIIIIIIhhHHH", 50, 30, 400, 120, 255, 255, 0, 0x0240027F & 0xFFFFFFFF,
                         0, 0, 0, 0, 0, 90, 40, 0xFFFF, 0xFFFF, 1)
        assert len(nw) == 48, len(nw)
        out += nw + struct.pack(">ii", 0, 0)
    out += img + simg
    if default_tool:
        out += bstr(default_tool)
    if tooltypes:
        out += struct.pack(">I", (len(tooltypes) + 1) * 4)
        for t in tooltypes:
            out += bstr(t)
    if glow:
        out += glow                     # OS 3.5+: read after the classic parts
    return bytes(out)


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    files = {
        "AmigaMCE.info": diskobject(WBTOOL, badge_art(), stack=32768,
                                    tooltypes=["DONOTWAIT", "(WINDOWED)"], glow=glowicon(*tool_glow())),
        "drawer.info": diskobject(WBDRAWER, DRAWER_ART, glow=glowicon(*drawer_glow())),
        "readme.info": diskobject(WBPROJECT, README_ART, default_tool="SYS:Utilities/MultiView"),
        "install.info": diskobject(WBPROJECT, badge_art(), default_tool="Installer", stack=16384,
                                   tooltypes=["APPNAME=AmigaMCE", "MINUSER=AVERAGE"],
                                   glow=glowicon(*tool_glow())),
    }
    for name, data in files.items():
        with open(os.path.join(outdir, name), "wb") as fh:
            fh.write(data)
        print(f"{name}: {len(data)} bytes")


if __name__ == "__main__":
    main()
