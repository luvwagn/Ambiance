; Ambiance - one output row of shrink.c's box averages, by hand for the
; 68060 (shrink.c's shrink_out calls it on the 68k).
; Copyright (c) 2026 Brian King, MIT
;
; Each output pixel the average of its box: its column span (xb) in each
; of its nr source rows. The same arithmetic as the C: the sums, then
; (sum + n/2) * (65536/n + 1) >> 16, at most 255, the reciprocals from
; shrink_new's table (rec[n]). m68kprof (tools/m68kprof) measured the C at about 10
; instructions a source pixel, the loads and adds through one register so
; that a 68060 could pair none of them; here the sums are in address
; registers (adda.l pairs with the next load) and the bytes go to three
; registers whose upper 24 bits stay clear.
;
;   a0 a row's walker        d0 the span's first byte (c0 * 3), then a sum
;   a1 the rows (pointers)   d1, d2, d3 a pixel's R, G, B (bits 8-31 clear)
;   a2 xb's walker           d4 the span's width (pixels)
;   a3 out                   d5 a row's pixels left (dbra)
;   a4, a5, a6 R, G, B sums  d6 rows left (dbra), then the reciprocal
;                            d7 output pixels left (dbra)
;
; Assembled with vasm (Motorola syntax), 68020 addressing. Checked byte
; for byte against the C by tests/shrink_host.c, run on a 68020 by vamos.

	machine	68020

	section	.text,code

	xdef	_shrink_out68k

; void shrink_out68k(UBYTE *d, const ULONG *xb, ULONG dw,
;     const UBYTE **row, ULONG nr, const ULONG *rec)
; rec[n]: 65536 / n + 1

ARGS	equ	4+11*4
A_D	equ	ARGS
A_XB	equ	ARGS+4
A_DW	equ	ARGS+8
A_ROW	equ	ARGS+12
A_NR	equ	ARGS+16
A_REC	equ	ARGS+20

_shrink_out68k:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	A_D(sp),a3
	move.l	A_XB(sp),a2
	move.l	A_DW(sp),d7
	beq	.done
	subq.l	#1,d7
	move.l	A_ROW(sp),a1
.px:
	move.l	(a2)+,d0		; c0
	move.l	(a2),d4			; c1
	cmp.l	d0,d4
	bhi.s	.wok
	move.l	d0,d4
	addq.l	#1,d4
.wok:	sub.l	d0,d4			; the span's width
	move.l	d0,d5
	add.l	d5,d5
	add.l	d5,d0			; c0 * 3: its first byte
	suba.l	a4,a4
	suba.l	a5,a5
	suba.l	a6,a6
	moveq	#0,d1
	moveq	#0,d2
	moveq	#0,d3
	move.l	A_NR(sp),d6
	subq.l	#1,d6
.row:
	move.l	(a1,d6.l*4),a0
	adda.l	d0,a0
	move.l	d4,d5
	subq.l	#1,d5
.sp:
	move.b	(a0)+,d1
	move.b	(a0)+,d2
	move.b	(a0)+,d3
	adda.l	d1,a4
	adda.l	d2,a5
	adda.l	d3,a6
	dbra	d5,.sp
	dbra	d6,.row
	; n = width * rows; its reciprocal from the table
	move.l	d4,d5
	mulu.l	A_NR(sp),d5
	move.l	A_REC(sp),a0
	move.l	(a0,d5.l*4),d6
	lsr.l	#1,d5			; n / 2
	move.l	a4,d0
	move.l	a5,d1
	move.l	a6,d2
	add.l	d5,d0
	add.l	d5,d1
	add.l	d5,d2
	mulu.l	d6,d0
	mulu.l	d6,d1
	mulu.l	d6,d2
	clr.w	d0
	clr.w	d1
	clr.w	d2
	swap	d0
	swap	d1
	swap	d2
	cmp.l	#255,d0
	bls.s	.r
	move.l	#255,d0
.r:	cmp.l	#255,d1
	bls.s	.g
	move.l	#255,d1
.g:	cmp.l	#255,d2
	bls.s	.b
	move.l	#255,d2
.b:	move.b	d0,(a3)+
	move.b	d1,(a3)+
	move.b	d2,(a3)+
	dbra	d7,.px
	; (dbra counts 65536 at most: dw is far below)
.done:
	movem.l	(sp)+,d2-d7/a2-a6
	rts
