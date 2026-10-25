/*
 * The trampoline: the code that runs after Windows CE. GlazeBoot (a Windows
 * CE program, GlazeBoot.c) carries it as bytes, tramp_blob.h, made from this
 * file by the GCC build (tools/mktrampblob.py). It copies them into a page
 * whose frame is no destination and calls tramp_entry, at offset 0, through
 * KSEG0 with a0 = the control page (plan.h), also through KSEG0.
 *
 * From there nothing of Windows CE may be used: no TLB-mapped address, no
 * call to an absolute address, no exception. The code is position
 * independent (branches only; the build checks tramp.o has no absolute
 * relocations), runs on a stack at the top of the control page, and:
 *
 *   1. turns interrupts off and leaves exception level (Status = 0, as the
 *      SPL does), and masks the VR4131 ICU;
 *   2. stops the wave DMA (Casio's ping-pong block, kernel/hw.h AUD_*;
 *      wavedev's stop sequence as in kernel/drivers/audio.c) and waits for
 *      a NAND transfer Windows CE may have started to finish;
 *   3. writes back and invalidates both caches, so RAM holds everything
 *      the loader wrote through Windows CE's mappings;
 *   4. puts every page in place (gb_permute);
 *   5. writes back and invalidates the caches again and jumps to the
 *      kernel entry with a0 = the boot info (glaze/bootinfo.h).
 *
 * Each step paints its block of the boot trace (gz_boot_trace in
 * glaze/bootinfo.h), since nothing else can tell where a hang happened.
 */

#include "plan.h"
#include <glaze/bootinfo.h>

#define REG16(pa)   (*(volatile uint16_t *)(0xA0000000u | (pa)))
#define REG32(pa)   (*(volatile uint32_t *)(0xA0000000u | (pa)))

/* VR4131 UM ch. 5 (ICU): the interrupt mask registers. */
#define ICU_MSYSINT1    REG16(0x0F00008Cu)
#define ICU_MGIUINTL    REG16(0x0F000094u)
#define ICU_MSYSINT2    REG16(0x0F0000A6u)
#define ICU_MGIUINTH    REG16(0x0F0000A8u)

/* kernel/hw.h. */
#define AUD_CTRL        REG32(0x0A000880u)
#define AUD_START       REG32(0x0A000890u)
#define AUD_DMA_RUN     REG32(0x0A000898u)
#define AUD_ARM         REG32(0x0A0008A0u)
#define AUD_ACK0        REG32(0x0A0008C8u)
#define AUD_ACK1        REG32(0x0A0008CCu)

void tramp_main(struct gb_control *c) __attribute__((section(".tramp"), noreturn, used));

__asm__(
    "   .section .tramp.entry, \"ax\"\n"
    "   .set    push\n"
    "   .set    noreorder\n"
    "   .set    mips3\n"
    "   .globl  tramp_entry\n"
    "tramp_entry:\n"
    "   mtc0    $zero, $12\n"           /* Status: IE, EXL, ERL off; kernel mode */
    "   nop\n"
    "   nop\n"
    "   nop\n"
    "   addiu   $sp, $a0, 4096 - 32\n"  /* stack at the top of the control page */
    "   bal     tramp_main\n"
    "   nop\n"
    "1: b       1b\n"
    "   nop\n"
    "   .set    pop\n"
    "   .previous\n");

/*
 * For GlazeBoot's probe mode: CP0 register a0 (0 Status, 1 PRId, 2 Config)
 * in v0, a leaf in the same blob, so the loader (a Windows CE program with no
 * MIPS assembly) can check it may run code through KSEG0 and read CP0
 * before relying on both. VR4131 UM ch. 6: a word of hazard after MFC0.
 */
__asm__(
    "   .section .tramp, \"ax\"\n"
    "   .set    push\n"
    "   .set    noreorder\n"
    "   .set    mips3\n"
    "   .globl  tramp_read_c0\n"
    "tramp_read_c0:\n"
    "   mfc0    $v0, $12\n"
    "   nop\n"
    "   li      $t0, 1\n"
    "   bne     $a0, $t0, 1f\n"
    "   nop\n"
    "   mfc0    $v0, $15\n"
    "   nop\n"
    "1: li      $t0, 2\n"
    "   bne     $a0, $t0, 2f\n"
    "   nop\n"
    "   mfc0    $v0, $16\n"
    "   nop\n"
    "2: jr      $ra\n"
    "   nop\n"
    "   .set    pop\n"
    "   .previous\n");

/* VR4131 UM 1.6.3 / ch. 23 (CACHE): Index_Writeback_Invalidate_D (0x01) and
 * Index_Invalidate_I (0x00) over both ways of the 16 KiB caches (and more,
 * which is harmless), at the 16-byte D line size. */
static inline __attribute__((always_inline)) void caches_flush(void)
{
    for (uint32_t a = 0x80000000u; a < 0x80008000u; a += 16)
        __asm__ volatile(".set push\n.set mips3\ncache 0x01, 0(%0)\ncache 0x00, 0(%0)\n.set pop"
                         :: "r"(a) : "memory");
    __asm__ volatile("sync" ::: "memory");
}

void tramp_main(struct gb_control *c)
{
    gz_boot_trace(1, GZ_TRACE_RED);
    ICU_MSYSINT1 = 0;
    ICU_MSYSINT2 = 0;
    ICU_MGIUINTL = 0;
    ICU_MGIUINTH = 0;

    AUD_ARM = 0;
    AUD_CTRL &= ~0x08u;
    AUD_DMA_RUN &= ~1u;
    AUD_START &= ~2u;
    AUD_ACK0 = 0x11;
    AUD_ACK1 = 0x11;

    /* Let any DMA in flight (NAND, audio) finish: a few ms at any clock. */
    for (volatile uint32_t i = 0; i < 2000000u; i++)
        ;
    gz_boot_trace(2, GZ_TRACE_YELLOW);

    caches_flush();
    gz_boot_trace(3, GZ_TRACE_GREEN);
    gb_permute(c);
    gz_boot_trace(4, GZ_TRACE_CYAN);
    caches_flush();
    gz_boot_trace(5, GZ_TRACE_BLUE);

    void (*entry)(uint32_t) = (void (*)(uint32_t))c->entry;
    entry(c->arg);
    for (;;)
        ;
}
