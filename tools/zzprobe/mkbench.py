#!/usr/bin/env python3
"""Make zzprobe's test files: Bench/test.jpg, test.png, test.webp, test.mp3.

Ambiance, MIT. Needs Pillow (with WebP), numpy and ffmpeg with libmp3lame.

    python3 tools/zzprobe/mkbench.py tools/zzprobe/Bench

- The pictures: one 300 x 300 picture (a folder cover's size) with
  photo-like detail (gradients, soft shapes, fine grain), since how long a
  JPEG takes to decode depends on its detail. The same picture in each
  format, so their times compare: JPEG quality 90 (baseline), PNG
  24-bit, WebP lossy quality 85.
- The MP3: 30 seconds of music-like stereo (chords with harmonics, a bass
  line, hi-hat and kick), 128 kbit/s CBR, 44.1 kHz joint stereo, the most
  common kind. No ID3 tag and no Xing frame, so every mpega.library opens
  it the same way.

Everything comes from a fixed seed: the files are the same on every run.
"""
import os
import subprocess
import sys
import tempfile
import wave

import numpy as np
from PIL import Image, ImageFilter

SIZE = 300
RATE = 44100
SECONDS = 30


def picture():
    rng = np.random.default_rng(1985)
    y, x = np.mgrid[0:SIZE, 0:SIZE].astype(np.float32) / SIZE
    # a sky-to-ground gradient, warm to cool
    r = 0.85 - 0.45 * y + 0.10 * np.sin(6.0 * x)
    g = 0.55 - 0.20 * y + 0.10 * np.cos(5.0 * x + 1.0)
    b = 0.35 + 0.45 * y
    img = np.stack([r, g, b], axis=-1)
    # soft circles: a sun and some blurred shapes
    for _ in range(14):
        cx, cy = rng.uniform(0, 1, 2)
        rad = rng.uniform(0.04, 0.22)
        col = rng.uniform(0.0, 1.0, 3)
        d = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
        a = np.clip(1.0 - d / rad, 0.0, 1.0) ** 1.5 * rng.uniform(0.4, 0.9)
        img = img * (1 - a[..., None]) + col * a[..., None]
    # a horizon line with structure (a city or hills)
    for i in range(40):
        x0 = int(rng.uniform(0, SIZE))
        w = int(rng.uniform(6, 30))
        h = int(rng.uniform(10, 90))
        shade = rng.uniform(0.05, 0.35)
        img[SIZE - 60 - h:SIZE - 60, x0:x0 + w] = shade
        # lit windows
        for _ in range(int(h * w / 60)):
            wy = int(rng.uniform(SIZE - 60 - h, SIZE - 62))
            wx = int(rng.uniform(x0, min(SIZE - 1, x0 + w)))
            img[wy:wy + 2, wx:wx + 2] = (0.95, 0.85, 0.45)
    img[SIZE - 60:] *= 0.5
    # fine grain, as a photo has
    img += rng.normal(0, 0.035, img.shape)
    img = (np.clip(img, 0, 1) * 255).astype(np.uint8)
    pic = Image.fromarray(img, "RGB").filter(ImageFilter.GaussianBlur(0.6))
    # text-like strokes, as covers have
    arr = np.array(pic)
    for row in range(3):
        y0 = 24 + row * 16
        for k in range(28):
            x0 = 20 + k * 9
            if rng.uniform() < 0.8:
                arr[y0:y0 + 10, x0:x0 + 2] = 250
                arr[y0 + rng.integers(0, 8):y0 + rng.integers(2, 10), x0:x0 + 6] = 250
    return Image.fromarray(arr, "RGB")


def sound(path_wav):
    rng = np.random.default_rng(2026)
    n = RATE * SECONDS
    t = np.arange(n) / RATE
    left = np.zeros(n)
    right = np.zeros(n)
    beat = 0.5                                  # 120 bpm
    chords = [[220.0, 277.18, 329.63], [196.0, 246.94, 293.66],
              [174.61, 220.0, 261.63], [196.0, 246.94, 329.63]]
    for bar in range(int(SECONDS / (4 * beat))):
        start = int(bar * 4 * beat * RATE)
        end = min(n, int((bar + 1) * 4 * beat * RATE))
        tt = t[start:end] - t[start]
        env = np.exp(-tt * 0.8)
        for i, f in enumerate(chords[bar % 4]):
            tone = sum(np.sin(2 * np.pi * f * h * tt) / h ** 1.3 for h in range(1, 7))
            pan = 0.3 + 0.2 * i
            left[start:end] += 0.12 * env * tone * (1 - pan)
            right[start:end] += 0.12 * env * tone * pan
        # bass: root an octave down, on each beat
        for q in range(4):
            s = start + int(q * beat * RATE)
            e = min(n, s + int(beat * RATE))
            tb = t[s:e] - t[s]
            f = chords[bar % 4][0] / 2
            bass = np.sin(2 * np.pi * f * tb) * np.exp(-tb * 3)
            left[s:e] += 0.25 * bass
            right[s:e] += 0.25 * bass
    # kick on beats, hi-hat on eighths
    for k in range(int(SECONDS / (beat / 2))):
        s = int(k * beat / 2 * RATE)
        e = min(n, s + int(0.12 * RATE))
        tk = t[s:e] - t[s]
        hat = rng.normal(0, 1, e - s) * np.exp(-tk * 60) * 0.08
        left[s:e] += hat
        right[s:e] += hat * 0.8
        if k % 2 == 0:
            kick = np.sin(2 * np.pi * (60 + 90 * np.exp(-tk * 30)) * tk) * np.exp(-tk * 18) * 0.5
            left[s:e] += kick
            right[s:e] += kick
    peak = max(np.abs(left).max(), np.abs(right).max())
    pcm = np.stack([left, right], axis=-1) / peak * 0.8
    pcm = (pcm * 32767).astype("<i2")
    with wave.open(path_wav, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.tobytes())


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "Bench"
    os.makedirs(out, exist_ok=True)
    pic = picture()
    pic.save(os.path.join(out, "test.jpg"), "JPEG", quality=90, optimize=True, progressive=False)
    pic.save(os.path.join(out, "test.png"), "PNG", optimize=True)
    pic.save(os.path.join(out, "test.webp"), "WEBP", quality=85, method=6)
    with tempfile.TemporaryDirectory() as tmp:
        wav = os.path.join(tmp, "test.wav")
        sound(wav)
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", wav,
                        "-c:a", "libmp3lame", "-b:a", "128k", "-ar", str(RATE), "-ac", "2",
                        "-joint_stereo", "1", "-write_xing", "0", "-id3v2_version", "0",
                        "-map_metadata", "-1", "-bitexact",
                        os.path.join(out, "test.mp3")], check=True)
    for f in ("test.jpg", "test.png", "test.webp", "test.mp3"):
        print("%-10s %7d bytes" % (f, os.path.getsize(os.path.join(out, f))))


if __name__ == "__main__":
    main()
