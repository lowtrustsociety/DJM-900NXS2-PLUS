/* Entry points from the stock panel firmware.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
#include "panel.h"
#include "menu.h"

	.section .text.entry,"ax"

	.global	_mod_entry
_mod_entry:
	stc	sp,a0
	mov.w	a0,MOD_SAVED_SP
	ldc	#MOD_STACK_TOP,sp
	pushm	r1,r2,r3,a1,sb,fb
	jsr.a	_hook_render
	popm	r1,r2,r3,a1,sb,fb
	mov.w	MOD_SAVED_SP,a0
	ldc	a0,sp
	jmp.a	0x0C68F0

	.global	_mod_init_entry
_mod_init_entry:
	stc	sp,a0
	mov.w	a0,MOD_SAVED_SP
	ldc	#MOD_STACK_TOP,sp
	pushm	r1,r2,r3,a1,sb,fb
	jsr.a	_mod_init
	popm	r1,r2,r3,a1,sb,fb
	mov.w	MOD_SAVED_SP,a0
	ldc	a0,sp
	jmp.a	0x0C6836


	.global	_tx_entry
_tx_entry:
	push.w	r2
	push.b	r1l
	jsr.a	_tx_pre
	pop.b	r1l
	pop.w	r2
	push.w	r2
	push.b	r0l
	jsr.a	0x0CAE90
	pop.b	r1l
	pop.w	r2
	jmp.a	_tx_post


	.global	_far_byte
_far_byte:
	push.w	a1
	mov.w	r1,a0
	mov.w	r2,a1
	lde.b	[a1a0],r0l
	pop.w	a1
	rts


	.section .text.tramp,"ax"
	.global	_tx_tramp
_tx_tramp:
	jmp.a	_tx_entry
