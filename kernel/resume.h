/*
 * The Hibernate resume record (docs/POWER.md "Power off"), shared by the
 * kernel (kernel/drivers/power.c, kernel/power_resume.S) and the SPL
 * (boot/spl.c). HIBERNATE wakes through the reset vector: the boot ROM
 * loads the SPL again, and the SPL resumes the kernel still in RAM instead
 * of loading it when this record is valid and the kernel's text and
 * read-only data still sum to KERNEL_TEXT_SUM, what tools/elf_consts.py
 * found in the kernel the SPL was built with.
 *
 * It sits in the page below GZ_BOOTINFO_PA (glaze/bootinfo.h), which the
 * kernel never allocates, clear of the reset stub at PA 0x1000 and of
 * Windows CE's own save area at 0x2000-0x3800.
 *
 * The boot ROM works in RAM on its way to the SPL. In the emulator, running
 * the stock ROM, a wake changed PA 0x0000-0x0FFF, 0x10000-0x18FFF and
 * 0x20000-0x20FFF, the vectors and the kernel's own text, and the first
 * word of the page at each power of two from 0x40000 (a RAM sizing probe
 * writing its address there). Windows CE's kernel lives above most of that
 * (0x80060000); Glaze's starts at 0x80010000. So the kernel copies the
 * first GZ_RESUME_LOW_PAGES pages (but this one) and every probed page
 * below its end to spare pages listed in saved[], and the SPL copies them
 * back before it checks the text and resumes; mm.c keeps the probed pages
 * above the kernel out of use.
 */
#ifndef GLAZE_RESUME_H
#define GLAZE_RESUME_H

#define GZ_RESUME_PA        0xE000u
#define GZ_RESUME_MAGIC     0x52534D47u     /* "GMSR" */

/* Word offsets in ctx[], for power_resume.S. */
#define GZ_CTX_S0           0       /* s0-s7 */
#define GZ_CTX_FP           8
#define GZ_CTX_SP           9
#define GZ_CTX_RA           10
#define GZ_CTX_GP           11
#define GZ_CTX_STATUS       12
#define GZ_CTX_ENTRYHI      13
#define GZ_CTX_WORDS        16
#define GZ_RESUME_CTX_OFF   48      /* offsetof(struct gz_resume, ctx) */
#define GZ_RESUME_LOW_PAGES  0x30   /* PA 0-0x2FFFF, saved across the boot ROM */
#define GZ_RESUME_PROBE_LOW  0x40000u   /* the ROM's probes: powers of two from here */
#define GZ_RESUME_SAVES      (GZ_RESUME_LOW_PAGES + 8)

#ifndef __ASSEMBLER__
#include <stdint.h>

struct gz_resume {
    uint32_t magic;             /* GZ_RESUME_MAGIC while hibernating */
    uint32_t entry;             /* power_resume_entry, KSEG0 */
    uint32_t alarm_hi, alarm_lo;    /* ETIME of the alarm, 0 = none */
    uint32_t wake;              /* GZ_WAKE_*, set by the SPL */
    uint32_t rests;             /* wakes the SPL slept through again */
    uint32_t pmuint;            /* PMUINT as the SPL found it */
    uint32_t pad[5];
    uint32_t ctx[GZ_CTX_WORDS]; /* power_save() */
    uint32_t nsaved;
    struct { uint32_t pa, copy; } saved[GZ_RESUME_SAVES];  /* page, its copy's PA */
};

_Static_assert(__builtin_offsetof(struct gz_resume, ctx) == GZ_RESUME_CTX_OFF, "ctx offset");

/* tools/elf_consts.py computes the same sum. */
static inline uint32_t gz_resume_sum(const volatile uint32_t *w, uint32_t bytes)
{
    uint32_t sum = 0;

    for (uint32_t i = 0; i < bytes / 4; i++)
        sum = ((sum << 1) | (sum >> 31)) + w[i];
    return sum;
}
#endif

#endif
