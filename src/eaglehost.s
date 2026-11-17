; Ambiance - the register-level side of hosting EaglePlayer/DeliTracker
; players (eagle.c has the rest)
; Copyright (c) 2026 Brian King
;
; Players are called with A5 pointing at the globals Ambiance gives them,
; and call back through the function pointers in it and through the jump
; table below it (6-byte JMPs at negative offsets), with their arguments in
; registers. The calls in, the callbacks and the jump table targets are
; here; whatever needs the OS or C is passed on to eagle.c.
;
; The callbacks follow HippoPlayer's EaglePlayer host (puu016.s, MIT
; licence, (c) 2017 Kari-Pekka Koljonen): the same choice of what to do
; and what to leave as a stub, which a great many players are known to run
; under.
;
; Assembled with vasm (Motorola syntax) into an AmigaOS hunk object.

	machine	68000

; offsets into the globals (DeliTracker and EaglePlayer interfaces)
dtg_DirArrayPtr		equ	24
dtg_FileArrayPtr	equ	28
dtg_PathArrayPtr	equ	32
dtg_Timer		equ	54

CUSTOM			equ	$dff000
DMACON			equ	$096
AUD0			equ	$0a0

	xref	_ehc_getlistdata
	xref	_ehc_loadfile
	xref	_ehc_cutsuffix
	xref	_ehc_settimer
	xref	_ehc_alloclist
	xref	_ehc_modchange
	xref	_ehc_clearcache
	xref	_eh_master
	xref	_eh_len
	xref	_eh_d1
	xref	_eh_a0
	xref	_eh_songend
	xref	_eh_hit
	xref	_eh_vol
	xref	_eh_adr
	xref	_eh_plen
	xref	_eh_per
	xref	_eh_sadr
	xref	_eh_slen
	xref	_eh_trig

	xdef	_eh_call
	xdef	_eh_icall
	xdef	_eh_enpp
	xdef	_ehs_getlistdata
	xdef	_ehs_loadfile
	xdef	_ehs_copydir
	xdef	_ehs_copyfile
	xdef	_ehs_copystring
	xdef	_ehs_ok
	xdef	_ehs_songend
	xdef	_ehs_cutsuffix
	xdef	_ehs_settimer
	xdef	_ehs_waitdma
	xdef	_ehs_alloclist
	xdef	_ehs_modchange
	xdef	_ehs_clearcache

	section	code,code

;------------------------------------------------------------ calls in
; LONG eh_call(APTR fn, APTR base, LONG d0, LONG d1): a player function,
; A5 = base; D1 and A0 as it left them in eh_d1, eh_a0.
_eh_call:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a0
	move.l	52(sp),a5
	move.l	56(sp),d0
	move.l	60(sp),d1
	jsr	(a0)
	move.l	d1,_eh_d1
	move.l	a0,_eh_a0
	movem.l	(sp)+,d2-d7/a2-a6
	rts

; LONG eh_icall(APTR fn, APTR base, LONG d0): the same, from the interrupt
; (nothing kept but D0)
_eh_icall:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a0
	move.l	52(sp),a5
	move.l	56(sp),d0
	jsr	(a0)
	movem.l	(sp)+,d2-d7/a2-a6
	rts

;------------------------------------------------------------ callbacks
; GetListData(D0 = number): A0 = where, D0 = size
_ehs_getlistdata:
	movem.l	d1-d7/a1-a6,-(sp)
	move.l	d0,-(sp)
	jsr	_ehc_getlistdata
	addq.l	#4,sp
	move.l	d0,a0
	move.l	_eh_len,d0
	movem.l	(sp)+,d1-d7/a1-a6
	rts

; LoadFile(): the file named in the path array, loaded; D0 = 0 if it was
_ehs_loadfile:
	movem.l	d1-d7/a0-a6,-(sp)
	move.l	a5,-(sp)
	jsr	_ehc_loadfile
	addq.l	#4,sp
	movem.l	(sp)+,d1-d7/a0-a6
	rts

; CopyDir(), CopyFile(), CopyString(A0): appended to the path array
_ehs_copydir:
	move.l	a0,-(sp)
	move.l	dtg_DirArrayPtr(a5),a0
	bra.b	append
_ehs_copyfile:
	move.l	a0,-(sp)
	move.l	dtg_FileArrayPtr(a5),a0
	bra.b	append
_ehs_copystring:
	move.l	a0,-(sp)
append:
	move.l	a1,-(sp)
	move.l	dtg_PathArrayPtr(a5),a1
.end:	tst.b	(a1)+
	bne.b	.end
	subq.l	#1,a1
	move.l	a0,d0
	beq.b	.done
.copy:	move.b	(a0)+,(a1)+
	bne.b	.copy
.done:	move.l	(sp)+,a1
	move.l	(sp)+,a0
	moveq	#0,d0
	rts

; AudioAlloc, AudioFree, StartInt, StopInt, LockScreen...: Ambiance holds
; the channels and runs the timer itself; D0 = 0 (fine / none)
_ehs_ok:
	moveq	#0,d0
	rts

; SongEnd(): every register kept
_ehs_songend:
	st	_eh_songend
	rts

_ehs_cutsuffix:
	movem.l	d0-d7/a0-a6,-(sp)
	move.l	a5,-(sp)
	jsr	_ehc_cutsuffix
	addq.l	#4,sp
	movem.l	(sp)+,d0-d7/a0-a6
	moveq	#0,d0
	rts

; SetTimer(): the timer from dtg_Timer, every register kept
_ehs_settimer:
	movem.l	d0-d7/a0-a6,-(sp)
	moveq	#0,d0
	move.w	dtg_Timer(a5),d0
	move.l	d0,-(sp)
	jsr	_ehc_settimer
	addq.l	#4,sp
	movem.l	(sp)+,d0-d7/a0-a6
	rts

; WaitAudioDMA(): a dozen raster lines, every register kept
_ehs_waitdma:
	movem.l	d0/d1,-(sp)
	moveq	#12-1,d1
.line:	move.b	$dff006,d0
.same:	cmp.b	$dff006,d0
	beq.b	.same
	dbf	d1,.line
	movem.l	(sp)+,d0/d1
	rts

; AllocListData(D0 = size, D1 = flags): D0 = memory or 0
_ehs_alloclist:
	movem.l	d1-d7/a0-a6,-(sp)
	move.l	d1,-(sp)
	move.l	d0,-(sp)
	jsr	_ehc_alloclist
	addq.l	#8,sp
	movem.l	(sp)+,d1-d7/a0-a6
	rts

_ehs_modchange:
	movem.l	d0-d7/a0-a6,-(sp)
	move.l	a5,-(sp)
	jsr	_ehc_modchange
	addq.l	#4,sp
	movem.l	(sp)+,d0-d7/a0-a6
	rts

_ehs_clearcache:
	movem.l	d0-d7/a0-a6,-(sp)
	jsr	_ehc_clearcache
	movem.l	(sp)+,d0-d7/a0-a6
	rts

;------------------------------------------------------------ Paula, for players
; DMAMask(D0.w < 0: on, else off; D1 = channels). Channels turned on
; start a note: where they start is kept for the spectrum (eh_sadr,
; eh_slen, eh_trig), the address and length poked last.
enpp_dmamask:
	movem.l	d0-d2/a0-a1,-(sp)
	and.w	#$000f,d1
	tst.w	d0
	bpl.b	.off
	or.b	d1,_eh_hit
	or.b	d1,_eh_trig
	moveq	#0,d2
.note:	btst	d2,d1
	beq.b	.next
	move.w	d2,d0
	lsl.w	#2,d0
	lea	_eh_adr,a0
	lea	_eh_sadr,a1
	move.l	0(a0,d0.w),0(a1,d0.w)
	lsr.w	#1,d0
	lea	_eh_plen,a0
	lea	_eh_slen,a1
	move.w	0(a0,d0.w),0(a1,d0.w)
.next:	addq.w	#1,d2
	cmp.w	#4,d2
	bne.b	.note
	or.w	#$8000,d1
.off:	move.w	d1,CUSTOM+DMACON
	bsr	_ehs_waitdma
	movem.l	(sp)+,d0-d2/a0-a1
	rts

; PokeAdr(D0 = address, D1 = channel)
enpp_pokeadr:
	movem.l	d1/a0,-(sp)
	and.w	#3,d1
	lsl.w	#2,d1
	lea	_eh_adr,a0
	move.l	d0,0(a0,d1.w)
	lsl.w	#2,d1
	lea	CUSTOM+AUD0,a0
	move.l	d0,0(a0,d1.w)
	movem.l	(sp)+,d1/a0
	rts

; PokeLen(D0 = words, D1 = channel)
enpp_pokelen:
	movem.l	d0/d1/a0,-(sp)
	and.w	#3,d1
	tst.w	d0
	bne.b	.some
	moveq	#1,d0
.some:	add.w	d1,d1
	lea	_eh_plen,a0
	move.w	d0,0(a0,d1.w)
	lsl.w	#3,d1
	lea	CUSTOM+AUD0,a0
	move.w	d0,4(a0,d1.w)
	movem.l	(sp)+,d0/d1/a0
	rts

; PokePer(D0 = period, D1 = channel)
enpp_pokeper:
	movem.l	d1/a0,-(sp)
	and.w	#3,d1
	add.w	d1,d1
	lea	_eh_per,a0
	move.w	d0,0(a0,d1.w)
	lsl.w	#3,d1
	lea	CUSTOM+AUD0,a0
	move.w	d0,6(a0,d1.w)
	movem.l	(sp)+,d1/a0
	rts

; PokeVol(D0 = volume, D1 = channel): Ambiance's volume applied
enpp_pokevol:
	movem.l	d0/d1/a0,-(sp)
	and.w	#3,d1
	lea	_eh_vol,a0
	move.b	d0,0(a0,d1.w)
	lsl.w	#4,d1
	mulu	_eh_master,d0
	lsr.l	#6,d0
	lea	CUSTOM+AUD0,a0
	move.w	d0,8(a0,d1.w)
	movem.l	(sp)+,d0/d1/a0
	rts

; PokeCommand(D0 = command, D1 = argument): 1 = the filter (LED)
enpp_pokecommand:
	cmp.b	#1,d0
	bne.b	.not
	tst.b	d1
	beq.b	.off
	bclr	#1,$bfe001
.not:	rts
.off:	bset	#1,$bfe001
	rts

;------------------------------------------------------------ the jump table
; Its targets, in order from -6 down (eagle.c writes JMPs to them under
; the globals). Unlisted ones are stubs (D0 = 0), as in HippoPlayer.
_eh_enpp:
	dc.l	_ehs_ok			; AllocSampleStruct
	dc.l	_ehs_ok			; NewLoadFile2
	dc.l	_ehs_ok			; MakeDirCorrect
	dc.l	_ehs_ok			; TestAufHide
	dc.l	_ehs_clearcache		; ClearCache
	dc.l	_ehs_ok			; CopyMemQuick
	dc.l	_ehs_ok			; GetPassword
	dc.l	_ehs_ok			; StringCopy2
	dc.l	_ehs_ok			; ScreenToFront
	dc.l	_ehs_ok			; WindowToFront
	dc.l	_ehs_getlistdata	; GetListData
	dc.l	_ehs_loadfile		; LoadFile
	dc.l	_ehs_copydir		; CopyDir
	dc.l	_ehs_copyfile		; CopyFile
	dc.l	_ehs_copystring		; CopyString
	dc.l	_ehs_ok			; AllocAudio
	dc.l	_ehs_ok			; FreeAudio
	dc.l	_ehs_ok			; StartInterrupt
	dc.l	_ehs_ok			; StopInterrupt
	dc.l	_ehs_songend		; SongEnd
	dc.l	_ehs_cutsuffix		; CutSuffix
	dc.l	_ehs_settimer		; SetTimer
	dc.l	_ehs_waitdma		; WaitAudioDMA
	dc.l	_ehs_ok			; SaveMem
	dc.l	_ehs_ok			; FileReq
	dc.l	_ehs_ok			; TextRequest
	dc.l	_ehs_ok			; LoadExecutable
	dc.l	_ehs_ok			; NewLoadFile
	dc.l	_ehs_ok			; ScrollText
	dc.l	_ehs_ok			; LoadPlConfig
	dc.l	_ehs_ok			; SavePlConfig
	dc.l	_ehs_ok			; FindTag
	dc.l	_ehs_ok			; FindAuthor
	dc.l	_ehs_ok			; Hexdez
	dc.l	_ehs_ok			; TypeText
	dc.l	_ehs_modchange		; ModuleChange
	dc.l	_ehs_ok			; ModuleRestore
	dc.l	_ehs_ok			; StringCopy
	dc.l	_ehs_ok			; CalcStringSize
	dc.l	_ehs_ok			; StringCMP
	dc.l	enpp_dmamask		; DMAMask
	dc.l	enpp_pokeadr		; PokeAdr
	dc.l	enpp_pokelen		; PokeLen
	dc.l	enpp_pokeper		; PokePer
	dc.l	enpp_pokevol		; PokeVol
	dc.l	enpp_pokecommand	; PokeCommand
	dc.l	_ehs_ok			; Amplifier
	dc.l	_ehs_ok			; TestAbortGadget
	dc.l	_ehs_ok			; GetEPNrfromMessage
	dc.l	_ehs_ok			; InitDisplay
	dc.l	_ehs_ok			; FillDisplay
	dc.l	_ehs_ok			; RemoveDisplay
	dc.l	_ehs_ok			; GetLocaleString
	dc.l	_ehs_ok			; SetWaitPointer
	dc.l	_ehs_ok			; ClearWaitPointer
	dc.l	_ehs_ok			; OpenCatalog
	dc.l	_ehs_ok			; CloseCatalog
	dc.l	_ehs_ok			; AllocAmigaAudio
	dc.l	_ehs_ok			; FreeAmigaAudio
	dc.l	_ehs_ok			; RawToFormat
	dc.l	_ehs_ok			; FindAmplifier
	dc.l	_ehs_ok			; UserCallup5
	dc.l	_ehs_ok			; GetLoadListData
	dc.l	_ehs_ok			; SetListData
	dc.l	_ehs_ok			; GetHardwareType
	dc.l	0

	end
