/*
 * Exception vectors, trap entry/exit, and kernel context switch.
 *
 * The kernel never touches user virtual addresses directly (user memory is
 * reached through page-table walks to KSEG0), so the TLB refill vector only
 * ever services user-mode misses and needs no kernel-mode special cases.
 */

#include "mips.h"
#include "trapframe.h"

	.set	noreorder
	.set	noat
	.set	mips3

/*
 * TLB refill, copied to 0x80000000. Two-level walk of cur_pgd: every PGD
 * slot points at a page table (an all-invalid shared table when empty), so
 * the handler has no branches. An invalid PTE pair makes the retried access
 * raise TLBL/TLBS through the general vector, where it becomes a fault.
 */
	.globl	vec_refill
	.globl	vec_refill_end
vec_refill:
	lui	$k1, %hi(cur_pgd)
	lw	$k1, %lo(cur_pgd)($k1)
	mfc0	$k0, CP0_BADVADDR
	srl	$k0, $k0, 22
	sll	$k0, $k0, 2
	addu	$k1, $k1, $k0
	lw	$k1, 0($k1)
	mfc0	$k0, CP0_BADVADDR
	srl	$k0, $k0, 10
	andi	$k0, $k0, 0xFF8
	addu	$k1, $k1, $k0
	lw	$k0, 0($k1)
	lw	$k1, 4($k1)
	mtc0	$k0, CP0_ENTRYLO0
	mtc0	$k1, CP0_ENTRYLO1
	nop
	nop
	nop
	tlbwr
	nop
	nop
	nop
	eret
vec_refill_end:

/* General exception stub, copied to 0x80000180. */
	.globl	vec_general
	.globl	vec_general_end
vec_general:
	lui	$k0, %hi(general_exception)
	addiu	$k0, $k0, %lo(general_exception)
	jr	$k0
	nop
vec_general_end:

	.text
	.globl	general_exception
general_exception:
	mfc0	$k0, CP0_STATUS
	andi	$k0, $k0, ST_KSU_USER
	beqz	$k0, 1f
	move	$k1, $sp
	/* From user mode: switch to this thread's (empty) kernel stack. */
	lui	$k0, %hi(cur_kstack_top)
	lw	$sp, %lo(cur_kstack_top)($k0)
1:
	addiu	$sp, $sp, -TF_SIZE
	sw	$k1, TF_REG(29)($sp)
	sw	$1, TF_REG(1)($sp)
	sw	$2, TF_REG(2)($sp)
	sw	$3, TF_REG(3)($sp)
	sw	$4, TF_REG(4)($sp)
	sw	$5, TF_REG(5)($sp)
	sw	$6, TF_REG(6)($sp)
	sw	$7, TF_REG(7)($sp)
	sw	$8, TF_REG(8)($sp)
	sw	$9, TF_REG(9)($sp)
	sw	$10, TF_REG(10)($sp)
	sw	$11, TF_REG(11)($sp)
	sw	$12, TF_REG(12)($sp)
	sw	$13, TF_REG(13)($sp)
	sw	$14, TF_REG(14)($sp)
	sw	$15, TF_REG(15)($sp)
	sw	$16, TF_REG(16)($sp)
	sw	$17, TF_REG(17)($sp)
	sw	$18, TF_REG(18)($sp)
	sw	$19, TF_REG(19)($sp)
	sw	$20, TF_REG(20)($sp)
	sw	$21, TF_REG(21)($sp)
	sw	$22, TF_REG(22)($sp)
	sw	$23, TF_REG(23)($sp)
	sw	$24, TF_REG(24)($sp)
	sw	$25, TF_REG(25)($sp)
	sw	$28, TF_REG(28)($sp)
	sw	$30, TF_REG(30)($sp)
	sw	$31, TF_REG(31)($sp)
	mfhi	$t0
	mflo	$t1
	sw	$t0, TF_HI($sp)
	sw	$t1, TF_LO($sp)
	mfc0	$t0, CP0_EPC
	mfc0	$t1, CP0_STATUS
	mfc0	$t2, CP0_CAUSE
	mfc0	$t3, CP0_BADVADDR
	sw	$t0, TF_EPC($sp)
	sw	$t1, TF_STATUS($sp)
	sw	$t2, TF_CAUSE($sp)
	sw	$t3, TF_BADVADDR($sp)

	/* Run the kernel with EXL, KSU and IE clear: kernel mode, no IRQs. */
	li	$t2, ~(ST_EXL | ST_KSU_USER | ST_IE)
	and	$t1, $t1, $t2
	mtc0	$t1, CP0_STATUS
	nop
	nop
	nop

	jal	trap
	move	$a0, $sp

	.globl	trap_return
trap_return:
	/* sp points at the trap frame to resume. Restoring Status sets EXL
	 * (always set in a saved frame), which keeps us in kernel mode with
	 * interrupts held off until eret. */
	lw	$t0, TF_STATUS($sp)
	mtc0	$t0, CP0_STATUS
	nop
	nop
	nop
	lw	$t1, TF_EPC($sp)
	mtc0	$t1, CP0_EPC
	lw	$t0, TF_HI($sp)
	lw	$t1, TF_LO($sp)
	mthi	$t0
	mtlo	$t1
	lw	$1, TF_REG(1)($sp)
	lw	$2, TF_REG(2)($sp)
	lw	$3, TF_REG(3)($sp)
	lw	$4, TF_REG(4)($sp)
	lw	$5, TF_REG(5)($sp)
	lw	$6, TF_REG(6)($sp)
	lw	$7, TF_REG(7)($sp)
	lw	$8, TF_REG(8)($sp)
	lw	$9, TF_REG(9)($sp)
	lw	$10, TF_REG(10)($sp)
	lw	$11, TF_REG(11)($sp)
	lw	$12, TF_REG(12)($sp)
	lw	$13, TF_REG(13)($sp)
	lw	$14, TF_REG(14)($sp)
	lw	$15, TF_REG(15)($sp)
	lw	$16, TF_REG(16)($sp)
	lw	$17, TF_REG(17)($sp)
	lw	$18, TF_REG(18)($sp)
	lw	$19, TF_REG(19)($sp)
	lw	$20, TF_REG(20)($sp)
	lw	$21, TF_REG(21)($sp)
	lw	$22, TF_REG(22)($sp)
	lw	$23, TF_REG(23)($sp)
	lw	$24, TF_REG(24)($sp)
	lw	$25, TF_REG(25)($sp)
	lw	$28, TF_REG(28)($sp)
	lw	$30, TF_REG(30)($sp)
	lw	$31, TF_REG(31)($sp)
	lw	$sp, TF_REG(29)($sp)
	eret
	nop

/*
 * void swtch(struct context *old, struct context *new)
 * Saves callee-saved state into *old and resumes *new.
 */
	.globl	swtch
swtch:
	sw	$s0, 0($a0)
	sw	$s1, 4($a0)
	sw	$s2, 8($a0)
	sw	$s3, 12($a0)
	sw	$s4, 16($a0)
	sw	$s5, 20($a0)
	sw	$s6, 24($a0)
	sw	$s7, 28($a0)
	sw	$s8, 32($a0)
	sw	$sp, 36($a0)
	sw	$ra, 40($a0)
	lw	$s0, 0($a1)
	lw	$s1, 4($a1)
	lw	$s2, 8($a1)
	lw	$s3, 12($a1)
	lw	$s4, 16($a1)
	lw	$s5, 20($a1)
	lw	$s6, 24($a1)
	lw	$s7, 28($a1)
	lw	$s8, 32($a1)
	lw	$sp, 36($a1)
	lw	$ra, 40($a1)
	jr	$ra
	nop

/* Flush the whole TLB to invalid, unique entries (index in a0 ignored). */
	.globl	tlb_flush_all
tlb_flush_all:
	mfc0	$t3, CP0_ENTRYHI
	mtc0	$zero, CP0_ENTRYLO0
	mtc0	$zero, CP0_ENTRYLO1
	li	$t0, 0
	li	$t1, 32
	/* Unique VPN2s in KSEG0 never match a mapped access. */
	lui	$t2, 0x8000
1:	mtc0	$t2, CP0_ENTRYHI
	mtc0	$t0, CP0_INDEX
	nop
	nop
	nop
	tlbwi
	nop
	addiu	$t0, $t0, 1
	addiu	$t2, $t2, 0x2000
	bne	$t0, $t1, 1b
	nop
	mtc0	$t3, CP0_ENTRYHI
	nop
	jr	$ra
	nop
