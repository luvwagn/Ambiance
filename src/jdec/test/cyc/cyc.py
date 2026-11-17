#!/usr/bin/env python3
"""Count jdec's 68020 cycles (Musashi, through machine68k).
Ambiance, MIT.

    python3 cyc.py <cyc.bin> <entry (hex)> <jtest (host)> file.jpg [file.jpg...]

(entry: start's address, from nm on the linked cyc.elf)

Each file is decoded at every scale on a bare 68020 (no caches, no wait
states: Musashi's own timings), its output checked byte for byte against
the host build's, and the cycles shown, with what they come to on a
50 MHz 68030 (a TwelveGauge, Ambiance's usual test machine: the same
timings, give or take the 030's data cache).
"""
import subprocess
import sys
import tempfile

import machine68k

PARAMS, CODE, JPEG, HEAP, OUT, STACK, RET = 0xF000, 0x10000, 0x400000, 0x800000, 0xC00000, 0xFF0000, 0x1000


def run(binary, entry, data, scale, keep):
    m = machine68k.Machine(machine68k.cpu_type_from_str("68020"), 16 * 1024)
    mem, cpu = m.mem, m.cpu
    mem.w32(0, STACK)
    mem.w32(4, entry)
    cpu.pulse_reset()
    end = m.create_execute_end("done")
    tid = m.traps.alloc(lambda op, pc: end)
    mem.w16(RET, 0xA000 | tid)
    mem.w_block(CODE, binary)
    mem.w_block(JPEG, data)
    for i, v in enumerate((JPEG, len(data), scale, OUT if keep else 0, HEAP)):
        mem.w32(PARAMS + 4 * i, v)
    cpu.w_sp(STACK - 4)
    mem.w32(STACK - 4, RET)
    cpu.w_pc(entry)
    total = 0
    while True:
        r = m.execute(2_000_000_000)
        total += r.cycles
        if r.result is end or r.cycles == 0:
            break
    res, w, h, used = (mem.r32(PARAMS + 4 * i) for i in (8, 9, 10, 11))
    out = mem.r_block(OUT, w * h * 3) if keep and res == 0 else None
    m.traps.free(tid)
    m.cleanup()
    return total, res, w, h, used, out


def main():
    binary = open(sys.argv[1], "rb").read()
    entry = int(sys.argv[2], 16)
    jtest = sys.argv[3]
    for path in sys.argv[4:]:
        data = open(path, "rb").read()
        for scale in range(4):
            cyc, res, w, h, used, out = run(binary, entry, data, scale, True)
            with tempfile.NamedTemporaryFile(suffix=".ppm") as t:
                subprocess.run([jtest, path, t.name, str(scale)], check=True)
                ppm = open(t.name, "rb").read()
            ref = ppm[len(ppm) - w * h * 3:]
            same = out == ref
            cyc, _, _, _, _, _ = run(binary, entry, data, scale, False)
            print("%-28s 1/%d %4dx%-4d %11d cycles %6.1f per pixel  %7.1f ms at 50 MHz  heap %6d  %s"
                  % (path.split("/")[-1], 1 << scale, w, h, cyc, cyc / (w * h), cyc / 50000, used,
                     "same as host" if same else "DIFFERENT"))


if __name__ == "__main__":
    main()
