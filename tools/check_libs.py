#!/usr/bin/env python3
"""Each library (and datatype) AmigaMCE builds, as AmigaOS will see it.

A library's version is in three places, and they must agree:

  - its Resident's rt_Version (src/sqlib/libinit.c's LIB_RT_VERSION):
    ramlib turns a library away, before it is ever opened, when this is
    older than the version asked for (OS 3.2 checks it; vamos does not);
  - lib_Version and lib_Revision, from the source's LibVersion and
    LibRevision, which OpenLibrary checks;
  - its $VER string, which Version and the Workbench show, and any other
    "name V.R" in it (its id string, the text a Version() function returns).

And the version AmigaMCE asks for (its header's define, or the number in
amce_open_own_lib's call) must be no newer than the library. mpegv.library
2.0 once went out with rt_Version 1 (a stale object) and would not open on
a real Amiga while every test passed.

Usage: check_libs.py library=source[=wanted] ...
  library  the built file (build/ac3lib/ac3.library)
  source   the C file with LibVersion, LibRevision and the $VER string
  wanted   the version AmigaMCE asks for (optional)
Exit 1 if any disagree.
"""
import re
import sys


def resident_version(data):
    """rt_Version of the first Resident (RTC_MATCHWORD whose rt_MatchTag
    points back at it), and its rt_Name's offset... version only."""
    i = data.find(b"\x4a\xfc")
    while i >= 0:
        # in a hunk file the MatchTag is a relocated pointer: take the first
        # MATCHWORD followed by the Resident's own shape (flags, version,
        # type NT_LIBRARY 9)
        if i + 14 <= len(data) and data[i + 12] == 9:
            return data[i + 11]
        i = data.find(b"\x4a\xfc", i + 2)
    return None


def ver_string(data, name):
    m = re.search(rb"\$VER: " + re.escape(name.encode()) + rb" (\d+)\.(\d+)", data)
    return (int(m.group(1)), int(m.group(2))) if m else None


def source_version(text, name):
    v = re.search(r"const\s+UWORD\s+LibVersion\s*=\s*(\d+)", text)
    r = re.search(r"const\s+UWORD\s+LibRevision\s*=\s*(\d+)", text)
    return (int(v.group(1)) if v else None, int(r.group(1)) if r else None)


def main(args):
    bad = 0
    for a in args:
        parts = a.split("=")
        lib, src = parts[0], parts[1]
        wanted = int(parts[2]) if len(parts) > 2 and parts[2] else None
        name = lib.rsplit("/", 1)[-1]
        data = open(lib, "rb").read()
        text = open(src, encoding="latin-1").read()
        rt = resident_version(data)
        vs = ver_string(data, name)
        sv, sr = source_version(text, name)
        why = []
        if rt is None:
            why.append("no Resident found")
        if vs is None:
            why.append("no $VER string naming it")
        if sv is None or sr is None:
            why.append("no LibVersion/LibRevision in " + src)
        if rt is not None and sv is not None and rt != sv:
            why.append("Resident says %d, LibVersion %d (ramlib checks the Resident)" % (rt, sv))
        if vs and sv is not None and sr is not None and vs != (sv, sr):
            why.append("$VER says %d.%d, LibVersion.LibRevision %d.%d" % (vs + (sv, sr)))
        # every other "name V.R" in it (an id string, its Version() text)
        short = name.rsplit(".", 1)[0]
        for m in re.finditer(rb"(?<![\w.])" + re.escape(short.encode()) + rb"(?:\.library|\.datatype)? (\d+)\.(\d+)\b", data):
            other = (int(m.group(1)), int(m.group(2)))
            if sv is not None and sr is not None and other != (sv, sr):
                why.append("%r says %d.%d" % (m.group(0).decode("latin-1"), other[0], other[1]))
                break
        if wanted is not None and sv is not None and wanted > sv:
            why.append("AmigaMCE asks for %d, the library is %d" % (wanted, sv))
        if why:
            bad += 1
            print("  %s: %s" % (name, "; ".join(why)))
        else:
            print("  CHECK %s %d.%d: Resident %d, $VER %d.%d%s ok" %
                  (name, sv, sr, rt, vs[0], vs[1], "" if wanted is None else ", asked for %d" % wanted))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
