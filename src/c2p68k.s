; Ambiance - 8-plane chunky to planar, 32 pixels a step, in registers
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
; 159 instructions a step.

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
	swap	d4
	eor.w	d4,d0
	eor.w	d0,d4
	eor.w	d4,d0
	swap	d4
	swap	d5
	eor.w	d5,d1
	eor.w	d1,d5
	eor.w	d5,d1
	swap	d5
	swap	d6
	eor.w	d6,d2
	eor.w	d2,d6
	eor.w	d6,d2
	swap	d6
	exg	d0,a2
	swap	d0
	eor.w	d0,d3
	eor.w	d3,d0
	eor.w	d0,d3
	swap	d0
	exg	d1,a2
	move.l	d2,d7
	lsr.l	#8,d7
	eor.l	d1,d7
	and.l	#$00FF00FF,d7
	eor.l	d7,d1
	lsl.l	#8,d7
	eor.l	d7,d2
	exg	d1,a2
	move.l	d3,d7
	lsr.l	#8,d7
	eor.l	d1,d7
	and.l	#$00FF00FF,d7
	eor.l	d7,d1
	lsl.l	#8,d7
	eor.l	d7,d3
	move.l	d6,d7
	lsr.l	#8,d7
	eor.l	d4,d7
	and.l	#$00FF00FF,d7
	eor.l	d7,d4
	lsl.l	#8,d7
	eor.l	d7,d6
	move.l	d0,d7
	lsr.l	#8,d7
	eor.l	d5,d7
	and.l	#$00FF00FF,d7
	eor.l	d7,d5
	lsl.l	#8,d7
	eor.l	d7,d0
	exg	d2,a2
	move.l	d1,d7
	lsr.l	#4,d7
	eor.l	d2,d7
	and.l	#$0F0F0F0F,d7
	eor.l	d7,d2
	lsl.l	#4,d7
	eor.l	d7,d1
	exg	d2,a2
	move.l	d3,d7
	lsr.l	#4,d7
	eor.l	d2,d7
	and.l	#$0F0F0F0F,d7
	eor.l	d7,d2
	lsl.l	#4,d7
	eor.l	d7,d3
	move.l	d5,d7
	lsr.l	#4,d7
	eor.l	d4,d7
	and.l	#$0F0F0F0F,d7
	eor.l	d7,d4
	lsl.l	#4,d7
	eor.l	d7,d5
	move.l	d0,d7
	lsr.l	#4,d7
	eor.l	d6,d7
	and.l	#$0F0F0F0F,d7
	eor.l	d7,d6
	lsl.l	#4,d7
	eor.l	d7,d0
	exg	d1,a2
	move.l	d4,d7
	lsr.l	#2,d7
	eor.l	d1,d7
	and.l	#$33333333,d7
	eor.l	d7,d1
	lsl.l	#2,d7
	eor.l	d7,d4
	exg	d1,a2
	move.l	d5,d7
	lsr.l	#2,d7
	eor.l	d1,d7
	and.l	#$33333333,d7
	eor.l	d7,d1
	lsl.l	#2,d7
	eor.l	d7,d5
	move.l	d6,d7
	lsr.l	#2,d7
	eor.l	d2,d7
	and.l	#$33333333,d7
	eor.l	d7,d2
	lsl.l	#2,d7
	eor.l	d7,d6
	move.l	d0,d7
	lsr.l	#2,d7
	eor.l	d3,d7
	and.l	#$33333333,d7
	eor.l	d7,d3
	lsl.l	#2,d7
	eor.l	d7,d0
	exg	d1,a2
	move.l	d2,d7
	lsr.l	#1,d7
	eor.l	d1,d7
	and.l	#$55555555,d7
	eor.l	d7,d1
	lsl.l	#1,d7
	eor.l	d7,d2
	exg	d1,a2
	move.l	d3,d7
	lsr.l	#1,d7
	eor.l	d1,d7
	and.l	#$55555555,d7
	eor.l	d7,d1
	lsl.l	#1,d7
	eor.l	d7,d3
	move.l	d6,d7
	lsr.l	#1,d7
	eor.l	d4,d7
	and.l	#$55555555,d7
	eor.l	d7,d4
	lsl.l	#1,d7
	eor.l	d7,d6
	move.l	d0,d7
	lsr.l	#1,d7
	eor.l	d5,d7
	and.l	#$55555555,d7
	eor.l	d7,d5
	lsl.l	#1,d7
	eor.l	d7,d0
	movem.l	0(a1),a3-a4
	move.l	d0,(0,a3,a6.l)
	move.l	d5,(0,a4,a6.l)
	movem.l	8(a1),a3-a4
	move.l	d3,(0,a3,a6.l)
	move.l	d1,(0,a4,a6.l)
	movem.l	16(a1),a3-a4
	move.l	d6,(0,a3,a6.l)
	move.l	d4,(0,a4,a6.l)
	movem.l	24(a1),a3-a4
	move.l	d2,(0,a3,a6.l)
	move.l	a2,(0,a4,a6.l)
	addq.l	#4,a6
	subq.l	#1,(sp)
	bne	.group
	addq.l	#4,sp
.done:
	movem.l	(sp)+,d2-d7/a2-a6
	rts
