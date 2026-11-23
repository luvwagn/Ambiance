#!/usr/bin/env python3
"""runs.py: m68kprof over zzprobe's six "own" cells (two photos, three
slideshow boxes), the counts grouped as jdec's and the shrink's, against the
real A3000/060 times (zzprobe 1.27); a least-squares fit of the clock and the
cache-miss cost. Prints the table and the fit; --save NAME keeps the counts."""
import json, os, subprocess, sys
O = "/tmp/m68kprof"
PROF = os.environ.get("PROF", O + "/m68kprof")
EXE = os.environ.get("EXE", O + "/t68k")
SYMS = os.environ.get("SYMS", O + "/t68k.syms")
CELLS = [("photo_1920x1080.jpg", 800, 600), ("photo_1920x1080.jpg", 1024, 768), ("photo_1920x1080.jpg", 1280, 1024),
         ("photo_4000x3000.jpg", 800, 600), ("photo_4000x3000.jpg", 1024, 768), ("photo_4000x3000.jpg", 1280, 1024)]
REAL = [(2921, 731), (5428, 1567), (5451, 2485), (9960, 1061), (16310, 2058), (16340, 3325)]  # ms: jdec, averaging
SHRINK = {"_shrink_out", "_shrink_rows", "_w_rows", "_shrink_done", "_shrink_new", "_shrink_free", "_shrink_src",
          "_shrink_out_asm", "_shrink_out68k"}
HARNESS = {"_calloc", "_memset", "_work_alloc", "_work_free", "_free", "_t_main", "_work_mark", "_w_read",
           "_start", "__stext", "_work_run", "_memmove"}
def run(photo, bw, bh, out=None):
    env = dict(os.environ, PROF_SYMS=SYMS, PROF_ALL="1")
    args = [PROF, EXE, f"{O}/ph/{photo}", str(bw), str(bh)] + ([out] if out else [])
    t = subprocess.run(args, capture_output=True, text=True, env=env).stdout.splitlines()
    g = {"jdec": [0] * 6, "shrink": [0] * 6, "copy": [0] * 6}
    fns = {}
    on = False
    for l in t:
        if l.startswith("function"):
            on = True
            continue
        if on:
            p = l.split()
            if len(p) < 8 or not p[1].isdigit():
                continue
            v = [int(x) for x in p[1:7]]
            fns[p[0]] = v
            k = "shrink" if p[0] in SHRINK else "copy" if p[0] == "_memcpy" else None if p[0] in HARNESS else "jdec"
            if k:
                g[k] = [a + b for a, b in zip(g[k], v)]
    return t[0], g, fns
def est(v, mhz, miss, mp=7.0):
    # v: instr, issue, mispred, dmiss, dwb, imiss
    return (v[1] + v[2] * mp + (v[3] + v[5]) * miss + v[4] * miss / 2) / (mhz * 1000.0)
if __name__ == "__main__":
    res = []
    for (ph, bw, bh), real in zip(CELLS, REAL):
        head, g, fns = run(ph, bw, bh, f"{O}/out_{ph[:-4]}_{bw}.rgb")
        res.append((ph, bw, head, g, fns, real))
        print(head, flush=True)
    if "--save" in sys.argv:
        json.dump(res, open(f"{O}/counts_{sys.argv[sys.argv.index('--save') + 1]}.json", "w"))
    # fit: real = (issue + 7 mp + miss*(dmiss + imiss + wb/2)) / mhz
    # (--use MHZ MISS: no fit, the baseline's numbers)
    if "--use" in sys.argv:
        i = sys.argv.index("--use")
        mhz, miss = float(sys.argv[i + 1]), float(sys.argv[i + 2])
        print(f"{'cell':28s} {'jdec real':>9s} {'model':>7s}   {'shrink real':>11s} {'model':>7s}   instr jdec / shrink")
        tj = ts = 0
        for ph, bw, head, g, fns, real in res:
            pj = est([a + b for a, b in zip(g["jdec"], g["copy"])], mhz, miss)
            ps = est(g["shrink"], mhz, miss)
            tj += pj; ts += ps
            print(f"{ph[:-4] + ' @' + str(bw):28s} {real[0]:9d} {pj:7.0f}   {real[1]:11d} {ps:7.0f}   {g['jdec'][0]:>11,} / {g['shrink'][0]:>11,}")
        print(f"totals: jdec {tj:.0f} ms, shrink {ts:.0f} ms (real 1.27: {sum(r[0] for r in REAL)} / {sum(r[1] for r in REAL)})")
        sys.exit(0)
    best = None
    for mhz10 in range(300, 1010, 5):
        for miss in range(0, 81, 1):
            e = 0.0
            for r in res:
                g, real = r[3], r[5]
                pj = est([a + b for a, b in zip(g["jdec"], g["copy"])], mhz10 / 10, miss)
                ps = est(g["shrink"], mhz10 / 10, miss)
                e += ((pj - real[0]) / real[0]) ** 2 + ((ps - real[1]) / real[1]) ** 2
            if best is None or e < best[0]:
                best = (e, mhz10 / 10, miss)
    e, mhz, miss = best
    print(f"fit: {mhz} MHz, a cache miss {miss} cycles (rms error {100 * (e / 12) ** 0.5:.1f}%)")
    print(f"{'cell':28s} {'jdec real':>9s} {'model':>7s}   {'shrink real':>11s} {'model':>7s}   instr jdec / shrink")
    for ph, bw, head, g, fns, real in res:
        pj = est([a + b for a, b in zip(g["jdec"], g["copy"])], mhz, miss)
        ps = est(g["shrink"], mhz, miss)
        print(f"{ph[:-4] + ' @' + str(bw):28s} {real[0]:9d} {pj:7.0f}   {real[1]:11d} {ps:7.0f}   {g['jdec'][0]:>11,} / {g['shrink'][0]:>11,}")
