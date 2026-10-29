/* Start code of the indicator test in the M16C simulator.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
	.section .text.crt,"ax"
	.global	_start
_start:
	ldc	#0x4f00,sp
	jsr.a	_harness
	mov.w	#0,r1
	mov.b	#1,0x400
1:	jmp.b	1b

	.global	_call_hook
_call_hook:
	pushm	r1,r2,r3,a0,a1,sb,fb
	jsr.a	HOOK_ADDR
	popm	r1,r2,r3,a0,a1,sb,fb
	rts

	.global	_call_init
_call_init:
	pushm	r1,r2,r3,a0,a1,sb,fb
	jsr.a	INIT_ADDR
	popm	r1,r2,r3,a0,a1,sb,fb
	rts


	.global	_call_parse
_call_parse:
	pushm	r1,r2,r3,a0,a1,sb,fb
	jsr.a	0x0C8650
	popm	r1,r2,r3,a0,a1,sb,fb
	rts

	.section .panel,"ax"
	.incbin	SIM_PANEL_FILE
