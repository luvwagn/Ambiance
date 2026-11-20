; Ambiance - a row of an MPEG picture made pens (AGA video), 4 at a time
; Copyright (c) 2026 Brian King
;
; void va_pens68k(const UBYTE *y, const UWORD *cl, UBYTE *dst, ULONG quads,
;                 const UWORD *yt4, const UBYTE *lut)
;
;   y:    the row's Y, quads * 4 of them
;   cl:   its blocks' Cb and Cr part (a UWORD a block of 2 pixels: two a
;         quad), as anim.c's va_pens_row makes it
;   dst:  quads * 4 pens
;   yt4:  the row's Y part, its 4 dither phases side by side: yt4[Y * 4 +
;         x & 3] (anim.c's va_yt4[y & 3])
;   lut:  the colour table: lut[Y part | Cb and Cr part] is a pen
;
; As the C (anim.c's va_pens_row) a pixel: lut[yt[phase][Y] | c]. Here
; the 4 phases of a row are one table (an index's scale of 8 and a
; displacement of 0, 2, 4, 6 pick a pixel's), and every pointer stays in
; a register:
;
;   a0 y   a1 cl   a2 dst   a3 lut   a4 yt4
;   d0 a pixel's Y (its upper 24 bits kept 0: written as bytes only)
;   d1 its Y part, then the pen's index (upper 16 bits kept 0)
;   d4 4 Y's, turned a byte at a time
;   d6, d7 the quad's two Cb and Cr parts   d5 quads left
;
; Checked against the C by tests/pens_test.c on a 68020 (vamos).

	machine	68020

	section	code,code

	xdef	_va_pens68k

ARGS	equ	4+6*4

_va_pens68k:
	movem.l	d2-d5/a2-a3,-(sp)
	move.l	a4,-(sp)
	move.l	d6,-(sp)
	move.l	d7,-(sp)
	move.l	ARGS+12+0(sp),a0
	move.l	ARGS+12+4(sp),a1
	move.l	ARGS+12+8(sp),a2
	move.l	ARGS+12+12(sp),d5
	move.l	ARGS+12+16(sp),a4
	move.l	ARGS+12+20(sp),a3
	tst.l	d5
	beq	.done
	moveq	#0,d0
	moveq	#0,d1
.quad:
	move.l	(a0)+,d4		; Y0 Y1 Y2 Y3
	move.w	(a1)+,d6		; pixels 0 and 1's Cb and Cr part
	move.w	(a1)+,d7		; pixels 2 and 3's
	rol.l	#8,d4
	move.b	d4,d0
	move.w	(0,a4,d0.l*8),d1
	or.w	d6,d1
	move.b	(a3,d1.l),(a2)+
	rol.l	#8,d4
	move.b	d4,d0
	move.w	(2,a4,d0.l*8),d1
	or.w	d6,d1
	move.b	(a3,d1.l),(a2)+
	rol.l	#8,d4
	move.b	d4,d0
	move.w	(4,a4,d0.l*8),d1
	or.w	d7,d1
	move.b	(a3,d1.l),(a2)+
	rol.l	#8,d4
	move.b	d4,d0
	move.w	(6,a4,d0.l*8),d1
	or.w	d7,d1
	move.b	(a3,d1.l),(a2)+
	subq.l	#1,d5
	bne	.quad
.done:
	move.l	(sp)+,d7
	move.l	(sp)+,d6
	move.l	(sp)+,a4
	movem.l	(sp)+,d2-d5/a2-a3
	rts
