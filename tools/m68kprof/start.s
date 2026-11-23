| m68kprof's 68k entry: first in the code hunk; the host sets the stack
	.text
	.globl	_start
_start:
	jsr	_t_main
1:	bra.s	1b
