/*
 * Hibernate context (docs/hardware/POWER.md "Power off"). power_save() records the
 * callee-saved registers like setjmp and returns 0; once the SPL finds the
 * resume record valid after a HIBERNATE wake, it jumps to
 * power_resume_entry through KSEG1 with a0 = the record, and power_save()
 * returns 1 in the context it saved.
 */
#include "resume.h"

	.set	noreorder
	.set	mips2

	.text
/* int power_save(uint32_t ctx[GZ_CTX_WORDS]) */
	.globl	power_save
power_save:
	sw	$s0, (GZ_CTX_S0 + 0) * 4($a0)
	sw	$s1, (GZ_CTX_S0 + 1) * 4($a0)
	sw	$s2, (GZ_CTX_S0 + 2) * 4($a0)
	sw	$s3, (GZ_CTX_S0 + 3) * 4($a0)
	sw	$s4, (GZ_CTX_S0 + 4) * 4($a0)
	sw	$s5, (GZ_CTX_S0 + 5) * 4($a0)
	sw	$s6, (GZ_CTX_S0 + 6) * 4($a0)
	sw	$s7, (GZ_CTX_S0 + 7) * 4($a0)
	sw	$fp, GZ_CTX_FP * 4($a0)
	sw	$sp, GZ_CTX_SP * 4($a0)
	sw	$ra, GZ_CTX_RA * 4($a0)
	sw	$gp, GZ_CTX_GP * 4($a0)
	mfc0	$t0, $12		/* Status */
	nop
	sw	$t0, GZ_CTX_STATUS * 4($a0)
	mfc0	$t0, $10		/* EntryHi: the current ASID */
	nop
	sw	$t0, GZ_CTX_ENTRYHI * 4($a0)
	jr	$ra
	move	$v0, $zero

/*
 * From the SPL, uncached, with Status cleared. The reset left the caches
 * undefined and the VR4131 cache workaround undone (start.S), so that runs
 * first, still from KSEG1; then Status (no BEV or ERL, interrupts off) and
 * the ASID come back before power_save() returns into the kernel.
 */
	.globl	power_resume_entry
power_resume_entry:
	move	$s0, $a0
	jal	cpu_cache_setup		/* stays in KSEG1 */
	nop
	la	$t0, 1f			/* KSEG0 from here */
	jr	$t0
	nop
1:	lui	$t1, 0x8000
	or	$s0, $s0, $t1		/* the record's KSEG0 alias */
	addiu	$a0, $s0, GZ_RESUME_CTX_OFF
	lw	$t0, GZ_CTX_STATUS * 4($a0)
	li	$t1, ~1			/* IE stays off until the kernel eret */
	and	$t0, $t0, $t1
	mtc0	$t0, $12
	nop
	nop
	nop
	lw	$t0, GZ_CTX_ENTRYHI * 4($a0)
	mtc0	$t0, $10
	nop
	nop
	nop
	lw	$s0, (GZ_CTX_S0 + 0) * 4($a0)
	lw	$s1, (GZ_CTX_S0 + 1) * 4($a0)
	lw	$s2, (GZ_CTX_S0 + 2) * 4($a0)
	lw	$s3, (GZ_CTX_S0 + 3) * 4($a0)
	lw	$s4, (GZ_CTX_S0 + 4) * 4($a0)
	lw	$s5, (GZ_CTX_S0 + 5) * 4($a0)
	lw	$s6, (GZ_CTX_S0 + 6) * 4($a0)
	lw	$s7, (GZ_CTX_S0 + 7) * 4($a0)
	lw	$fp, GZ_CTX_FP * 4($a0)
	lw	$sp, GZ_CTX_SP * 4($a0)
	lw	$ra, GZ_CTX_RA * 4($a0)
	lw	$gp, GZ_CTX_GP * 4($a0)
	jr	$ra
	li	$v0, 1
