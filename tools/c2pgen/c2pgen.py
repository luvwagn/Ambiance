#!/usr/bin/env python3
"""Writes src/c2p68k.s: 8-plane chunky-to-planar, 32 pixels a step, the
merge method (as gfx.c's c2p32), every value kept in registers.

Values v0..v7 (the 8 chunky longs) live in d0-d7 and a2: one of them is
always "stashed" in a2, leaving one data register free as the merges'
temporary; a merge that needs the stashed value swaps it (exg) with one
it does not use. The 16-pixel merges need no temporary (swap, and the
low words exchanged by three eor.w). The emitted code is checked here by
simulating it against the C algorithm on random input before it is
written; tests/c2p_test.c checks the assembled code on a 68020 (vamos)."""
import random, sys

MERGES = [(0,4,16),(1,5,16),(2,6,16),(3,7,16),
          (0,2,8),(1,3,8),(4,6,8),(5,7,8),
          (0,1,4),(2,3,4),(4,5,4),(6,7,4),
          (0,4,2),(1,5,2),(2,6,2),(3,7,2),
          (0,2,1),(1,3,1),(4,6,1),(5,7,1)]
MASK = {16:0x0000FFFF, 8:0x00FF00FF, 4:0x0F0F0F0F, 2:0x33333333, 1:0x55555555}
OUT = [7,5,3,1,6,4,2,0]          # plane k: value OUT[k]

def ref(c):
    a = list(c)
    for lo, hi, s in MERGES:
        t = ((a[hi] >> s) ^ a[lo]) & MASK[s]
        a[lo] ^= t
        a[hi] = (a[hi] ^ (t << s)) & 0xFFFFFFFF
    return [a[OUT[k]] for k in range(8)]

def gen():
    loc = {v: 'd%d' % v for v in range(7)}
    loc[7] = 'a2'
    ops = []                                  # (op, args)
    def data_regs_used():
        return {r for r in loc.values() if r.startswith('d')}
    def free_d():
        f = [r for r in ('d%d' % i for i in range(8)) if r not in data_regs_used()]
        assert len(f) == 1
        return f[0]
    def unstash(need):
        if any(loc[v] == 'a2' for v in need):
            # a value in a data register that this merge does not use
            w = next(v for v in range(8) if v not in need and loc[v].startswith('d'))
            s = next(v for v in need if loc[v] == 'a2')
            ops.append(('exg', loc[w], 'a2'))
            loc[s], loc[w] = loc[w], 'a2'
    for lo, hi, s in MERGES:
        unstash((lo, hi))
        A, B = loc[lo], loc[hi]
        if s == 16:
            ops += [('swap', B), ('eorw', B, A), ('eorw', A, B), ('eorw', B, A), ('swap', B)]
        else:
            T = free_d()
            ops += [('movel', B, T), ('lsr', s, T), ('eorl', A, T), ('andl', MASK[s], T),
                    ('eorl', T, A), ('lsl', s, T), ('eorl', T, B)]
    return ops, loc

def sim(ops, c):
    r = {('d%d' % i): c[i] for i in range(7)}
    r['a2'] = c[7]
    r['d7'] = 0
    M = 0xFFFFFFFF
    for op in ops:
        o = op[0]
        if o == 'exg':
            r[op[1]], r[op[2]] = r[op[2]], r[op[1]]
        elif o == 'swap':
            x = r[op[1]]; r[op[1]] = ((x << 16) | (x >> 16)) & M
        elif o == 'eorw':
            r[op[2]] = (r[op[2]] & 0xFFFF0000) | ((r[op[2]] ^ r[op[1]]) & 0xFFFF)
        elif o == 'movel':
            r[op[2]] = r[op[1]]
        elif o == 'lsr':
            r[op[2]] = r[op[2]] >> op[1]
        elif o == 'lsl':
            r[op[2]] = (r[op[2]] << op[1]) & M
        elif o == 'eorl':
            r[op[2]] ^= r[op[1]]
        elif o == 'andl':
            r[op[2]] &= op[1]
    return r

def asm(ops, loc):
    L = []
    for op in ops:
        o = op[0]
        if o == 'exg':   L.append('\texg\t%s,%s' % (op[1], op[2]))
        elif o == 'swap': L.append('\tswap\t%s' % op[1])
        elif o == 'eorw': L.append('\teor.w\t%s,%s' % (op[1], op[2]))
        elif o == 'movel': L.append('\tmove.l\t%s,%s' % (op[1], op[2]))
        elif o == 'lsr': L.append('\tlsr.l\t#%d,%s' % (op[1], op[2]))
        elif o == 'lsl': L.append('\tlsl.l\t#%d,%s' % (op[1], op[2]))
        elif o == 'eorl': L.append('\teor.l\t%s,%s' % (op[1], op[2]))
        elif o == 'andl': L.append('\tand.l\t#$%08X,%s' % (op[1], op[2]))
    # the stores: plane bases two at a time (a3, a4), the row's offset a6
    for k in range(0, 8, 2):
        L.append('\tmovem.l\t%d(a1),a3-a4' % (k * 4))
        L.append('\tmove.l\t%s,(0,a3,a6.l)' % loc[OUT[k]])
        L.append('\tmove.l\t%s,(0,a4,a6.l)' % loc[OUT[k + 1]])
    return L

ops, loc = gen()
random.seed(1)
for _ in range(20000):
    c = [random.getrandbits(32) for _ in range(8)]
    r = sim(ops, c)
    got = [r[loc[OUT[k]]] for k in range(8)]
    assert got == ref(c), (c, got, ref(c))
body = asm(ops, loc)
n = sum(1 for l in body if l.startswith('\t'))
out = sys.argv[1]
with open(out, 'w') as f:
    f.write("""; Ambiance - 8-plane chunky to planar, 32 pixels a step, in registers
; Copyright (c) 2026 Brian King
;
; WRITTEN BY tools/c2pgen/c2pgen.py: change that, not this.
;
; void va_c2p68k(const UBYTE *src, ULONG groups, UBYTE **planes)
;   src:    the row's pens, groups * 32 bytes (long-aligned)
;   planes: 8 pointers, each plane's first long of the row (written
;           groups longs each, from there)
;
; The merge method (as gfx.c's c2p32: five rounds of masked swaps between
; pairs of longs), every value in a register: the 8 longs in d0-d7 and
; a2, one of them always in a2 so one data register is the merges'
; temporary; the 16-pixel round needs none (swap, and the low words
; exchanged by three eor.w). Checked by c2pgen.py's simulation against
; the C algorithm, and assembled on a 68020 by tests/c2p_test.c (vamos).
; %d instructions a step.

	machine	68020

	section	code,code

	xdef	_va_c2p68k

ARGS	equ	4+11*4

_va_c2p68k:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	ARGS+0(sp),a0
	move.l	ARGS+4(sp),d0
	move.l	ARGS+8(sp),a1
	tst.l	d0
	beq	.done
	move.l	d0,-(sp)			; groups left
	sub.l	a6,a6				; the row's offset
.group:
	movem.l	(a0)+,d0-d6/a2
""" % (n + 6))
    f.write("\n".join(body) + "\n")
    f.write("""	addq.l	#4,a6
	subq.l	#1,(sp)
	bne	.group
	addq.l	#4,sp
.done:
	movem.l	(sp)+,d2-d7/a2-a6
	rts
""")
print("c2pgen: %d instructions a step, checked on 20000 random steps" % n)
