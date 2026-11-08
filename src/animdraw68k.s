; AmigaMCE - an ANIM's rows drawn as 16-bit pixels, the common cases by
; hand (animdraw.c's ad_row_put calls it, and does the rest itself).
; Copyright (c) 2026 Brian King
;
; Byte columns of a row of planes, 8 pixels each, at 1 or 2 screen pixels
; a picture pixel. The C version runs out of registers and reloads its
; plane pointers for every column; here everything stays in registers:
;
;   a0 the planes' walker     d0 a plane's byte (high bits kept 0)
;   a1 the plane tables       d1 a pixel's colour number (high bits 0)
;   a2 the column (plane      d2 HAM: the pixel so far
;      depth - 1's byte)      d3 columns left (dbra)
;   a3 out                    d4, d5 the 8 colour numbers (4 + 4)
;   a4, a6 its tables         d7 bytes from a plane to the next
;   a5 the gather's entry
;
; The gather is unrolled, a block a plane from plane 7 down, entered at
; the picture's top plane: each block ors that plane's table entry (8
; pixels' bits, ad_p2c[p][byte]) into d4 and d5.
;
; Assembled with vasm (Motorola syntax); 68020 addressing (scaled
; indexes, 16-bit displacements). Checked against the C version and HAM
; worked out by hand by tests/animdraw_host.c, run on a 68020 by vamos.

	machine	68020

	section	code,code

	xdef	_ad_cols68k

; unsigned long ad_cols68k(const UBYTE *col, ULONG rb, ULONG depth,
;     ULONG ncols, void *out, const void *ta, const void *tb, ULONG pix,
;     ULONG kind, const ULONG *p2c)
;
; col: the first column's byte in plane depth - 1. kind, its tables:
;   0  colours, 1:1     ta: pixels (UWORD[256])
;   1  colours, 2:1     ta: pixels doubled (ULONG[256])
;   2  HAM, 1:1         ta, tb: keep, set (UWORD[256])
;   3  HAM, 2:1         ta, tb: keep, set doubled (ULONG[256])
;   4  HAM, worked out, not written (out unused): ta, tb as 2
; pix: HAM's pixel before the first column; returned as it is after the
; last (its 16 bits). ncols at least 1.

ARGS	= 4+44

_ad_cols68k:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	ARGS+0(sp),a2
	move.l	ARGS+4(sp),d7
	move.l	ARGS+8(sp),d6
	move.l	ARGS+12(sp),d3
	move.l	ARGS+16(sp),a3
	move.l	ARGS+20(sp),a4
	move.l	ARGS+24(sp),a6
	move.l	ARGS+28(sp),d2
	move.l	ARGS+32(sp),d0
	move.l	ARGS+36(sp),a1
	subq.l	#1,d3			; for dbra

	lea	gtab(pc),a5		; the gather's entry for this depth
	move.w	-2(a5,d6.w*2),d6
	ext.l	d6
	add.l	d6,a5

	moveq	#0,d1
	cmp.w	#1,d0
	beq	k1
	cmp.w	#2,d0
	beq	k2
	cmp.w	#3,d0
	beq	k3
	cmp.w	#4,d0
	beq	k4

; ---- 0: colours, a pixel each
k0:	moveq	#0,d0
.col:	move.l	a2,a0
	moveq	#0,d4
	moveq	#0,d5
	jsr	(a5)
	rept	4
	rol.l	#8,d4
	move.b	d4,d1
	move.w	(a4,d1.w*2),(a3)+
	endr
	rept	4
	rol.l	#8,d5
	move.b	d5,d1
	move.w	(a4,d1.w*2),(a3)+
	endr
	addq.l	#1,a2
	dbra	d3,.col
	bra	done

; ---- 1: colours, each pixel twice (a long)
k1:	moveq	#0,d0
.col:	move.l	a2,a0
	moveq	#0,d4
	moveq	#0,d5
	jsr	(a5)
	rept	4
	rol.l	#8,d4
	move.b	d4,d1
	move.l	(a4,d1.w*4),(a3)+
	endr
	rept	4
	rol.l	#8,d5
	move.b	d5,d1
	move.l	(a4,d1.w*4),(a3)+
	endr
	addq.l	#1,a2
	dbra	d3,.col
	bra	done

; ---- 2: HAM, a pixel each: pix = (pix & keep[c]) | set[c]
k2:	moveq	#0,d0
.col:	move.l	a2,a0
	moveq	#0,d4
	moveq	#0,d5
	jsr	(a5)
	rept	4
	rol.l	#8,d4
	move.b	d4,d1
	and.w	(a4,d1.w*2),d2
	or.w	(a6,d1.w*2),d2
	move.w	d2,(a3)+
	endr
	rept	4
	rol.l	#8,d5
	move.b	d5,d1
	and.w	(a4,d1.w*2),d2
	or.w	(a6,d1.w*2),d2
	move.w	d2,(a3)+
	endr
	addq.l	#1,a2
	dbra	d3,.col
	bra	done

; ---- 3: HAM, each pixel twice: the pixel kept doubled, the tables too
k3:	moveq	#0,d0
	move.w	d2,d6
	swap	d2
	move.w	d6,d2
.col:	move.l	a2,a0
	moveq	#0,d4
	moveq	#0,d5
	jsr	(a5)
	rept	4
	rol.l	#8,d4
	move.b	d4,d1
	and.l	(a4,d1.w*4),d2
	or.l	(a6,d1.w*4),d2
	move.l	d2,(a3)+
	endr
	rept	4
	rol.l	#8,d5
	move.b	d5,d1
	and.l	(a4,d1.w*4),d2
	or.l	(a6,d1.w*4),d2
	move.l	d2,(a3)+
	endr
	addq.l	#1,a2
	dbra	d3,.col
	bra	done

; ---- 4: HAM worked out, not written (left of what changed)
k4:	moveq	#0,d0
.col:	move.l	a2,a0
	moveq	#0,d4
	moveq	#0,d5
	jsr	(a5)
	rept	4
	rol.l	#8,d4
	move.b	d4,d1
	and.w	(a4,d1.w*2),d2
	or.w	(a6,d1.w*2),d2
	endr
	rept	4
	rol.l	#8,d5
	move.b	d5,d1
	and.w	(a4,d1.w*2),d2
	or.w	(a6,d1.w*2),d2
	endr
	addq.l	#1,a2
	dbra	d3,.col

done:	moveq	#0,d0
	move.w	d2,d0
	movem.l	(sp)+,d2-d7/a2-a6
	rts

; ---- the gather: from plane depth - 1 (a0) down to plane 0
g7:	move.b	(a0),d0
	sub.l	d7,a0
	or.l	(7*2048,a1,d0.w*8),d4
	or.l	(7*2048+4,a1,d0.w*8),d5
g6:	move.b	(a0),d0
	sub.l	d7,a0
	or.l	(6*2048,a1,d0.w*8),d4
	or.l	(6*2048+4,a1,d0.w*8),d5
g5:	move.b	(a0),d0
	sub.l	d7,a0
	or.l	(5*2048,a1,d0.w*8),d4
	or.l	(5*2048+4,a1,d0.w*8),d5
g4:	move.b	(a0),d0
	sub.l	d7,a0
	or.l	(4*2048,a1,d0.w*8),d4
	or.l	(4*2048+4,a1,d0.w*8),d5
g3:	move.b	(a0),d0
	sub.l	d7,a0
	or.l	(3*2048,a1,d0.w*8),d4
	or.l	(3*2048+4,a1,d0.w*8),d5
g2:	move.b	(a0),d0
	sub.l	d7,a0
	or.l	(2*2048,a1,d0.w*8),d4
	or.l	(2*2048+4,a1,d0.w*8),d5
g1:	move.b	(a0),d0
	sub.l	d7,a0
	or.l	(1*2048,a1,d0.w*8),d4
	or.l	(1*2048+4,a1,d0.w*8),d5
g0:	move.b	(a0),d0
	or.l	(a1,d0.w*8),d4
	or.l	(4,a1,d0.w*8),d5
	rts

; the entry for depth 1..8
gtab:	dc.w	g0-gtab,g1-gtab,g2-gtab,g3-gtab,g4-gtab,g5-gtab,g6-gtab,g7-gtab
