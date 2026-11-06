; AmigaMCE - the spectrum's FFT: HippoPlayer's (hippo_fft.s), unchanged
; but for the C entry points at the end.
;
; From HippoPlayer, MIT licence, (c) 2017 Kari-Pekka Koljonen
; (https://github.com/koobo/HippoPlayer): Tom Roberts' fix_fft.c (1989,
; made portable by Malcolm Slaney, 1994) by hand in 68000 assembler, a
; 128-point fixed-point FFT with a Hanning window. The sine table is
; 1024 words, a sine of 32767 shifted right once.
;
; Assembled with vasm (Motorola syntax) into an AmigaOS hunk object.

	machine	68000

FFT_TEST = 0
FFT_LOOPS = 0
FFT_ALTERNATE_ASR = 1
N_WAVE = 1024
LOG2_N_WAVE = 10
FFT_SIZE = 7
FFT_LENGTH = 1<<FFT_SIZE

	section	code,code

	xdef	_hfft_window
	xdef	_hfft_fft

; void hfft_window(WORD *re, const WORD *sine)
_hfft_window:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	4+44(sp),a0
	move.l	8+44(sp),a2
	bsr	windowFFT
	movem.l	(sp)+,d2-d7/a2-a6
	rts

; void hfft_fft(WORD *re, WORD *im, const WORD *sine): re and im in place
_hfft_fft:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	4+44(sp),a0
	move.l	8+44(sp),a1
	move.l	12+44(sp),a2
	bsr	sampleFFT
	movem.l	(sp)+,d2-d7/a2-a6
	rts

sampleFFT
	; clear first half of the imaginary array
	move.l	a1,a3

	moveq	#FFT_LENGTH/2/4-1,d0
	moveq	#0,d6
.c	
	move.l	d6,(a3)+
	move.l	d6,(a3)+
	move.l	d6,(a3)+
	move.l	d6,(a3)+
	dbf	d0,.c

 
fix_fft

	rsreset
.m		rs.w	1
.k		rs.w	1
.i		rs.w	1
.l		rs.w	1
.istep		rs.w	1
.varsSizeof	rs.b	0

	movem.l	a4/a5/a6,-(sp)
	lea	-.varsSizeof(sp),sp
	;lea	.vars(pc),a5
	; m, data size 1<<7 = 128
	move	#FFT_SIZE,.m(sp)
 ifne FFT_TEST
	lea	Sinewave(pc),a2	
 endif
	; Cosine
	lea	N_WAVE/4*2(a2),a5


;; int fix_fft(fixed fr[], fixed fi[], int m, int inverse)
;; {
;;         int mr,nn,i,j,l,k,istep, n, scale, shift;
;;         fixed qr,qi,tr,ti,wr,wi,t;

;;                 n = 1<<m;

.n	= 	FFT_LENGTH

;;         if(n > N_WAVE)
;;                 return -1;

;;         mr = 0;
;	clr.w	.mr(sp)

;;         nn = n - 1;

.nn	= 	.n-1

;;         /* decimation in time - re-order data */
;;         for(m=1; m<=nn; ++m) {

	moveq	#.nn-1,d7 	; loop counter
	moveq	#.nn,d3		; loop comparison

; Cleared earlier:
;	moveq	#0,d6		; mr

	moveq	#1,d5		; m
	move	#.n,d4		; preloaded constant

	lea	(.n*2).w,a6	; loop condition constant for loop5 preloaded
	
;; ------------------------------------------------------------------
; top level loop
.loop1
; loop 1 run 127 times when FFT_LENGTH = 128
	ifne	FFT_LOOPS
	addq.l	#1,cloop1
	endif
		
;;                 l = n;

	move	d4,d0		; d4 = .n
	
;; ------------------------------------------------------------------
.loop2
; loop 2 run 247 times when FFT_LENGTH = 128
	ifne	FFT_LOOPS
	addq.l	#1,cloop2
	endif

;;                 do {
;;                         l >>= 1;

	lsr.w	#1,d0

;;                 } while(mr+l > nn);

	move.w	d6,d2	
	add.w	d0,d2
	cmp.w	d3,d2
	bhi.b	.loop2
;; ------------------------------------------------------------------

	;move.w	d0,.l(sp)

;;                 mr = (mr & (l-1)) + l;

	; d6 = mr
	move.w	d0,d1
	subq.w	#1,d1		; l-1
	and.w	d6,d1		; d6 = mr
	add.w	d0,d1
	move.w	d1,d6

;;                 if(mr <= m) continue;

	cmp.w	d5,d1		; d5 = m
	bls.b	.continue

	add	d1,d1		; mr index
	move.w	d5,d2		; m index
	add	d2,d2

;;                 tr = fr[m];
	; d0 = tr/ti
	move	(a0,d2.w),d0

;;                 fr[m] = fr[mr];

	move	(a0,d1.w),(a0,d2.w)
	
;;                 fr[mr] = tr;

	move	d0,(a0,d1.w)
	
;;                 ti = fi[m];
	
	move	(a1,d2.w),d0
	
;;                 fi[m] = fi[mr];

	move	(a1,d1.w),(a1,d2.w)

;;                 fi[mr] = ti;

	move	d0,(a1,d1.w)

;; ------------------------------------------------------------------
	; loop condition and increment
.continue
	addq	#1,d5	; d5 = m
	dbf	d7,.loop1

;;         }

;;         l = 1;

	move.w	#1*2,.l(sp) 	* index

;;         k = LOG2_N_WAVE-1;

	move.w	#LOG2_N_WAVE-1,.k(sp)
 ifeq FFT_ALTERNATE_ASR
	moveq	#15,d5		; shift for multiplications for loop 5 
 endif

;; ------------------------------------------------------------------
; top level loop 
.loop3
; loop 3 run 7 times when FFT_LENGTH = 128

	ifne	FFT_LOOPS
	addq.l	#1,cloop3
	endif

;;         while(l < n) {
;;                 /* it may not be obvious, but the shift will be performed
;;                    on each data point exactly once, during this pass. */
;;                 istep = l << 1;

	move.w	.l(sp),d0	* index
	add.w	d0,d0
	move.w	d0,.istep(sp)	* index

;;                 for(m=0; m<l; ++m) {

;	clr.w	.m(sp)
 ifne FFT_ALTERNATE_ASR
 	moveq	#0,d5
 else
	clr.w	(sp)
 endif
	
;; ------------------------------------------------------------------
.loop4
	ifne	FFT_LOOPS
	addq.l	#1,cloop4
	endif
	
; loop 4 run 127 times when FFT_LENGTH = 128

;;                         j = m << k;
;	move.w	.m(sp),d0

;	move.w	(sp),d0
;	move.w	.k(sp),d1

 ifne FFT_ALTERNATE_ASR
 	move	d5,d0
 	move	d5,d6
	move	.k(sp),d1
	lsl	d1,d0
 else
	movem.w	(sp),d0/d1	; load both .m an .k, stored sequentially
	move	d0,d6		* index .m
	lsl	d1,d0
 endif
;;                         wi = -Sinewave[j];
;;                         wi >>= 1;

	;add.w	d0,d0
	move.w	(a2,d0.w),d1
	neg.w	d1
;	asr.w	#1,d1
	move	d1,a4
;	a4 = wi

;;                         wr =  Sinewave[j+N_WAVE/4];
;;                         wr >>= 1;

;	add.w	#N_WAVE/4*2,d0
;	move.w	(a2,d0.w),d1
;	asr.w	#1,d1
;	move	d1,a3
	move	(a5,d0.w),a3

	; a3 = wr

;; ------------------------------------------------------------------
;;                         for(i=m; i<n; i+=istep) {
	;move.w	.m(sp),d6	; i
	;move.w	(sp),d6		; i - load a few lins above already
	
	move	.l(sp),d7	; j 
	add	d6,d7
 
;	add	d6,d6		; i table index
;	add	d7,d7		; j table index

	; loop increment
	; use double as d6 and d7 are word table indices
	move	.istep(sp),d0	* index
;	add	d0,d0
	; loop condition is in a6

.loop5
	ifne	FFT_LOOPS
	addq.l	#1,cloop5
	endif

;speed 162 to 160 when removed moveq #15,d5 from inside the loop

; loop 5 run 448 times when FFT_LENGTH = 128

;;                                 j = i + l;
	; step one at loop end

;;                                 tr = fix_mpy(wr,fr[j])-fix_mpy(wi,fi[j]);

	
	move	(a0,d7),d3	; fr(j)
	move	(a1,d7),d4	; fi(j)

	move.w	a3,d1		; fr(j)*wr
	muls.w	d3,d1
 ifeq FFT_ALTERNATE_ASR
	asr.l	d5,d1
 endif
	move.w	a4,d2		; fi(j)*wi
	muls.w	d4,d2
 ifeq FFT_ALTERNATE_ASR
	asr.l	d5,d2
 endif
 
	sub.l	d2,d1

 ifne FFT_ALTERNATE_ASR
	; Tricky asr #15
	; Calculations use 16-bit words with fixed point at bit 15.
	; Multiplying these will yield a value where the fixed point is at 30,
	; with one sign bit and one value bit. Doing an add.l d1,d1 could cause
	; an overflow but in practice it seems to work fine. Much faster
	; than asr.l #15.
	add.l	d1,d1
	swap	d1
 endif

	; d1 = tr
	
;;                                 qr = fr[i];
;;                                 qr >>= 1;

	move.w	(a0,d6.w),d2
	asr.w	#1,d2
	; d2 = qr

	;; Use of d5 or not:
 if 0
		;; fr[j] = qr - tr
	move	d2,d5
	sub	d1,d5
	move	d5,(a0,d7.w)

		;; fr[i] = qr + tr
	add.w	d1,d2
	move	d2,(a0,d6.w)
 else
		;; fr[j] = qr - tr
	sub	d1,d2
	move	d2,(a0,d7.w)

		;; fr[i] = qr + tr
	add.w	d1,d2
	add.w	d1,d2
	move	d2,(a0,d6.w)
 endif
;;                                 ti = fix_mpy(wr,fi[j])+fix_mpy(wi,fr[j]);

	move.w	a3,d1		; fi(j)*wr
	muls.w	d4,d1
 ifeq FFT_ALTERNATE_ASR
	asr.l	d5,d1
 endif
 
	move.w	a4,d2		; fr(j)*wi
	muls.w	d3,d2
 ifeq FFT_ALTERNATE_ASR
	asr.l	d5,d2
 endif

	add.l	d2,d1

 ifne FFT_ALTERNATE_ASR
 	; Tricky asr #15
	add.l	d1,d1
	swap	d1
 endif

	; d1 = ti

		;;  qi = fi[i];
		;;  qi >>= 1;
	move.w	(a1,d6.w),d2
	asr.w	#1,d2
	; d2 = qi


	;; Use of d5 or not:
 if 0
 		;; fi[j] = qi - ti
	move	d2,d5
	sub	d1,d5
	move	d5,(a1,d7.w)
	
		;; fi[i] = qi + ti
	add	d1,d2
	move	d2,(a1,d6.w)
 else
 		;; fi[j] = qi - ti
	sub	d1,d2
	move	d2,(a1,d7.w)
	
		;; fi[i] = qi + ti
	add	d1,d2
	add	d1,d2
	move	d2,(a1,d6.w)
 endif

;;                         }
; for loop:  for(i=m; i<n; i+=istep) 
	; add istep twice as d6 and d7 are word table indices
	; d0 = 2*istep
	add	d0,d6	; i+=istep
	add	d0,d7	; j+=istep
	; a6 = 2*.n
	cmp.w	a6,d6
	blo.b	.loop5

;;                 }

	ifne	.m
	fail 	.m referred without index, so it must be zero
	endif

;for loop: for(m=0; m<l; ++m) 	
 ifne FFT_ALTERNATE_ASR
 	addq	#1*2,d5
 	cmp	.l(sp),d5
 	blo.b	.loop4
 else 
	addq	#1*2,(sp)
	;move.w	.m(sp),d0
	move.w	(sp),d0
	cmp.w	.l(sp),d0
	blo.b	.loop4
 endif 

;;                 --k;

	subq.w	#1,.k(sp)

;;                 l = istep;

	move.w	.istep(sp),.l(sp)

;;         }

; loop condition: while(l < n) 
;	move.w	.l(sp),d0
;	cmp.w	#.n,d0
	cmp.w	#.n*2,.l(sp)
	blo.w	.loop3

;;         return scale;
;; }

	; DONE
	lea	.varsSizeof(sp),sp
	movem.l	(sp)+,a4/a5/a6
	rts

;; /*      window() - apply a Hanning window       */
;; void window(fixed fr[], int n)

; in:
;   a0 = input data
;   a2 = sinewave
windowFFT

;; {
;;         int i,j,k;

;;         j = N_WAVE/n;

	; sinewave index step
	move	#N_WAVE/FFT_LENGTH*2,d7

;;         n >>= 1;
	
	; loop end condition
	moveq	#FFT_LENGTH/2-1,d6

;;         for(i=0,k=N_WAVE/4; i<n; ++i,k+=j)
;;                 FIX_MPY(fr[i],fr[i],16384-(Sinewave[k]>>1));

	;i=0

	;k=N_WAVE/4
	;index to sinewave
	;lea	Sinewave+N_WAVE/4*2(pc),a1
	lea 	N_WAVE/4*2(a2),a1

	moveq	#15,d3		; muls shift
	move	#16384,d2
.for1
	; 16384-(Sinewave(k)>>1)
	move	(a1),d4
	;asr	#1,d4 * sinetable prescaled earlier
	move	d2,d5
	sub	d4,d5

	; fr(i)
	muls	(a0),d5
	asr.l	d3,d5
	move	d5,(a0)+

	; ++i
	;addq	#1,d0
	; k+=j
	;add	d7,d1
	add	d7,a1
	;i<n
	dbf	d6,.for1

;;         n <<= 1;

	; loop end condition
;	add	d6,d6
	moveq	#FFT_LENGTH/2-1,d6
	
;;         for(k-=j; i<n; ++i,k-=j)
;;                 FIX_MPY(fr[i],fr[i],16384-(Sinewave[k]>>1));
;; }

	; k-=j
	;sub	d7,d1
	sub	d7,a1
		
.for2
	; 16384-(Sinewave(k)>>1)
	move	(a1),d4
	;asr	#1,d4 * sinetable prescaled earlier
	move	d2,d5
	sub	d4,d5

	muls	(a0),d5
	asr.l	d3,d5
	move	d5,(a0)+

	;++i
	;addq	#1,d0
	;k-=j
	;sub	d7,d1
	sub	d7,a1
	;i<n
	dbf	d6,.for2

	rts
