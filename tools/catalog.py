"""AmigaMCE's translations: catalog descriptions and catalogs.

  python3 -I catalog.py scan  <srcdir> <catalogs-dir>
      every _("...") and N_("...") in <srcdir>/*.c into <catalogs-dir>/AmigaMCE.cd,
      and each <catalogs-dir>/<language>.ct brought up to date with it: new
      strings added untranslated (English as a comment), gone ones dropped
  python3 -I catalog.py build <catalogs-dir> <outdir> [language ...]
      each .ct (or those named) into <outdir>/<language>/AmigaMCE.catalog,
      after checking that each translation keeps its English's % formats
  python3 -I catalog.py check <catalogs-dir>
      counts per language, and what is untranslated

A string's ID is a hash of its English (FNV-1a, 31 bits, as src/lang.c
computes it at run time), named MSG_<hex> in the .cd and .ct files, which
are FlexCat's formats. The .ct files are kept in UTF-8 here; the catalogs
are Latin-1, with Japanese, Chinese and Korean characters in AmigaMCE's own
three bytes and Greek letters in two (src/cjk.c), so any of those languages
can be added too.
"""
import os
import re
import struct
import sys
import time
import unicodedata

LANGS = ["deutsch", "français", "italiano", "español", "svenska", "suomi", "dansk", "norsk",
         "nederlands", "português", "nihongo", "greek", "korean"]


def fnv(b):
    h = 2166136261
    for c in b:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h & 0x7FFFFFFF


ESC = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "e": "\x1b", "0": "\0"}


def c_unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        n = s[i + 1]
        if n == "x":
            m = re.match(r"[0-9A-Fa-f]{1,2}", s[i + 2:])
            out.append(chr(int(m.group(0), 16)))
            i += 2 + len(m.group(0))
        elif n in "01234567":
            m = re.match(r"[0-7]{1,3}", s[i + 1:])
            out.append(chr(int(m.group(0), 8)))
            i += 1 + len(m.group(0))
        else:
            out.append(ESC[n])
            i += 2
    return "".join(out)


def ct_escape(s):
    """For a .cd/.ct line: FlexCat's escapes."""
    s = (s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")
         .replace("\x1b", "\\e"))
    return "\\x3B" + s[1:] if s.startswith(";") else s     # not a comment line


def ct_unescape(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "x":
                out.append(chr(int(s[i + 2:i + 4], 16)))
                i += 4
                continue
            out.append({"n": "\n", "t": "\t", "e": "\x1b", "\\": "\\", "\"": "\""}.get(n, n))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


MARK = re.compile(r'\bN?_\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)')
LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')


def scan_src(srcdir):
    """{id: (english, [where])}, in order of first use."""
    found = {}
    for fn in sorted(os.listdir(srcdir)):
        if not fn.endswith(".c"):
            continue
        text = open(os.path.join(srcdir, fn), encoding="latin-1").read()
        # comments out (their newlines kept, for the line numbers)
        text = re.sub(r"/\*.*?\*/", lambda c: re.sub(r"[^\n]", " ", c.group(0)), text,
                      flags=re.S)
        for m in MARK.finditer(text):
            en = "".join(c_unescape(x) for x in LIT.findall(m.group(1)))
            i = fnv(en.encode("latin-1"))
            line = text.count("\n", 0, m.start()) + 1
            if i in found and found[i][0] != en:
                sys.exit("hash collision: %r and %r" % (found[i][0], en))
            found.setdefault(i, (en, []))[1].append("%s:%d" % (fn, line))
    return found


def read_cd(path):
    ids = {}
    if not os.path.exists(path):
        return ids
    lines = open(path, encoding="utf-8").read().split("\n")
    i = 0
    while i < len(lines):
        m = re.match(r"^(MSG_[0-9A-F]{8}) \((\d+)//\)$", lines[i])
        if m:
            ids[m.group(1)] = ct_unescape(lines[i + 1])
            i += 2
        else:
            i += 1
    return ids


def read_ct(path):
    """{name: translation} ('' untranslated), and the header lines."""
    tr, hdr = {}, []
    lines = open(path, encoding="utf-8").read().split("\n")
    i = 0
    while i < len(lines):
        l = lines[i]
        if l.startswith("##"):
            hdr.append(l)
            i += 1
            continue
        m = re.match(r"^(MSG_[0-9A-F]{8})$", l)
        if m:
            v = lines[i + 1] if i + 1 < len(lines) else ""
            if v.startswith(";"):
                v = ""                      # no translation line at all
                i += 1
            else:
                i += 2
            tr[m.group(1)] = ct_unescape(v)
            continue
        i += 1
    return tr, hdr


def name_of(i):
    return "MSG_%08X" % i


def cmd_scan(srcdir, catdir):
    found = scan_src(srcdir)
    os.makedirs(catdir, exist_ok=True)
    with open(os.path.join(catdir, "AmigaMCE.cd"), "w", encoding="utf-8") as fh:
        fh.write("; AmigaMCE's catalog description: made by tools/catalog.py scan from\n"
                 "; the _() and N_() strings in src/. Do not edit: edit the source.\n")
        for i, (en, where) in found.items():
            fh.write("; %s\n%s (%d//)\n%s\n" % (" ".join(where[:3]), name_of(i), i, ct_escape(en)))
    for fn in sorted(os.listdir(catdir)):
        if fn.endswith(".ct"):
            update_ct(os.path.join(catdir, fn), found)
    print("%d strings" % len(found))


def update_ct(path, found):
    tr, hdr = read_ct(path)
    lang = os.path.basename(path)[:-3]
    if not hdr:
        hdr = ["## version $VER: AmigaMCE.catalog 1.0 (%s)" % time.strftime("%d.%m.%Y"),
               "## language %s" % lang, "## codeset 0", ";"]
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(hdr) + "\n")
        for i, (en, where) in found.items():
            n = name_of(i)
            fh.write("%s\n" % n)
            if tr.get(n):
                fh.write("%s\n" % ct_escape(tr[n]))
            fh.write("; %s\n" % ct_escape(en))


def formats(s):
    return re.findall(r"%[-+ 0#]*\d*(?:\.\d+)?l?[a-zA-Z%]", s)


def amiga_bytes(s):
    """Latin-1, the Far East characters as AmigaMCE's three bytes, Greek as
    its two."""
    out = bytearray()
    for ch in s:
        c = ord(ch)
        if c < 0x100:
            out.append(c)
        elif c in (0x2018, 0x2019):
            out.append(0x27)
        elif c in (0x201C, 0x201D):
            out.append(0x22)
        elif c in (0x2013, 0x2014):
            out.append(0x2D)
        elif c == 0x2026:
            out += b"..."
        elif 0x380 <= c <= 0x3FF:
            # Greek: two bytes (src/cjk.c)
            out += bytes([0x80 | (c - 0x380) >> 6, 0x80 | (c & 63)])
        elif (0x1100 <= c <= 0x11FF or 0x3000 <= c <= 0x30FF or 0x3130 <= c <= 0x318F or
              0x31F0 <= c <= 0x31FF or 0x3400 <= c <= 0x4DBF or 0x4E00 <= c <= 0x9FFF or
              0xAC00 <= c <= 0xD7A3 or 0xFF00 <= c <= 0xFFEF):
            out += bytes([0x80 | c >> 11, 0xA0 | (c >> 6 & 31), 0x80 | (c & 63)])
        else:
            raise ValueError("U+%04X cannot be shown" % c)
    return bytes(out)


def chunk(tag, data):
    pad = b"\0" if len(data) & 1 else b""
    return tag + struct.pack(">L", len(data)) + data + pad


def cstr4(b):
    b += b"\0"
    return b + b"\0" * (-len(b) & 3)


def cmd_build(catdir, outdir, langs):
    cd = read_cd(os.path.join(catdir, "AmigaMCE.cd"))
    bad = 0
    for fn in sorted(os.listdir(catdir)):
        if not fn.endswith(".ct"):
            continue
        lang = fn[:-3]
        if langs and lang not in langs:
            continue
        tr, hdr = read_ct(os.path.join(catdir, fn))
        ver = next((h[11:].strip() for h in hdr if h.startswith("## version")),
                   "$VER: AmigaMCE.catalog 1.0")
        strs = b""
        n = 0
        for name in sorted(tr, key=lambda k: int(k[4:], 16)):
            t = tr[name]
            if not t or name not in cd:
                continue
            if formats(t) != formats(cd[name]):
                print("%s %s: formats %s, English has %s" % (lang, name, formats(t),
                                                              formats(cd[name])))
                bad += 1
                continue
            try:
                b = amiga_bytes(t)
            except ValueError as e:
                print("%s %s: %s" % (lang, name, e))
                bad += 1
                continue
            if len(b) > 2 * len(cd[name].encode("latin-1")) + 16:
                # src/lang.c would show the English: buffers allow no more
                print("%s %s: %d bytes, more than twice the English's and 16" % (lang, name, len(b)))
                bad += 1
                continue
            body = b + b"\0"
            strs += struct.pack(">LL", int(name[4:], 16), len(body)) + body + b"\0" * (-len(body) & 3)
            n += 1
        form = b"CTLG" + chunk(b"FVER", cstr4(ver.encode("latin-1"))) + \
            chunk(b"LANG", cstr4(lang.encode("latin-1"))) + \
            chunk(b"CSET", struct.pack(">L", 0) + b"\0" * 28) + chunk(b"STRS", strs)
        # the drawer's name in ASCII ("francais"): AmigaMCE knows a catalog by
        # the language in its LANG chunk, and an ASCII name survives any
        # file system (an emulator's host folder spelled "français" otherwise)
        ascii_name = unicodedata.normalize("NFKD", lang).encode("ascii", "ignore").decode()
        d = os.path.join(outdir.encode(), ascii_name.encode())
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, b"AmigaMCE.catalog"), "wb") as fh:
            fh.write(b"FORM" + struct.pack(">L", len(form)) + form)
        print("%s: %d of %d translated" % (lang, n, len(cd)))
    if bad:
        sys.exit("%d strings left out" % bad)


def cmd_check(catdir):
    cd = read_cd(os.path.join(catdir, "AmigaMCE.cd"))
    for fn in sorted(os.listdir(catdir)):
        if fn.endswith(".ct"):
            tr, _ = read_ct(os.path.join(catdir, fn))
            missing = [k for k in cd if not tr.get(k)]
            print("%s: %d of %d, missing %d" % (fn[:-3], len(cd) - len(missing), len(cd),
                                                len(missing)))


def main():
    a = sys.argv[1:]
    if a and a[0] == "scan":
        cmd_scan(a[1], a[2])
    elif a and a[0] == "build":
        cmd_build(a[1], a[2], a[3:])
    elif a and a[0] == "check":
        cmd_check(a[1])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
