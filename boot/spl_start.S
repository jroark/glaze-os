/*
 * Stage-1 SPL entry stub for the BE-300.
 *
 * Linked at 0x80F00000. The boot ROM's record walker loads this binary into
 * SDRAM via the partition-1 B000FF container and `jr`s to 0x80F00004 (the
 * record terminator's entry value). The first 4 bytes are a NOP veneer to
 * mirror the WinCE SPL's record-6 layout.
 */

	.set	noreorder
	.set	noat
	.set	mips2

	.text
	.globl	_start
_start:
	/* 0x80F00000: alignment NOP (boot ROM jumps to +4) */
	nop
	mtc0	$zero, $12        /* interrupts, BEV and ERL clear */
	mtc0	$zero, $13        /* clear software-pending interrupts */
	/* Run code and stack uncached before C touches memory. The ROM loaded
	 * the SPL through aliases whose cache state we must not inherit. */
	la	$t0, 1f
	lui	$t1, 0x2000
	or	$t0, $t0, $t1
	jr	$t0
	nop
1:	nop
	nop
	nop
	/* Index operations are safe before the VR4131 Hit-op workaround.
	 * Flush ROM cache lines before copying any kernel data through KSEG1. */
	.set	push
	.set	mips3
	lui	$t0, 0x8000
	ori	$t1, $t0, 0x8000
2:	cache	0x01, 0($t0)
	cache	0x00, 0($t0)
	addiu	$t0, $t0, 16
	bne	$t0, $t1, 2b
	nop
	sync
	.set	pop
	li	$sp, 0xA0FF0000   /* uncached stack, 64 KiB below SDRAM end */
	move	$gp, $zero
	/* BSS is absent from the container's file-backed ELF segments. */
	la	$t0, __bss_start
	la	$t1, __bss_end
	lui	$t2, 0x2000
	or	$t0, $t0, $t2
	or	$t1, $t1, $t2
3:	beq	$t0, $t1, 4f
	nop
	sw	$zero, 0($t0)
	b	3b
	addiu	$t0, $t0, 4
4:	la	$t0, spl_main
	or	$t0, $t0, $t2
	jalr	$t0
	nop
	move	$t0, $v0          /* spl_main returns the kernel entry VA */
	/* MIPS Linux head.S stores a0..a3 to fw_arg0..3 (cmdline argc/argv
	 * etc). The kernel's prom_init then dereferences these. Pass zeros
	 * so the kernel sees "no boot arguments" rather than random pointers
	 * left in the registers by spl_main. */
	move	$a0, $zero
	move	$a1, $zero
	move	$a2, $zero
	move	$a3, $zero
	jr	$t0
	nop
