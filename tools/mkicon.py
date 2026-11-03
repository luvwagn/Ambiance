#!/usr/bin/env python3
"""Write classic Workbench .info icons (OS 1.x/2.x/3.x readable, 4 colours).

Usage: python3 tools/mkicon.py <outdir>
Writes:
  AmigaMCE.info   tool icon for the program (stack 32768)
  drawer.info     drawer icon for the AmigaMCE drawer in the LHA
  Disk.info       disk icon for the ADF
  readme.info     project icon for readme.txt (default tool SYS:Utilities/MultiView)

The format is the DiskObject file layout from workbench/workbench.h:
DiskObject (78 bytes), optional OldDrawerData (56 bytes), the Image
structure (20 bytes) and its bitplanes, then the default tool and tool types.
Pens use the standard Workbench 2.x/3.x palette: 0 grey, 1 black, 2 white,
3 blue.
"""
import os
import struct
import sys

WB_DISKMAGIC = 0xE310
WBDISK, WBDRAWER, WBTOOL, WBPROJECT = 1, 2, 3, 4
NO_ICON_POSITION = 0x80000000

# 4-colour pixel art: '.' grey (0), 'k' black (1), 'w' white (2), 'b' blue (3)
TOOL_ART = [
    "..........................................",
    ".kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk.",
    ".kbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbk.",
    ".kbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbk.",
    ".kbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbk.",
    ".kbbbbbbbbbbbbbbwwbbbbbbbbbbbbbbbbbbbbbbk.",
    ".kbbbbbbbbbbbbbbwwwwbbbbbbbbbbbbbbbbbbbbk.",
    ".kbbbbbbbbbbbbbbwwwwwwbbbbbbbbbbbbbbbbbbk.",
    ".kbbbbbbbbbbbbbbwwwwwwwwbbbbbbbbbbbbbbbbk.",
    ".kbbbbbbbbbbbbbbwwwwwwbbbbbbbbbbbbbbbbbbk.",
    ".kbbbbbbbbbbbbbbwwwwbbbbbbbbbbbbbbbbbbbbk.",
    ".kbbbbbbbbbbbbbbwwbbbbbbbbbbbbbbbbbbbbbbk.",
    ".kbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbk.",
    ".kbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbk.",
    ".kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk.",
    "..........................................",
    "...kk...kk...kkkk...kkkkkk...............",
    "...kkk.kkk..kk..kk..kk...................",
    "...kk.k.kk..kk......kkkk.................",
    "...kk...kk..kk..kk..kk...................",
    "...kk...kk...kkkk...kkkkkk...............",
    "..........................................",
]

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

DISK_ART = [
    "..........................................",
    ".kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk..",
    ".kbbbbbbbkwwwwwwwwwwwwwwwwwwwkbbbbbbbbbbk.",
    ".kbbbbbbbkwwwwwwwwwwwwkkkwwwwkbbbbbbbbbbk.",
    ".kbbbbbbbkwwwwwwwwwwwwkkkwwwwkbbbbbbbbbbk.",
    ".kbbbbbbbkwwwwwwwwwwwwwwwwwwwkbbbbbbbbbbk.",
    ".kbbbbbbbbkkkkkkkkkkkkkkkkkkkbbbbbbbbbbbk.",
    ".kbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbk.",
    ".kbbbbbkkkkkkkkkkkkkkkkkkkkkkkkkkkkbbbbbk.",
    ".kbbbbbkwwwwwwwwwwwwwwwwwwwwwwwwwwkbbbbbk.",
    ".kbbbbbkwwwwwbbwwwwwwwwwwwwwwwwwwwkbbbbbk.",
    ".kbbbbbkwwwwwbbbbwwwwwwwwwwwwwwwwwkbbbbbk.",
    ".kbbbbbkwwwwwbbbbbbwwwwwwwwwwwwwwwkbbbbbk.",
    ".kbbbbbkwwwwwbbbbwwwwwwwwwwwwwwwwwkbbbbbk.",
    ".kbbbbbkwwwwwbbwwwwwwwwwwwwwwwwwwwkbbbbbk.",
    ".kbbbbbkwwwwwwwwwwwwwwwwwwwwwwwwwwkbbbbbk.",
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


def image(art):
    w, h, data = planes(art)
    # LeftEdge, TopEdge, Width, Height, Depth, ImageData(ptr), PlanePick, PlaneOnOff, NextImage
    return struct.pack(">hhhhhIBBI", 0, 0, w, h, 2, 1, 3, 0, 0) + data, w, h


def bstr(s):
    b = s.encode("latin-1") + b"\0"
    return struct.pack(">I", len(b)) + b


def diskobject(kind, art, default_tool=None, tooltypes=None, stack=0):
    img, w, h = image(art)
    gadget = struct.pack(">IhhhhHHHIIIiIHI",
                         0,            # NextGadget
                         0, 0, w, h,   # LeftEdge, TopEdge, Width, Height
                         0x0004,       # Flags: GFLG_GADGIMAGE, complement highlight
                         0x0003,       # Activation: RELVERIFY | GADGIMMEDIATE
                         0x0001,       # GadgetType: BOOLGADGET
                         1,            # GadgetRender (non-zero: image follows)
                         0,            # SelectRender
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
    out += img
    if default_tool:
        out += bstr(default_tool)
    if tooltypes:
        out += struct.pack(">I", (len(tooltypes) + 1) * 4)
        for t in tooltypes:
            out += bstr(t)
    return bytes(out)


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    files = {
        "AmigaMCE.info": diskobject(WBTOOL, TOOL_ART, stack=32768,
                                    tooltypes=["DONOTWAIT"]),
        "drawer.info": diskobject(WBDRAWER, DRAWER_ART),
        "Disk.info": diskobject(WBDISK, DISK_ART, default_tool="SYS:System/DiskCopy"),
        "readme.info": diskobject(WBPROJECT, README_ART, default_tool="SYS:Utilities/MultiView"),
    }
    for name, data in files.items():
        with open(os.path.join(outdir, name), "wb") as fh:
            fh.write(data)
        print(f"{name}: {len(data)} bytes")


if __name__ == "__main__":
    main()
