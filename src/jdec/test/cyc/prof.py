#!/usr/bin/env python3
"""Where jdec's 68020 cycles go: the PC sampled every N cycles.
Ambiance, MIT.   python3 prof.py <cyc.elf> <cyc.bin> file.jpg scale"""
import bisect
import subprocess
import sys
from collections import Counter

import machine68k
import cyc

elf, binpath, path, scale = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
syms = []
for line in subprocess.run(["m68k-linux-gnu-nm", "-n", elf], capture_output=True, text=True).stdout.splitlines():
    a, t, n = line.split()
    if t in "tT":
        syms.append((int(a, 16), n))
addrs = [a for a, _ in syms]
entry = [a for a, n in syms if n == "start"][0]
binary = open(binpath, "rb").read()
data = open(path, "rb").read()

m = machine68k.Machine(machine68k.cpu_type_from_str("68020"), 16 * 1024)
mem, cpu = m.mem, m.cpu
mem.w32(0, cyc.STACK); mem.w32(4, entry); cpu.pulse_reset()
end = m.create_execute_end("done")
tid = m.traps.alloc(lambda op, pc: end)
mem.w16(cyc.RET, 0xA000 | tid)
mem.w_block(cyc.CODE, binary); mem.w_block(cyc.JPEG, data)
for i, v in enumerate((cyc.JPEG, len(data), scale, 0, cyc.HEAP)):
    mem.w32(cyc.PARAMS + 4 * i, v)
cpu.w_sp(cyc.STACK - 4); mem.w32(cyc.STACK - 4, cyc.RET); cpu.w_pc(entry)
hits = Counter()
total = 0
while True:
    r = m.execute(997)
    total += r.cycles
    if r.result is end:
        break
    pc = cpu.r_pc()
    i = bisect.bisect_right(addrs, pc) - 1
    hits[syms[i][1] if i >= 0 else "?"] += 1
n = sum(hits.values())
print("%s at 1/%d: %d cycles" % (path.split("/")[-1], 1 << scale, total))
for name, c in hits.most_common(12):
    print("  %5.1f%%  %s" % (100 * c / n, name))
