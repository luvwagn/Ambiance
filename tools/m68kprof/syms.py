#!/usr/bin/env python3
"""syms.py exe obj... : every function's address in exe (code hunk offsets),
the locals too: each object's text placed by a global it shares with exe."""
import subprocess, sys
NM = "m68k-amigaos-nm"
def nm(f):
    out = []
    for l in subprocess.run([NM, f], capture_output=True, text=True).stdout.splitlines():
        p = l.split()
        if len(p) == 3 and p[1] in "tT":
            out.append((int(p[0], 16), p[1], p[2]))
    return out
exe = {n: a for a, t, n in nm(sys.argv[1]) if t == "T"}
res = {}
for o in sys.argv[2:]:
    s = nm(o)
    base = None
    for a, t, n in s:
        if t == "T" and n in exe:
            base = exe[n] - a
            break
    if base is None:
        continue
    for a, t, n in s:
        res[base + a] = n
for n, a in exe.items():
    res.setdefault(a, n)
for a in sorted(res):
    print(f"{a:08x} {res[a]}")
