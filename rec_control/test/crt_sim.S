/* Start code of the SH simulator programs.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
	.text
	.global	_start
_start:
	mov.l	stack,r15
	mov.l	harness,r0
	jsr	@r0
	nop
	mov	#1,r4
	mov	#0,r5
	trapa	#34
1:	bra	1b
	nop
	.align	2
stack:	.long	0x00400000
harness:	.long	_harness

	.global	_sim_write
_sim_write:
	mov	r4,r6
	mov	r5,r7
	mov	#1,r5
	mov	#4,r4
	trapa	#34
	rts
	nop
