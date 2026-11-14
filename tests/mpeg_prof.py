#!/usr/bin/env python3
# AmigaMCE tests: where a 68k program's time goes, under vamos (68020 cycle
# counts, from Musashi): its PC sampled every SLICE cycles, each sample
# given to the function it falls in by the program's symbols (nm).
# Copyright (c) 2026 Brian King, MIT licence
#
#   m68k-amigaos-nm program > program.syms
#   mpeg_prof.py program.syms -- [vamos arguments] program [its arguments]
#
# The program writes "code at <address of main>" into mpeg_bench.base in
# its current directory (tests/mpeg_bench.c), which places the symbols.
# Run with amitools' Python.
import atexit
import bisect
import collections
import re
import sys

SLICE = 500

mapfile = sys.argv[1]
args = sys.argv[sys.argv.index("--") + 1:]

from amitools.vamos.machine import machine as mm
from amitools.vamos.machine import runtime as rt

samples = collections.Counter()
total = [0]
_execute = mm.Machine.execute


def execute(self, max_cycles=1000):
    er = _execute(self, max_cycles)
    samples[self.cpu.r_pc()] += er.cycles
    total[0] += er.cycles
    return er


mm.Machine.execute = execute
_init = rt.Runtime.__init__


def init(self, machine, run_cycles=1000, default_sp=None):
    _init(self, machine, SLICE, default_sp)


rt.Runtime.__init__ = init

# the program's functions (its code's symbols): (offset, name)
syms = []
for line in open(mapfile):
    m = re.match(r"([0-9a-f]+) [tT] _?([A-Za-z_][A-Za-z0-9_.]*)\s*$", line)
    if m:
        syms.append((int(m.group(1), 16), m.group(2)))
syms.sort()
offs = [s[0] for s in syms]
main_off = dict((n, o) for o, n in syms).get("main")

base = [None]


def report():
    try:
        m = re.search(r"code at ([0-9a-fA-F]+)", open("mpeg_bench.base").read())
        if m and main_off is not None:
            base[0] = int(m.group(1), 16) - main_off
    except OSError:
        pass
    if base[0] is None:
        print("no 'code at' line from the program", file=sys.__stdout__)
        return
    per = collections.Counter()
    for pc, c in samples.items():
        off = pc - base[0]
        i = bisect.bisect_right(offs, off) - 1
        name = syms[i][1] if 0 <= i < len(syms) and off >= 0 and off < offs[-1] + 65536 else "(outside: libraries, vamos)"
        per[name] += c
    t = total[0] or 1
    out = sys.__stdout__
    print("total %d cycles" % total[0], file=out)
    for name, c in per.most_common(30):
        print("%6.2f%% %12d  %s" % (100.0 * c / t, c, name), file=out)


atexit.register(report)


from amitools.tools.vamos import main

sys.argv = ["vamos"] + args
sys.exit(main())
