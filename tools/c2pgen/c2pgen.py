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

def asm_sb(ops, loc, spread=8):
    """va_c2p68k_sb's step, for a CPU with a store buffer (68040, 68060):
    the previous step's 8 longs (left in 32 bytes at (sp)) written to chip
    RAM one at a time between this step's merges, so the chip bus is kept
    busy while the step computes, not given 8 writes at its end. Returns
    (the merges alone, the merges with the writes, the step's end: its
    longs kept at (sp), the writes alone)."""
    plain = asm(ops, loc)[:-12]                 # (no stores)
    regs = sorted(set(loc.values()), key=lambda r: (r[0] == 'a', int(r[1])))
    off = {r: 4 * i for i, r in enumerate(regs)}
    def write(k, ar):
        return ['\tmove.l\t%d(a1),%s' % (k * 4, ar),
                '\tmove.l\t%d(sp),(-4,%s,a6.l)' % (off[loc[OUT[k]]], ar)]
    # each write's two halves a few merges apart (the pointer loaded before
    # it is used), a3/a4/a5 in turn
    n = len(plain)
    at = {}
    for k in range(8):
        i = 2 + (k * (n - 8)) // 8
        ar = ('a3', 'a4', 'a5')[k % 3]
        w = write(k, ar)
        at.setdefault(i, []).append(w[0])
        at.setdefault(i + 4, []).append(w[1])
    mixed = []
    for i, l in enumerate(plain):
        mixed += at.get(i, [])
        mixed.append(l)
    keep = ['\tmovem.l\t%s,(sp)' % '/'.join(regs)]
    flush = []
    for k in range(8):
        flush += write(k, ('a3', 'a4', 'a5')[k % 3])
    return plain, mixed, keep, flush

ops, loc = gen()
random.seed(1)
for _ in range(20000):
    c = [random.getrandbits(32) for _ in range(8)]
    r = sim(ops, c)
    got = [r[loc[OUT[k]]] for k in range(8)]
    assert got == ref(c), (c, got, ref(c))
body = asm(ops, loc)
n = sum(1 for l in body if l.startswith('\t'))
sb_plain, sb_mixed, sb_keep, sb_flush = asm_sb(ops, loc)
nsb = sum(1 for l in sb_mixed + sb_keep if l.startswith('\t'))

# ---- each pen twice across (half size video, va_c2p2x68k): 16 pens in,
# 32 pixels out. As addresses: a pen's bit is (long n: j3 j2 | byte j1 j0 |
# bit c), and the 32 doubled pixels' (j3 j2 j1 j0 e), e the copy. Copies
# of the 4 longs would be a long index bit e, alike in every pair it
# divides, so the rounds of the 32-pixel network that do not move it are
# made on the 4 longs alone (two merges a round, not four): n's bits
# swapped with the byte's and the bits' (16, 8, 4, 2). The last round
# (e with the lowest bit, 1) on a long and its copy is a split: its odd
# bits doubled (e = 0), its even bits doubled (e = 1). Planes: long n's
# two halves are planes 7 - 2n (odd bits) and 6 - 2n (even).
MERGES2 = [(0, 2, 16), (1, 3, 16), (0, 1, 8), (2, 3, 8), (0, 2, 4), (1, 3, 4), (0, 1, 2), (2, 3, 2)]


def ref2(pens):
    """the plain c2p of the 16 pens, each twice: 8 plane longs"""
    dbl = []
    for p in pens:
        dbl += [p, p]
    c = [int.from_bytes(bytes(dbl[4 * i:4 * i + 4]), 'big') for i in range(8)]
    return ref(c)


def gen2():
    ops = []
    for lo, hi, s in MERGES2:
        A, B = 'd%d' % lo, 'd%d' % hi
        if s == 16:
            ops += [('swap', B), ('eorw', B, A), ('eorw', A, B), ('eorw', B, A), ('swap', B)]
        else:
            ops += [('movel', B, 'd4'), ('lsr', s, 'd4'), ('eorl', A, 'd4'), ('andl', MASK[s], 'd4'),
                    ('eorl', 'd4', A), ('lsl', s, 'd4'), ('eorl', 'd4', B)]
    for n in range(4):
        y = 'd%d' % n
        ops += [('movel', y, 'd4'), ('andl', 0xAAAAAAAA, 'd4'), ('movel', 'd4', 'd5'), ('lsr', 1, 'd5'),
                ('orl', 'd5', 'd4'),                                        # odd bits doubled
                ('andl', 0x55555555, y), ('movel', y, 'd5'), ('addl', 'd5', 'd5'), ('orl', 'd5', y),
                ('store', n)]                                               # (d4: 7 - 2n, y: 6 - 2n)
    return ops


def sim2(ops, x):
    r = {('d%d' % i): 0 for i in range(8)}
    for i in range(4):
        r['d%d' % i] = x[i]
    M = 0xFFFFFFFF
    out = {}
    for op in ops:
        o = op[0]
        if o == 'swap':
            v = r[op[1]]; r[op[1]] = ((v << 16) | (v >> 16)) & M
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
        elif o == 'orl':
            r[op[2]] |= r[op[1]]
        elif o == 'addl':
            r[op[2]] = (r[op[2]] + r[op[1]]) & M
        elif o == 'store':
            n = op[1]
            out[7 - 2 * n] = r['d4']
            out[6 - 2 * n] = r['d%d' % n]
    return [out[k] for k in range(8)]


def asm2(ops, twice=False):
    L = []
    for op in ops:
        o = op[0]
        if o == 'swap':   L.append('\tswap\t%s' % op[1])
        elif o == 'eorw': L.append('\teor.w\t%s,%s' % (op[1], op[2]))
        elif o == 'movel': L.append('\tmove.l\t%s,%s' % (op[1], op[2]))
        elif o == 'lsr': L.append('\tlsr.l\t#%d,%s' % (op[1], op[2]))
        elif o == 'lsl': L.append('\tlsl.l\t#%d,%s' % (op[1], op[2]))
        elif o == 'eorl': L.append('\teor.l\t%s,%s' % (op[1], op[2]))
        elif o == 'andl': L.append('\tand.l\t#$%08X,%s' % (op[1], op[2]))
        elif o == 'orl': L.append('\tor.l\t%s,%s' % (op[1], op[2]))
        elif o == 'addl': L.append('\tadd.l\t%s,%s' % (op[1], op[2]))
        elif o == 'store':
            n = op[1]
            # planes 6 - 2n and 7 - 2n: consecutive pointers
            L.append('\tmovem.l\t%d(a1),a3-a4' % ((6 - 2 * n) * 4))
            L.append('\tmove.l\td%d,(0,a3,a6.l)' % n)
            L.append('\tmove.l\td4,(0,a4,a6.l)')
            if twice:                   # (and the row below: d7 the offset there)
                L.append('\tmove.l\td%d,(0,a3,d7.l)' % n)
                L.append('\tmove.l\td4,(0,a4,d7.l)')
    return L


ops2 = gen2()
for _ in range(20000):
    pens = [random.getrandbits(8) for _ in range(16)]
    x = [int.from_bytes(bytes(pens[4 * i:4 * i + 4]), 'big') for i in range(4)]
    assert sim2(ops2, x) == ref2(pens), (pens, sim2(ops2, x), ref2(pens))
body2 = asm2(ops2)
n2 = sum(1 for l in body2 if l.startswith('\t'))
body2t = asm2(ops2, twice=True)
n2t = sum(1 for l in body2t if l.startswith('\t'))
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

; void va_c2p2x68k(const UBYTE *src, ULONG groups, UBYTE **planes)
;   src:    the row's pens, groups * 16 bytes (long-aligned)
;   planes: as va_c2p68k's: groups longs each written
;
; Each pen twice across (a half size video shown at its whole size): 16
; pens a step, 32 pixels. The 4 longs' merges as the 32-pixel network's
; that do not involve the copies (two a round, not four: 16, 8, 4, 2),
; then each long split in two, its odd bits doubled and its even bits
; doubled (the last round, on a long and its copy). Checked as above,
; against va_c2p68k's network on the pens doubled.
; %d instructions a step (32 pixels).

	xdef	_va_c2p2x68k

_va_c2p2x68k:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	ARGS+0(sp),a0
	move.l	ARGS+4(sp),d0
	move.l	ARGS+8(sp),a1
	tst.l	d0
	beq	.done2
	move.l	d0,-(sp)			; groups left
	sub.l	a6,a6				; the row's offset
.group2:
	movem.l	(a0)+,d0-d3
""" % (n2 + 5))
    f.write("\n".join(body2) + "\n")
    f.write("""	addq.l	#4,a6
	subq.l	#1,(sp)
	bne	.group2
	addq.l	#4,sp
.done2:
	movem.l	(sp)+,d2-d7/a2-a6
	rts

; void va_c2p2x68k_twice(const UBYTE *src, ULONG groups, UBYTE **planes,
;                        ULONG bpr)
;   as va_c2p2x68k, each long written to the row below too (bpr bytes on):
;   a half size picture's rows twice without the copper, its merges made
;   once for both (not va_c2p2x68k twice over).
; %d instructions a step (32 pixels, two rows).

	xdef	_va_c2p2x68k_twice

_va_c2p2x68k_twice:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	ARGS+0(sp),a0
	move.l	ARGS+4(sp),d0
	move.l	ARGS+8(sp),a1
	move.l	ARGS+12(sp),d7			; the row below's offset
	tst.l	d0
	beq	.done4
	move.l	d0,-(sp)			; groups left
	sub.l	a6,a6				; the row's offset
.group4:
	movem.l	(a0)+,d0-d3
""" % (n2t + 6))
    f.write("\n".join(body2t) + "\n")
    f.write("""	addq.l	#4,a6
	addq.l	#4,d7
	subq.l	#1,(sp)
	bne	.group4
	addq.l	#4,sp
.done4:
	movem.l	(sp)+,d2-d7/a2-a6
	rts

; void va_c2p68k_sb(const UBYTE *src, ULONG groups, UBYTE **planes)
;   as va_c2p68k, for a 68040 or 68060 (it runs on a 68020 too, a little
;   slower than va_c2p68k): their store buffer takes a few writes and lets
;   the CPU go on, so a step's 8 longs written together at its end (as
;   va_c2p68k does) keep it waiting for chip RAM while the next step could
;   compute. Here each step's longs are kept in 32 bytes on the stack
;   (one movem, in the cache) and written during the next step, one
;   between each few merges; the last step's after the loop. The same
;   merges, the same planes.
; %d instructions a step.

	xdef	_va_c2p68k_sb

_va_c2p68k_sb:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	ARGS+0(sp),a0
	move.l	ARGS+4(sp),d0
	move.l	ARGS+8(sp),a1
	tst.l	d0
	beq	.done3
	move.l	d0,-(sp)			; groups left (32(sp))
	lea	-32(sp),sp			; the last step's longs
	sub.l	a6,a6				; the row's offset
	movem.l	(a0)+,d0-d6/a2
""" % nsb)
    f.write("\n".join(sb_plain + sb_keep) + "\n")
    f.write("""	addq.l	#4,a6
	subq.l	#1,32(sp)
	beq	.flush3
.group3:
	movem.l	(a0)+,d0-d6/a2
""")
    f.write("\n".join(sb_mixed + sb_keep) + "\n")
    f.write("""	addq.l	#4,a6
	subq.l	#1,32(sp)
	bne	.group3
.flush3:
""")
    f.write("\n".join(sb_flush) + "\n")
    f.write("""	lea	36(sp),sp
.done3:
	movem.l	(sp)+,d2-d7/a2-a6
	rts
""")
print("c2pgen: %d instructions a step, checked on 20000 random steps; each pen twice: %d (32 pixels);"
      " store buffer's: %d; two rows: %d" % (n, n2, nsb, n2t))
