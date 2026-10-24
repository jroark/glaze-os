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

	.section .bss
	.align	4
	.globl	kstack
kstack:
	.space	16384
	.globl	kstack_top
kstack_top:
