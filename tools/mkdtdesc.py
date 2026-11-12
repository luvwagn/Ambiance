#!/usr/bin/env python3
"""Write a DataTypes descriptor (DEVS:DataTypes/<name>): an IFF FORM DTYP
with a NAME and a DTHD chunk, as datatypes.library reads them.
AmigaMCE, MIT.

    python3 mkdtdesc.py <out> --name NAME --base BASE --group pict --id jpeg
                        --recog "ff d8 ff" [--pattern '#?'] [--priority N]
                        [--version '$VER: ...']

DTHD: struct DataTypeHeader (datatypes/datatypes.h), its STRPTRs and mask
pointer as offsets from the chunk's start (datatypes.library adds the
address it loads the chunk at), then the mask (WORDs: 0-255 a byte, -1
any byte), the name, the base name and the pattern, each ended with a 0.
The base name picks the class library: <base>.datatype in
SYS:Classes/DataTypes. A higher priority is tried first among
descriptors that match.
"""
import argparse
import struct
import sys

DTF_BINARY = 0


def chunk(cid, data):
    return cid + struct.pack(">I", len(data)) + data + (b"\0" if len(data) & 1 else b"")


def dthd(name, base, pattern, group, ident, recog, flags, priority):
    mask = b"".join(struct.pack(">h", v) for v in recog)
    o_mask = 32
    o_name = o_mask + len(mask)
    o_base = o_name + len(name) + 1
    o_pat = o_base + len(base) + 1
    head = struct.pack(">IIII4s4sHHHH", o_name, o_base, o_pat, o_mask,
                       group, ident, len(recog), 0, flags, priority)
    return head + mask + name + b"\0" + base + b"\0" + pattern + b"\0"


def fourcc(s):
    b = s.encode("latin-1")
    if len(b) != 4:
        sys.exit("mkdtdesc: %r is not four characters" % s)
    return b


def main():
    a = argparse.ArgumentParser()
    a.add_argument("out")
    a.add_argument("--name", required=True)
    a.add_argument("--base", required=True)
    a.add_argument("--group", required=True)
    a.add_argument("--id", required=True)
    a.add_argument("--recog", required=True, help='hex bytes, "??" for any')
    a.add_argument("--pattern", default="#?")
    a.add_argument("--priority", type=int, default=0)
    a.add_argument("--version")
    o = a.parse_args()
    recog = [-1 if f == "??" else int(f, 16) for f in o.recog.split()]
    if not recog or any(v > 255 for v in recog):
        sys.exit("mkdtdesc: bad --recog")
    if not 0 <= o.priority <= 65535:
        sys.exit("mkdtdesc: bad --priority")
    name = o.name.encode("latin-1")
    body = b"DTYP" + chunk(b"NAME", name)
    if o.version:
        body += chunk(b"FVER", o.version.encode("latin-1") + b"\0")
    body += chunk(b"DTHD", dthd(name, o.base.encode("latin-1"), o.pattern.encode("latin-1"),
                                fourcc(o.group), fourcc(o.id), recog, DTF_BINARY, o.priority))
    with open(o.out, "wb") as f:
        f.write(b"FORM" + struct.pack(">I", len(body)) + body)


if __name__ == "__main__":
    main()
