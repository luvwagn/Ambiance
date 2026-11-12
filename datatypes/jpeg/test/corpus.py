#!/usr/bin/env python3
"""Make jdec's test corpus: JPEGs of every kind jdec decodes, and some it
should refuse cleanly. AmigaMCE, MIT. Needs Pillow, numpy, ffmpeg.

    python3 corpus.py <dir>

Writes <dir>/*.jpg and <dir>/expect.txt: one line a file, "name ok" or
"name <error code>" (jdec.h's JD_E... values).
"""
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageFilter


def scene(w, h, seed):
    """A photo-like picture: gradients, soft shapes, edges, grain."""
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    y /= max(h, 1)
    x /= max(w, 1)
    img = np.stack([0.8 - 0.4 * y + 0.1 * np.sin(7 * x + seed),
                    0.5 - 0.2 * y + 0.1 * np.cos(5 * x),
                    0.3 + 0.5 * y], axis=-1)
    for _ in range(10):
        cx, cy = rng.uniform(0, 1, 2)
        rad = rng.uniform(0.05, 0.3)
        col = rng.uniform(0, 1, 3)
        d = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
        a = np.clip(1 - d / rad, 0, 1) ** 1.5
        img = img * (1 - a[..., None]) + col * a[..., None]
    for _ in range(6):                  # hard edges: rectangles
        x0, y0 = int(rng.uniform(0, w)), int(rng.uniform(0, h))
        img[y0:y0 + max(1, h // 6), x0:x0 + max(1, w // 8)] = rng.uniform(0, 1, 3)
    img += rng.normal(0, 0.03, img.shape)
    img = (np.clip(img, 0, 1) * 255).astype(np.uint8)
    return Image.fromarray(img, "RGB").filter(ImageFilter.GaussianBlur(0.4))


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    expect = []

    def save(name, img, result="ok", **kw):
        img.save(os.path.join(out, name), "JPEG", **kw)
        expect.append((name, result))

    sizes = [(1, 1), (7, 5), (8, 8), (16, 16), (17, 13), (33, 47), (100, 75), (160, 160),
             (300, 300), (257, 129), (640, 480), (1023, 767)]
    seed = 1
    for w, h in sizes:
        img = scene(w, h, seed)
        seed += 1
        for sub, sname in ((0, "444"), (1, "422"), (2, "420")):
            for q in (5, 50, 90, 100):
                if q in (5, 100) and w * h > 300 * 300:
                    continue
                save("c%dx%d_%s_q%d.jpg" % (w, h, sname, q), img, quality=q, subsampling=sub)
        save("g%dx%d_q85.jpg" % (w, h), img.convert("L"), quality=85)
    big = scene(640, 480, 99)
    # restart markers
    save("rst_blocks1.jpg", big, quality=80, subsampling=2, restart_marker_blocks=1)
    save("rst_blocks7.jpg", big, quality=80, subsampling=2, restart_marker_blocks=7)
    save("rst_rows1.jpg", big, quality=80, subsampling=0, restart_marker_rows=1)
    save("rst_grey3.jpg", big.convert("L"), quality=80, restart_marker_blocks=3)
    # optimised Huffman tables (long codes), and 16-bit quant tables
    save("opt_420.jpg", big, quality=92, subsampling=2, optimize=True)
    # RGB stored as RGB (Adobe transform 0)
    save("rgb_adobe.jpg", scene(200, 150, 7), quality=90, subsampling=0, keep_rgb=True)
    # refused: progressive, CMYK
    save("prog_420.jpg", big, "-5", quality=85, progressive=True)
    save("cmyk.jpg", scene(64, 64, 3).convert("CMYK"), "-6", quality=85)
    # 4:4:0 and 4:1:1 by ffmpeg's encoder
    src = os.path.join(out, "_src.png")
    big.save(src)
    for pix, name in (("yuvj440p", "c640x480_440.jpg"), ("yuvj411p", "c640x480_411.jpg")):
        r = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", src,
                            "-pix_fmt", pix, "-q:v", "3", "-frames:v", "1",
                            os.path.join(out, name)])
        if r.returncode == 0:
            expect.append((name, "ok"))
    os.remove(src)
    # not JPEGs
    with open(os.path.join(out, "notjpeg.jpg"), "wb") as f:
        f.write(b"GIF89a" + bytes(40))
    expect.append(("notjpeg.jpg", "-3"))
    with open(os.path.join(out, "empty.jpg"), "wb") as f:
        pass
    expect.append(("empty.jpg", "-3"))
    with open(os.path.join(out, "expect.txt"), "w") as f:
        for n, r in expect:
            f.write("%s %s\n" % (n, r))
    print(len(expect), "files")


if __name__ == "__main__":
    main()
