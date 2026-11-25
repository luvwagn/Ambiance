; Ambiance - a row of an MPEG picture made AGA HAM8 bytes (ham8.c's loop)
; Copyright (c) 2026 Brian King, MIT licence
;
; void ham8_row68k(const UBYTE *y, const UBYTE *cb, LONG cr_less_cb,
;                  UBYTE *out, ULONG pairs, const struct Ham8Step *s,
;                  ULONG twice, const UBYTE *nearest, const UBYTE *base)
;
;   y:       the row's Y, pairs * 2 of them
;   cb:      its Cb, pairs of them; its Cr cr_less_cb bytes on from each
;   out:     pairs * 2 bytes (twice: * 4)
;   s:       struct Ham8Step: q[3][128] (UWORDs: red's, green's, blue's
;            step for a difference d of 6 bits at [d + 64], already
;            shifted to its place); dec[4096] (0 green, 1 red, 2 blue, 3 a
;            base); rp[256] (by Cr), bp[256] (by Cb), gu[256] (by Cb):
;            where a channel is in lum (ptr[Y]: the channel's HAM8 byte;
;            green's gv[Cr] bytes on from gu's)
;   twice:   two bytes a pixel (a hires screen's), the second a step more
;   nearest: a 4:4:4 colour's nearest base register; base: 64 x 3 bytes
;            (each as its HAM8 bytes)
;
; As ham8.c's C, a pixel at a time. Registers:
;   a0 y   a1 out   a2 cb   a3 a4 a5 the pair's red, green, blue in lum
;   a6 s + 128 (q's red at 0, green 256, blue 512; dec at 640; rp at
;      4736, bp 5760, gu 6784, gv 7808)
;   d0 Y, then a difference, then dec's answer
;   d1 d2 d3 the pixel as HAM8 left it (r g b), each as its HAM8 byte
;   (its control bits, red 2, green 3, blue 1, << 6, and its 6 bits)
;   d4 d5 d6 the target, the same   d7 dec's index
; d1-d6 are only ever written a byte at a time: their upper 24 bits stay
; 0 (cleared at the start), so a long's subtraction is the channel's
; difference; and a channel set is its byte out.
;
; Checked against the C by tests/ham8_test.c (the same bytes).

	machine	68020

	section	code,code

	xdef	_ham8_row68k

NSAVE	equ	11*4
ARGS	equ	4+NSAVE
A_Y	equ	ARGS+0
A_CB	equ	ARGS+4
A_DIST	equ	ARGS+8
A_OUT	equ	ARGS+12
A_PAIRS	equ	ARGS+16
A_STEP	equ	ARGS+20
A_TWICE	equ	ARGS+24
A_NEAR	equ	ARGS+28
A_BASE	equ	ARGS+32
O_DEC	equ	640
O_RP	equ	4736
O_BP	equ	5760
O_GU	equ	6784
O_GV	equ	7808

; one HAM8 byte towards d4-d6 from d1-d3 (\1: the stack's extra depth
; here, for the arguments)
STEP	macro
	move.l	d4,d0
	sub.l	d1,d0			; red's difference
	move.w	(0,a6,d0.l*2),d7	; its step << 8
	move.l	d5,d0
	sub.l	d2,d0
	or.w	(256,a6,d0.l*2),d7	; green's << 4
	move.l	d6,d0
	sub.l	d3,d0
	or.w	(512,a6,d0.l*2),d7	; blue's
	move.b	(O_DEC,a6,d7.w),d0	; the step: 0 green, 1 red, 2 blue, 3 a base
	beq.s	.g\@
	subq.b	#1,d0
	beq.s	.r\@
	subq.b	#1,d0
	beq.s	.b\@
	; a base colour: the nearest to the whole target (4 bits a channel)
	move.l	d4,d7
	and.l	#$3C,d7
	lsl.l	#6,d7			; red's top 4 << 8
	move.l	d5,d0
	and.l	#$3C,d0
	lsl.l	#2,d0			; green's << 4
	or.l	d0,d7
	move.l	d6,d0
	and.l	#$3C,d0
	lsr.l	#2,d0			; blue's
	or.l	d0,d7
	move.l	a6,-(sp)
	move.l	A_NEAR+4+\1(sp),a6
	moveq	#0,d0
	move.b	(a6,d7.l),d0		; its register
	move.l	A_BASE+4+\1(sp),a6
	move.l	d0,d7
	add.l	d7,d7
	add.l	d0,d7			; * 3
	move.b	(a6,d7.l),d1		; (as HAM8 bytes, the base's channels)
	move.b	1(a6,d7.l),d2
	move.b	2(a6,d7.l),d3
	move.l	(sp)+,a6
	move.b	d0,(a1)+		; the register, control 0
	bra.s	.x\@
.g\@:
	move.b	d5,d2			; green set: its byte is the pixel's
	move.b	d5,(a1)+
	bra.s	.x\@
.r\@:
	move.b	d4,d1
	move.b	d4,(a1)+
	bra.s	.x\@
.b\@:
	move.b	d6,d3
	move.b	d6,(a1)+
.x\@:
	endm

; a pair's three places in lum, from its Cb and Cr
CHROMA	macro
	move.l	A_DIST(sp),d0
	moveq	#0,d7
	move.b	(a2,d0.l),d7		; Cr
	moveq	#0,d0
	move.b	(a2)+,d0		; Cb
	move.l	(O_RP,a6,d7.w*4),a3
	move.l	(O_BP,a6,d0.w*4),a5
	move.l	(O_GU,a6,d0.w*4),a4
	adda.w	(O_GV,a6,d7.w*2),a4
	endm

; a pixel's target from its Y and the pair's three places
TARGET	macro
	moveq	#0,d0
	move.b	(a0)+,d0
	move.b	(a3,d0.l),d4
	move.b	(a4,d0.l),d5
	move.b	(a5,d0.l),d6
	endm

_ham8_row68k:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	A_PAIRS(sp),d0
	beq	.done
	move.l	A_Y(sp),a0
	move.l	A_CB(sp),a2
	move.l	A_OUT(sp),a1
	move.l	A_STEP(sp),a6
	lea	128(a6),a6
	moveq	#0,d1			; the pixel before: register 0 (black)
	moveq	#0,d2
	moveq	#0,d3
	move.l	A_BASE(sp),a3
	move.b	(a3),d1
	move.b	1(a3),d2
	move.b	2(a3),d3
	moveq	#0,d4
	moveq	#0,d5
	moveq	#0,d6
	moveq	#0,d7
	tst.l	A_TWICE(sp)
	bne	.twice
.pair:
	CHROMA
	TARGET
	STEP	0
	TARGET
	STEP	0
	subq.l	#1,A_PAIRS(sp)
	bne	.pair
	bra	.done
.twice:
	CHROMA
	TARGET
	STEP	0
	STEP	0
	TARGET
	STEP	0
	STEP	0
	subq.l	#1,A_PAIRS(sp)
	bne	.twice
.done:
	movem.l	(sp)+,d2-d7/a2-a6
	rts
