/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The last code the old kernel runs: copied to a page above everything the new
 * kernel overwrites and run uncached (KSEG1), with interrupts off and the
 * second core held in reset. Copies the image's pages to its load address,
 * writes back and invalidates the L1 data, L2 and L1 instruction caches, and
 * jumps to the image's entry with the boot loader's arguments.
 * Position independent: only branches, no absolute addresses.
 *
 * a0 = struct c5_kexec_args * (KSEG0), see c5_kexec.c
 */
#include <asm/asm.h>
#include <asm/regdef.h>

	.text
	.set	push
	.set	noreorder
	.set	noat
	.set	mips32r2

	.globl	c5_kexec_tramp
	.globl	c5_kexec_tramp_end
c5_kexec_tramp:
	lw	t0, 0(a0)		/* list: KSEG0 addresses of the image's pages */
	lw	t1, 4(a0)		/* count */
	lw	t2, 8(a0)		/* dest: KSEG0 load address */

1:	beqz	t1, 3f
	 nop
	lw	t3, 0(t0)		/* this page */
	addiu	t4, t3, 4096
2:	lw	t5, 0(t3)
	lw	t6, 4(t3)
	lw	t7, 8(t3)
	lw	t8, 12(t3)
	sw	t5, 0(t2)
	sw	t6, 4(t2)
	sw	t7, 8(t2)
	sw	t8, 12(t2)
	addiu	t3, t3, 16
	bne	t3, t4, 2b
	 addiu	t2, t2, 16
	addiu	t0, t0, 4
	b	1b
	 addiu	t1, t1, -1

	/* L1 data: Index_Writeback_Inv_D over every set and way */
3:	lw	t3, 24(a0)		/* dsize */
	lw	t4, 28(a0)		/* dline */
	lui	t5, 0x8000
	addu	t6, t5, t3
4:	cache	0x01, 0(t5)
	addu	t5, t5, t4
	bne	t5, t6, 4b
	 nop
	sync

	/* L2: Index_Writeback_Inv_SD */
	lw	t3, 40(a0)		/* ssize */
	lw	t4, 44(a0)		/* sline */
	beqz	t3, 6f
	 nop
	lui	t5, 0x8000
	addu	t6, t5, t3
5:	cache	0x03, 0(t5)
	addu	t5, t5, t4
	bne	t5, t6, 5b
	 nop
	sync

	/* L1 instruction: Index_Invalidate_I */
6:	lw	t3, 32(a0)		/* isize */
	lw	t4, 36(a0)		/* iline */
	lui	t5, 0x8000
	addu	t6, t5, t3
7:	cache	0x00, 0(t5)
	addu	t5, t5, t4
	bne	t5, t6, 7b
	 nop
	sync

	/* As U-Boot: a0 = argc, a1 = argv, a2 = envp, a3 = 0 */
	lw	t9, 12(a0)		/* entry */
	lw	a1, 20(a0)
	move	a2, zero
	move	a3, zero
	lw	a0, 16(a0)
	jr.hb	t9
	 nop
c5_kexec_tramp_end:
	.set	pop
