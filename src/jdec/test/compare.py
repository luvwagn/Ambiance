#!/usr/bin/env python3
"""Decode the corpus with jtest and compare with libjpeg (Pillow).
Ambiance, MIT.

    python3 compare.py <corpus dir> <jtest> [runner...]

runner: a prefix for jtest, e.g. "qemu-m68k -cpu m68020" for the 68020
build. Each file that should decode is decoded at 1/1, 1/2, 1/4 and 1/8:
- 1/1 against Pillow's own decode (libjpeg, fancy upsampling): PSNR and
  the largest difference;
- the smaller scales against Pillow's 1/1 decode averaged down (box), which
  is what jdec's reduced IDCTs compute (libjpeg's own scaled decode is no
  closer, and for 4:2:2 much further).
Files that should be refused must give their error and no crash.
Exit 1 on any failure.
"""
import math
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

FULL_PSNR, FULL_MAXDIFF = 38.0, 24     # jdec's 16-bit AAN IDCT against libjpeg's islow
SCALED_PSNR = 36.0                      # against libjpeg's full decode averaged down,
                                        # or its own scaled decode
SMALL_PSNR = 22.0                       # pictures under 4 blocks of the scale across
PROG_EIGHTH_PSNR = 30.0                 # progressive at 1/8 with chroma subsampled: jdec
                                        # keeps every component's DC alone (chroma then made
                                        # bigger, softer at colour edges), so the AC scans
                                        # are passed over and memory is 2 bytes a block


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)


def prog_sub(path):
    """A progressive JPEG whose chroma is subsampled"""
    im = Image.open(path)
    if not (im.info.get("progressive") or im.info.get("progression")) or len(im.layer) != 3:
        return False
    return any(h != im.layer[0][1] or v != im.layer[0][2] for _, h, v, _ in im.layer[1:])


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(maxsplit=4)
    w, h = int(parts[1]), int(parts[2])
    pix = np.frombuffer(parts[4][: w * h * 3], np.uint8)
    return pix.reshape(h, w, 3)


def main():
    corpus, jtest = sys.argv[1], sys.argv[2]
    runner = sys.argv[3:]
    fails = 0
    worst = {}
    with open(os.path.join(corpus, "expect.txt")) as f:
        cases = [l.split() for l in f if l.strip()]
    with tempfile.TemporaryDirectory() as tmp:
        for name, want in cases:
            path = os.path.join(corpus, name)
            out = os.path.join(tmp, "o.ppm")
            if want != "ok":
                r = subprocess.run(runner + [jtest, "-i", path], capture_output=True, text=True)
                got = r.stdout.strip().rsplit(":", 1)[-1].strip() if r.stdout else "?"
                if r.returncode not in (0, 3) or got != want:
                    print("FAIL %s: wanted %s, got %s (exit %d)" % (name, want, got, r.returncode))
                    fails += 1
                continue
            ref = np.asarray(Image.open(path).convert("RGB"))
            for scale in range(4):
                r = subprocess.run(runner + [jtest, path, out, str(scale)], capture_output=True, text=True)
                if r.returncode != 0:
                    print("FAIL %s 1/%d: exit %d %s" % (name, 1 << scale, r.returncode, r.stderr.strip()))
                    fails += 1
                    continue
                got = read_ppm(out)
                k = 1 << scale
                h, w = ref.shape[:2]
                ow, oh = (w + k - 1) // k, (h + k - 1) // k
                if got.shape != (oh, ow, 3):
                    print("FAIL %s 1/%d: size %s, wanted %dx%d" % (name, k, got.shape, ow, oh))
                    fails += 1
                    continue
                if scale == 0:
                    p = psnr(got, ref)
                    md = int(np.abs(got.astype(int) - ref.astype(int)).max())
                    bad = p < FULL_PSNR or md > FULL_MAXDIFF
                    tag = "1/1"
                else:
                    # the full decode averaged down: what jdec's reduced IDCTs are
                    # (each pixel the average of those it covers). Small pictures
                    # differ at their edges: a box at the edge averages only the
                    # pixels there, a JPEG block also its padding.
                    ref_s = np.asarray(Image.fromarray(ref).reduce(k))
                    small = min(w, h) < 8 * k * 4
                    limit, tag = (SMALL_PSNR, "1/%d small" % k) if small else (SCALED_PSNR, "1/%d" % k)
                    if k == 8 and not small and prog_sub(path):
                        limit, tag = PROG_EIGHTH_PSNR, "1/8 progressive, chroma subsampled"
                    p = psnr(got, ref_s)
                    md = int(np.abs(got.astype(int) - ref_s.astype(int)).max())
                    if p < limit:       # or libjpeg's own scaled decode, where Pillow gives it
                        im = Image.open(path)
                        im.draft("RGB", (max(1, w // k), max(1, h // k)))
                        lj = np.asarray(im.convert("RGB"))
                        if lj.shape == got.shape:
                            p = max(p, psnr(got, lj))
                    bad = p < limit
                if tag not in worst or p < worst[tag][0]:
                    worst[tag] = (p, name, md)
                if bad:
                    print("FAIL %s %s: PSNR %.1f dB, max diff %d" % (name, tag, p, md))
                    fails += 1
    for tag in sorted(worst):
        p, name, md = worst[tag]
        print("worst %s: %.1f dB (%s, max diff %d)" % (tag, p, name, md))
    print("%d cases, %d failures" % (len(cases), fails))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
