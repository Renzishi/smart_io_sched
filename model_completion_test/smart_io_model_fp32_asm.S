/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/linkage.h>
#include <asm/assembler.h>

/*
 * x0: model, x1: scaler, x2: 11 raw FP32 State bit patterns,
 * x3: joint action, x4: 12 joint Q-value bit patterns.
 */
SYM_FUNC_START(smart_io_fp32_infer)
	sub	sp, sp, #320
	mov	x17, x4
	movi	v3.2s, #0

	/* Normalize and clip the prepared State: (x - mean) / std, [-5, 5]. */
	add	x6, x1, #44
	ldr	s4, [x1, #88]
	fneg	s5, s4
	mov	x9, sp
	mov	w7, #0
.Lfp32_normalize:
	ldr	s0, [x2, x7, lsl #2]
	ldr	s1, [x1, x7, lsl #2]
	ldr	s2, [x6, x7, lsl #2]
	fsub	s0, s0, s1
	fdiv	s0, s0, s2
	fmaxnm	s0, s0, s5
	fminnm	s0, s0, s4
	str	s0, [x9], #4
	add	w7, w7, #1
	cmp	w7, #11
	b.lo	.Lfp32_normalize

	/* First shared layer: 11 -> 32. */
	mov	x5, x0
	mov	x16, #1408
	add	x6, x0, x16
	add	x9, sp, #64
	mov	w7, #0
.Lfp32_l1_output:
	ldr	s0, [x6, x7, lsl #2]
	mov	x10, sp
	mov	w8, #0
.Lfp32_l1_input:
	ldr	s1, [x10, x8, lsl #2]
	ldr	s2, [x5], #4
	fmadd	s0, s1, s2, s0
	add	w8, w8, #1
	cmp	w8, #11
	b.lo	.Lfp32_l1_input
	fmaxnm	s0, s0, s3
	str	s0, [x9], #4
	add	w7, w7, #1
	cmp	w7, #32
	b.lo	.Lfp32_l1_output

	/* Second shared layer: 32 -> 32. */
	mov	x16, #1536
	add	x5, x0, x16
	mov	x16, #5632
	add	x6, x0, x16
	add	x9, sp, #192
	mov	w7, #0
.Lfp32_l2_output:
	ldr	s0, [x6, x7, lsl #2]
	add	x10, sp, #64
	mov	w8, #0
.Lfp32_l2_input:
	ldr	s1, [x10], #4
	ldr	s2, [x5], #4
	fmadd	s0, s1, s2, s0
	add	w8, w8, #1
	cmp	w8, #32
	b.lo	.Lfp32_l2_input
	fmaxnm	s0, s0, s3
	str	s0, [x9], #4
	add	w7, w7, #1
	cmp	w7, #32
	b.lo	.Lfp32_l2_output

	/* Joint action head: 32 -> 12. */
	mov	x16, #5760
	add	x5, x0, x16
	mov	x16, #7296
	add	x6, x0, x16
	mov	w7, #0
	mov	w12, #0
.Lfp32_joint_output:
	ldr	s0, [x6, x7, lsl #2]
	add	x10, sp, #192
	mov	w8, #0
.Lfp32_joint_input:
	ldr	s1, [x10], #4
	ldr	s2, [x5], #4
	fmadd	s0, s1, s2, s0
	add	w8, w8, #1
	cmp	w8, #32
	b.lo	.Lfp32_joint_input
	str	s0, [x17, x7, lsl #2]
	cbnz	w7, .Lfp32_joint_compare
	fmov	s4, s0
	b	.Lfp32_joint_next
.Lfp32_joint_compare:
	fcmp	s0, s4
	b.le	.Lfp32_joint_next
	fmov	s4, s0
	mov	w12, w7
.Lfp32_joint_next:
	add	w7, w7, #1
	cmp	w7, #12
	b.lo	.Lfp32_joint_output
	str	w12, [x3]

	add	sp, sp, #320
	ret
SYM_FUNC_END(smart_io_fp32_infer)
