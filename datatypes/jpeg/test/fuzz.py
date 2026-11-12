#!/usr/bin/env python3
"""Fuzz jdec: damaged JPEGs must decode or be refused, never crash, hang,
leak or touch memory they should not. AmigaMCE, MIT.

    python3 fuzz.py <corpus dir> <jtest built with -fsanitize=address,undefined> [runs] [seed]

Each run takes a corpus file and damages it (bit flips, bytes overwritten,
cut short, bytes inserted or removed, a marker spliced in, a header
length changed), then decodes it at a random scale. jtest must exit 0
(decoded), 2 (decoded, damaged) or 3 (refused) within 10 seconds.
Failing inputs are kept in <corpus>/../fuzz-fail/.
"""
import os
import random
import shutil
import subprocess
import sys
import tempfile

MARKERS = [0xC0, 0xC2, 0xC4, 0xD0, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDD, 0xE0, 0xEE, 0xFE]


def damage(data, rng):
    b = bytearray(data)
    for _ in range(rng.choice((1, 1, 2, 3, 8))):
        if not b:
            break
        kind = rng.randrange(7)
        i = rng.randrange(len(b))
        if kind == 0:
            b[i] ^= 1 << rng.randrange(8)
        elif kind == 1:
            b[i] = rng.choice((0x00, 0xFF, rng.randrange(256)))
        elif kind == 2:
            del b[rng.randrange(max(1, len(b) // 2), len(b)):]
        elif kind == 3:
            b[i:i] = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 9)))
        elif kind == 4:
            del b[i:i + rng.randrange(1, 9)]
        elif kind == 5:
            b[i:i] = bytes((0xFF, rng.choice(MARKERS)))
        else:
            # a segment length or a size field near the start: big, small or zero
            j = rng.randrange(min(len(b), 600))
            b[j:j + 2] = rng.choice((b"\x00\x00", b"\x00\x02", b"\xff\xff", b"\x7f\xff", b"\x00\x01"))
    return bytes(b)


def main():
    corpus, jtest = sys.argv[1], sys.argv[2]
    runs = int(sys.argv[3]) if len(sys.argv) > 3 else 2000
    rng = random.Random(int(sys.argv[4]) if len(sys.argv) > 4 else 1)
    files = [os.path.join(corpus, f) for f in sorted(os.listdir(corpus))
             if f.endswith(".jpg") and os.path.getsize(os.path.join(corpus, f)) < 60000]
    faildir = os.path.join(os.path.dirname(os.path.abspath(corpus)), "fuzz-fail")
    env = dict(os.environ, ASAN_OPTIONS="exitcode=99:detect_leaks=1",
               UBSAN_OPTIONS="halt_on_error=1:exitcode=98:print_stacktrace=1")
    fails = 0
    counts = {}
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "f.jpg")
        for n in range(runs):
            src = rng.choice(files)
            with open(src, "rb") as f:
                data = damage(f.read(), rng)
            with open(path, "wb") as f:
                f.write(data)
            scale = str(rng.randrange(4))
            try:
                r = subprocess.run([jtest, path, "-", scale], capture_output=True, timeout=10, env=env)
                code = r.returncode
            except subprocess.TimeoutExpired:
                code = "hang"
            counts[code] = counts.get(code, 0) + 1
            if code not in (0, 2, 3):
                fails += 1
                os.makedirs(faildir, exist_ok=True)
                keep = os.path.join(faildir, "fail%04d_s%s.jpg" % (fails, scale))
                shutil.copy(path, keep)
                err = r.stderr.decode(errors="replace").strip().splitlines() if code != "hang" else ["hang"]
                print("FAIL %s (from %s, scale %s): %s" % (keep, os.path.basename(src), scale,
                                                         " | ".join(err[:3])))
    print("%d runs: %s; %d failures" % (runs, ", ".join("exit %s x%d" % kv for kv in sorted(counts.items(), key=str)), fails))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
