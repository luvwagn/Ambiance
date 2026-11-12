#!/usr/bin/env python3
"""Write a small ProTracker module for testing the replayer (make test).

Usage: python3 -I mkmod.py <out.mod> [8CHN | FLT8]

With 8CHN or FLT8, the same song on 8 channels (each pattern's rows 8
cells wide, or for FLT8 as Startrekker kept them, two 4-channel patterns):
channels 5-8 play notes of their own, so each is heard.

Two positions of one pattern at speed 6 / 125 BPM (0.12 s a row). Channel 4
loops rows 0-15 once (E60 ... E61) and row 20 is held a row longer (EE1),
so each pass plays 64 + 16 + 1 rows: 162 rows, 19.44 s in all. Channels 1-3
play notes with arpeggio, portamento, vibrato, tone portamento, volume
slides, tremolo, sample offset, retrigger and note delay along the way.
"""
import math
import struct
import sys

PERIODS = [856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
           428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
           214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113]


def cell(period=0, sample=0, fx=0, param=0):
    return bytes([(sample & 0xF0) | (period >> 8), period & 0xFF,
                  ((sample & 0x0F) << 4) | fx, param])


def main():
    out = sys.argv[1]
    kind = sys.argv[2] if len(sys.argv) > 2 else "M.K."
    # one looping sample: a square-ish wave, 64 bytes; one one-shot: a decay
    loop = bytes((40 if i < 32 else -40) & 0xFF for i in range(64))
    shot = bytes(int(100 * math.exp(-i / 300.0) * math.sin(i / 3.0)) & 0xFF for i in range(2048))
    head = b"replayer test".ljust(20, b"\0")
    samples = [(b"loop", loop, 64, 0, 32), (b"shot", shot, 48, 0, 1)]
    for i in range(31):
        if i < len(samples):
            name, data, vol, rep, replen = samples[i]
            head += name.ljust(22, b"\0") + struct.pack(">HBBHH", len(data) // 2, 0, vol, rep, replen)
        else:
            head += b"\0" * 22 + struct.pack(">HBBHH", 0, 0, 0, 0, 1)
    head += bytes([2, 127]) + bytes([0, 0] + [0] * 126) + kind.encode()
    rows = []
    more = []
    fxs = [(0, 0x37), (1, 0x02), (2, 0x02), (4, 0x46), (3, 0x08), (0xA, 0x02), (7, 0x34),
           (9, 0x02), (0xE, 0x93), (0xE, 0xD2), (6, 0x01), (5, 0x01), (0xC, 0x20), (0, 0)]
    for r in range(64):
        c1 = cell(PERIODS[(r // 4) % 24], 1, *fxs[(r // 4) % len(fxs)]) if r % 4 == 0 else cell()
        c2 = cell(PERIODS[12 + (r // 8) % 12], 2) if r % 8 == 2 else cell(0, 0, 0xA, 0x01)
        c3 = cell(PERIODS[(r * 5) % 36], 1, 0, 0) if r % 6 == 1 else cell(0, 0, 4, 0x24)
        if r == 0:
            c4 = cell(PERIODS[0], 2, 0xE, 0x60)
        elif r == 15:
            c4 = cell(0, 0, 0xE, 0x61)
        elif r == 20:
            c4 = cell(PERIODS[5], 1, 0xE, 0xE1)
        else:
            c4 = cell()
        rows.append(c1 + c2 + c3 + c4)
        # channels 5-8 (8-channel kinds): notes of their own
        more.append((cell(PERIODS[(r * 3) % 36], 2) if r % 5 == 0 else cell()) +
                    (cell(PERIODS[(r * 7) % 36], 1, 0xC, 0x30) if r % 7 == 3 else cell()) +
                    (cell(PERIODS[10 + r % 20], 1) if r % 9 == 4 else cell(0, 0, 1, 0x01)) +
                    (cell(PERIODS[r % 36], 2, 0x9, 0x04) if r % 11 == 6 else cell()))
    if kind == "8CHN":
        pats = b"".join(rows[r] + more[r] for r in range(64))
    elif kind == "FLT8":
        pats = b"".join(rows) + b"".join(more)
    else:
        pats = b"".join(rows)
    with open(out, "wb") as fh:
        fh.write(head + pats)
        for _, data, _, _, _ in samples:
            fh.write(data)


if __name__ == "__main__":
    main()
