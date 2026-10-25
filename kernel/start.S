/*
 * Glaze kernel entry. The SPL copies the flat kernel image to its link
 * address (KSEG0 0x80010000) and jumps here with interrupts disabled and
 * a0 = 0. GlazeBoot (a RAM boot from Windows CE) passes a0 = its
 * struct gz_bootinfo (glaze/bootinfo.h), which kmain gets.
 */

	.set	noreorder
	.set	mips2

	.section .text.start, "ax"
	.globl	_start
_start:
	move	$s0, $a0

	/*
	 * The first VR4131 revisions (PRId 0x0C80-0x0C82) have a CACHE
	 * instruction bug: without Linux's workaround the kernel's Hit ops
	 * corrupted cached data on a BE-300 (arch/mips/mm/c-r4k.c, CPU_VR4131:
	 * Config bit 22, and on 0x0C80 also bit 16, VR41_CONF_BP). The write
	 * also changes the cache line size, which loses dirty lines, so it
	 * runs here, uncached, between two write-back-and-invalidate passes
	 * over both 16 KiB caches (VR4131 UM ch. 23 CACHE ops 0x01 and 0x00,
	 * both ways, 16-byte steps), before anything is stored through KSEG0.
	 */
	.set	push
	.set	mips3
	la	$t0, 1f
	lui	$t1, 0x2000
	or	$t0, $t0, $t1		/* the KSEG1 alias */
	jr	$t0
	nop
1:	mfc0	$t2, $15		/* PRId */
	nop
	andi	$t2, $t2, 0xFFFF
	sltiu	$t3, $t2, 0x0C80
	bnez	$t3, 5f
	sltiu	$t3, $t2, 0x0C83
	beqz	$t3, 5f
	nop
	jal	cache_flush_all
	nop
	mfc0	$t3, $16		/* Config */
	nop
	lui	$t4, 0x0040		/* bit 22 */
	or	$t3, $t3, $t4
	li	$t4, 0x0C80
	bne	$t2, $t4, 4f
	lui	$t4, 0x0001		/* bit 16, VR41_CONF_BP (0x0C80 only) */
	or	$t3, $t3, $t4
4:	mtc0	$t3, $16
	nop
	nop
	nop
	nop
	jal	cache_flush_all
	nop
5:	la	$t0, 6f			/* back to KSEG0 */
	jr	$t0
	nop
6:	.set	pop
	la	$sp, kstack_top
	move	$gp, $zero

	/* Zero .bss (not present in the flat image). */
	la	$t0, __bss_start
	la	$t1, __bss_end
1:	beq	$t0, $t1, 2f
	nop
	sw	$zero, 0($t0)
	b	1b
	addiu	$t0, $t0, 4

2:	jal	kmain
	move	$a0, $s0
3:	b	3b
	nop

/* Write back and invalidate both caches by index (uses t0, t1; called
 * through KSEG1 so it is not itself cached). */
cache_flush_all:
	.set	push
	.set	mips3
	lui	$t0, 0x8000
	ori	$t1, $t0, 0x8000
7:	cache	0x01, 0($t0)		/* Index_Writeback_Invalidate_D */
	cache	0x00, 0($t0)		/* Index_Invalidate_I */
	addiu	$t0, $t0, 16
	bne	$t0, $t1, 7b
	nop
	sync
	jr	$ra
	nop
	.set	pop

	.section .bss
	.align	4
	.globl	kstack
kstack:
	.space	16384
	.globl	kstack_top
kstack_top:
