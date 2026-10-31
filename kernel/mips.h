/* MIPS / VR4131 CP0 definitions (VR4131 UM chapter 6, R4000-compatible). */
#ifndef GLAZE_MIPS_H
#define GLAZE_MIPS_H

#define CP0_INDEX       $0
#define CP0_RANDOM      $1
#define CP0_ENTRYLO0    $2
#define CP0_ENTRYLO1    $3
#define CP0_CONTEXT     $4
#define CP0_PAGEMASK    $5
#define CP0_WIRED       $6
#define CP0_BADVADDR    $8
#define CP0_COUNT       $9
#define CP0_ENTRYHI     $10
#define CP0_COMPARE     $11
#define CP0_STATUS      $12
#define CP0_CAUSE       $13
#define CP0_EPC         $14
#define CP0_PRID        $15
#define CP0_CONFIG      $16

#define ST_IE           0x00000001
#define ST_EXL          0x00000002
#define ST_ERL          0x00000004
#define ST_KSU_USER     0x00000010
#define ST_IM2          0x00000400      /* VRIP (ICU) */
#define ST_IM7          0x00008000      /* CP0 Compare */
#define ST_BEV          0x00400000
#define ST_CU0          0x10000000

#define CAUSE_EXC(c)    (((c) >> 2) & 0x1F)
#define CAUSE_IP2       0x00000400
#define CAUSE_IP7       0x00008000
#define CAUSE_BD        0x80000000

#define EXC_INT         0
#define EXC_MOD         1
#define EXC_TLBL        2
#define EXC_TLBS        3
#define EXC_ADEL        4
#define EXC_ADES        5
#define EXC_SYS         8
#define EXC_BP          9
#define EXC_RI          10
#define EXC_CPU         11
#define EXC_OV          12

/*
 * VR41xx TLB encoding (VR4131 UM §6.2; GXemul V2P_MMU4100): the PFN field is
 * PA[31:10] at EntryLo bit 6, so EntryLo = PA >> 4 for 1 KiB-aligned PAs.
 * A PageMask of 0x1800 selects 4 KiB pages.
 */
#define PAGEMASK_4K     0x00001800
#define PTE_G           0x00000001
#define PTE_V           0x00000002
#define PTE_D           0x00000004
#define PTE_UNCACHED    (2u << 3)
#define PTE_CACHED      (3u << 3)
#define PTE_PA(pte)     (((pte) & ~0x3Fu) << 4)
#define PTE_MAKE(pa)    ((uint32_t)(pa) >> 4)

#ifndef __ASSEMBLER__
#include "types.h"

#define mfc0(reg) ({ uint32_t __v; \
    __asm__ volatile("mfc0 %0, " #reg : "=r"(__v)); __v; })
#define mtc0(reg, v) __asm__ volatile("mtc0 %0, " #reg "\n\tnop\n\tnop\n\tnop" :: "r"((uint32_t)(v)))

#define read_c0_status()        mfc0($12)
#define write_c0_status(v)      mtc0($12, v)
#define read_c0_cause()         mfc0($13)
#define read_c0_count()         mfc0($9)
#define write_c0_compare(v)     mtc0($11, v)
#define read_c0_badvaddr()      mfc0($8)
#define read_c0_prid()          mfc0($15)
#define read_c0_config()        mfc0($16)
#define write_c0_config(v)      mtc0($16, v)
#define read_c0_entryhi()       mfc0($10)
#define write_c0_entryhi(v)     mtc0($10, v)

static inline uint32_t irq_save(void)
{
    uint32_t s = read_c0_status();
    write_c0_status(s & ~ST_IE);
    return s;
}

static inline void irq_restore(uint32_t s)
{
    write_c0_status(s);
}

/* VR4131 STANDBY (UM 12.1, 12.3): the pipeline clock stops until any
 * interrupt; the timer, ICU and companion keep running. It sets Status.IE
 * itself (Linux's vr41xx_cpu_wait, arch/mips/vr41xx/common/pmu.c, relies on
 * that), but sched.c's idle loop doesn't, for emulators. The NOPs fill
 * the pipeline after it, as the UM's sequence and Linux's have them. GAS
 * takes the mnemonic only with -march=vr41xx, so it is spelled as a word. */
static inline void vr41xx_standby(void)
{
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"
                         ".word 0x42000021\n\t"    /* standby: COP0, function 0x21 */
                         "nop\n\tnop\n\tnop\n\tnop\n\t"
                         ".set pop" ::: "memory");
}
#endif

#endif
