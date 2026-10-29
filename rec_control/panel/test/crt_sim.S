/* Start code of the M16C simulator programs.
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
	jsr.a	0x0EF200
	rts


	.global	_call_init
_call_init:
	pushm	r1,r2,r3,a0,a1,sb,fb
	jsr.a	MOD_INIT_ADDR
	popm	r1,r2,r3,a0,a1,sb,fb
	rts


	.global	_call_step0
_call_step0:
	pushm	r1,r2,r3,a0,a1,sb,fb
	mov.b	#0,r1l
	mov.w	#0x3000,r2
	jsr.a	0x0D10E0
	popm	r1,r2,r3,a0,a1,sb,fb
	rts


	.global	_get_sp
_get_sp:
	stc	sp,r0
	rts


	.global	_call_parse
_call_parse:
	pushm	r1,r2,r3,a0,a1,sb,fb
	jsr.a	0x0C8650
	popm	r1,r2,r3,a0,a1,sb,fb
	rts


	.global	_call_stock_frame
_call_stock_frame:
	pushm	r1,r2,r3,a0,a1,sb,fb
	jsr.a	0x0C8650
	jsr.a	0x0C68F0
	jsr.a	0x0C68F0
	jsr.a	0x0C68F0
	jsr.a	0x0C68F0
	popm	r1,r2,r3,a0,a1,sb,fb
	rts


#ifndef SIM_PANEL_FILE
#define SIM_PANEL_FILE "build/sim_panel.bin"
#endif
	.section .panel,"ax"
	.incbin	SIM_PANEL_FILE
