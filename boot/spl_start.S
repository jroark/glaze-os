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
	/* 0x80F00004: entry — disable interrupts, set up sp, call C main, jump
	 * to the value spl_main returns. */
	mtc0	$zero, $12        /* CP0 Status: clear all (ERL=0, BEV=stays, IE=0) */
	mtc0	$zero, $13        /* CP0 Cause: clear pending */
	li	$sp, 0x80FF0000   /* stack high in SDRAM, 64 KiB below 16 MiB */
	move	$gp, $zero
	jal	spl_main
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
