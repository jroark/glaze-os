/*
 * Power off: SYS_POWER_SLEEP (docs/POWER.md "Power off"), as Windows CE
 * powers off a BE-300 (nk.exe 0x800792AC-0x80079598, PowerOn.dll's "Bye..."
 * being SpringBoard's Goodbye screen).
 *
 * The LCD goes off and the CPU sleeps until the power switch, or the RTC
 * alarm SpringBoard sets for its next notification. Either way the
 * devices that can't run through it are stopped first and started again
 * after, Casio's power-management microcontroller is told to stop its
 * watchdog (cmcu.c), and the monotonic clock is carried across on the RTC
 * (timer.c).
 *
 *   Suspend    VR4131 SUSPEND: RAM and CPU state stay, and an interrupt
 *              wakes it. Only the power switch's own interrupt, GIRQ0's
 *              button source and the ETIME compare are left unmasked; a
 *              wake by any other button suspends again, as CE's second
 *              check at 0x800795D8 does.
 *
 *   Hibernate  VR4131 HIBERNATE: only the RTC runs, and the wake is a
 *              reset. The kernel stays in the self-refreshed SDRAM with a
 *              resume record (kernel/resume.h) that the SPL checks before
 *              it would load a kernel; it hibernates again itself when
 *              neither Power nor the alarm woke it. Every peripheral register
 *              but the PMU's and some of the RTC, BCU and GIU's is undefined
 *              after (UM 12.1.4(4)), so the devices are set up again. A RAM
 *              boot from Windows CE suspends instead, since CE's SPL would
 *              start CE from the NAND.
 *
 * The kernel runs with interrupts off; the sleeps take the wake interrupt
 * through trap(), which only notes it (power_trap) and returns here.
 */

#include "../kernel.h"
#include "../hw.h"
#include "../mips.h"
#include "../trapframe.h"
#include "../drivers.h"
#include "../resume.h"

int power_save(uint32_t *ctx);
void power_resume_entry(void);

static volatile bool sleeping;
extern const char power_sleep_begin[], power_sleep_end[];

/* VR4131 SUSPEND (UM 12.1.4(3)): every clock but the RTC's, ICU's and
 * PMU's stops and the SDRAM self-refreshes until an interrupt request; the
 * PLL keeps running, so the wake is immediate. Not during DMA. It comes
 * with interrupts turned on, as one block: an interrupt taken anywhere in
 * it returns past the SUSPEND (power_trap), which otherwise, with
 * interrupts off again, would wait for good. The NOPs fill the pipeline
 * after it, as for STANDBY (mips.h). */
static void __attribute__((noinline)) suspend_once(uint32_t status)
{
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"
                         ".globl power_sleep_begin\npower_sleep_begin:\n\t"
                         "mtc0 %0, $12\n\tnop\n\tnop\n\tnop\n\t"
                         ".word 0x42000022\n\t"    /* suspend: COP0, function 0x22 */
                         "nop\n\tnop\n\tnop\n\tnop\n\t"
                         ".globl power_sleep_end\npower_sleep_end:\n\t"
                         ".set pop" :: "r"(status) : "memory");
}

bool power_trap(void *frame)
{
    struct trapframe *tf = frame;

    if (!sleeping)
        return false;
    if (tf->epc >= (uint32_t)(uintptr_t)power_sleep_begin && tf->epc < (uint32_t)(uintptr_t)power_sleep_end)
        tf->epc = (uint32_t)(uintptr_t)power_sleep_end;
    tf->status &= ~ST_IE;       /* back in suspend_until() with interrupts off */
    return true;
}

static void alarm_set(uint64_t at)
{
    RTC_ECMP_L = (uint16_t)at;
    RTC_ECMP_M = (uint16_t)(at >> 16);
    RTC_ECMP_H = (uint16_t)(at >> 32);
    RTC_INT = RTCINT_ETIME;
}

/* Power held: the button word as input_poll() reads it. */
static bool power_held(void)
{
    (void)KEYS_LATCH;
    return (KEYS_SET2 & KEY2_POWER) != 0;
}

static int wake_reason(uint64_t alarm)
{
    if (power_held() || (ICU_SYSINT1 & SYSINT1_POWER))
        return GZ_WAKE_POWER;
    if (alarm && timer_etime() >= alarm)
        return GZ_WAKE_ALARM;
    return 0;
}

/* Acknowledge every wake source, so the next sleep waits for a new one. */
static void wake_ack(void)
{
    PMU_INT = PMUINT_POWERSWINTR;
    RTC_INT = RTCINT_ETIME;
    KJ_SRC_KEYS = KJ_SRC_MODE | KJ_SRC_ENABLE;    /* a write acknowledges it */
    GIU_INTSTATL = 0x0001u;
}

/* GIRQ0 sources with a Glaze driver (irq.c): PCMCIA, buttons, USB, audio
 * and touch. Bits 4-7 read back as status and aren't written back. */
static const uint8_t girq0_sources[] = { 0, 1, 7, 8, 9 };

static int suspend_until(uint64_t alarm)
{
    uint32_t status = read_c0_status();
    uint16_t msysint1 = ICU_MSYSINT1, msysint2 = ICU_MSYSINT2;
    uint16_t mgiul = ICU_MGIUINTL, mgiuh = ICU_MGIUINTH;
    uint32_t src[sizeof(girq0_sources)];
    int wake;

    ICU_MSYSINT1 = ICU_MSYSINT2 = ICU_MGIUINTL = ICU_MGIUINTH = 0;
    for (unsigned i = 0; i < sizeof(girq0_sources); i++) {
        src[i] = KJ_GIRQ0_EN(girq0_sources[i]) & 0x0Fu;
        KJ_GIRQ0_EN(girq0_sources[i]) = src[i] & ~KJ_SRC_ENABLE;
    }
    if (alarm)
        alarm_set(alarm);
    wake_ack();
    ICU_MGIUINTL = 0x0001u;
    ICU_MSYSINT1 = SYSINT1_POWER | SYSINT1_GIU | (alarm ? SYSINT1_ETIME : 0);
    for (;;) {
        sleeping = true;
        suspend_once((status & ~0xFF00u) | ST_IM2 | ST_IE);
        write_c0_status(status & ~ST_IE);
        sleeping = false;
        if ((wake = wake_reason(alarm)))
            break;
        wake_ack();
    }
    ICU_MSYSINT1 = 0;
    PMU_INT = PMUINT_POWERSWINTR;
    RTC_INT = RTCINT_ETIME;
    for (unsigned i = 0; i < sizeof(girq0_sources); i++)
        KJ_GIRQ0_EN(girq0_sources[i]) = src[i];
    GIU_INTSTATL = 0x0001u;
    ICU_MGIUINTL = mgiul;
    ICU_MGIUINTH = mgiuh;
    ICU_MSYSINT2 = msysint2;
    ICU_MSYSINT1 = msysint1;
    write_c0_status(status);
    return wake;
}

/* Index_Writeback_Invalidate_D over both ways, as start.S's flush: the
 * SDRAM must hold everything when the caches lose it. */
static void dcache_writeback_all(void)
{
    for (uintptr_t a = 0x80000000u; a < 0x80008000u; a += 16)
        __asm__ volatile(".set push\n.set mips3\ncache 0x01, 0(%0)\n.set pop" :: "r"(a) : "memory");
    __asm__ volatile("sync" ::: "memory");
}

static struct gz_resume *const record = (struct gz_resume *)KSEG0(GZ_RESUME_PA);

#ifdef GLAZE_HIBERNATE_VERIFY
/* HIBERNATE_VERIFY=1: which pages the boot ROM and SPL overwrite on the way
 * back, so mm.c can keep them out of use. Every page in use is summed just
 * before the HIBERNATE and again after; the sums themselves, the record
 * and this thread's stack change in between. */
#define NFRAMES (SDRAM_SIZE / PAGE_SIZE)
static uint32_t frame_sums[NFRAMES];

static bool verify_skip(uint32_t frame, uint32_t sp)
{
    uint32_t sums = PHYS(frame_sums) / PAGE_SIZE;
    if (frame >= 0xF00000u / PAGE_SIZE && frame < 0xF10000u / PAGE_SIZE)
        return true;        /* the SPL, which mm.c keeps free for it */
    if (frame >= 0xFE0000u / PAGE_SIZE && frame < 0xFF0000u / PAGE_SIZE)
        return true;        /* and its stack */
    return frame == GZ_RESUME_PA / PAGE_SIZE || frame == PHYS(sp) / PAGE_SIZE ||
           frame + 1 == PHYS(sp) / PAGE_SIZE ||
           (frame >= sums && frame <= PHYS(frame_sums + NFRAMES - 1) / PAGE_SIZE);
}

static uint32_t frame_sum(uint32_t frame)
{
    return gz_resume_sum((const uint32_t *)KSEG0(frame * PAGE_SIZE), PAGE_SIZE);
}

static void verify_before(void)
{
    uint32_t sp = (uint32_t)(uintptr_t)&sp;
    for (uint32_t f = 0; f < NFRAMES; f++)
        frame_sums[f] = mm_page_used(f) && !verify_skip(f, sp) ? frame_sum(f) : 0;
}

static void verify_after(void)
{
    uint32_t sp = (uint32_t)(uintptr_t)&sp, bad = 0;
    for (uint32_t f = 0; f < NFRAMES; f++) {
        if (!mm_page_used(f) || verify_skip(f, sp) || frame_sum(f) == frame_sums[f])
            continue;
        if (bad++ < 32) {
            const uint32_t *w = (const uint32_t *)KSEG0(f * PAGE_SIZE);
            kprintf("power: verify: page 0x%08x changed across the HIBERNATE (%08x %08x ...)\n",
                    f * PAGE_SIZE, w[0], w[1]);
        }
    }
    kprintf("power: verify: %u pages changed\n", bad);
}
#endif

/* RAM the boot ROM works in, copied aside for the SPL to put back
 * (kernel/resume.h). Last before HIBERNATE, so little changes after. */
extern uint8_t __kernel_end[];
extern const uint8_t __text_start[], __ro_end[];

static bool save_page(uint32_t pa)
{
    void *copy = page_alloc_between(GZ_RESUME_LOW_PAGES * PAGE_SIZE, SDRAM_SIZE);

    if (!copy || record->nsaved == GZ_RESUME_SAVES)
        return false;
    memcpy(copy, (void *)KSEG0(pa), PAGE_SIZE);
    record->saved[record->nsaved].pa = pa;
    record->saved[record->nsaved++].copy = PHYS(copy);
    return true;
}

static void free_low(void)
{
    for (uint32_t i = 0; i < record->nsaved; i++)
        page_free((void *)KSEG0(record->saved[i].copy));
    record->nsaved = 0;
}

static bool save_low(void)
{
    record->nsaved = 0;
    for (uint32_t i = 0; i < GZ_RESUME_LOW_PAGES; i++)
        if (i != GZ_RESUME_PA / PAGE_SIZE && !save_page(i * PAGE_SIZE))
            return false;
    for (uint32_t pa = GZ_RESUME_PROBE_LOW; pa < PHYS(__kernel_end); pa <<= 1)
        if (!save_page(pa))
            return false;
    return true;
}

/* GZ_WAKE_* once the SPL has resumed this, or 0 if HIBERNATE returned
 * with everything as it was. */
static int hibernate_until(uint64_t alarm)
{
    uint16_t msysint1 = ICU_MSYSINT1, msysint2 = ICU_MSYSINT2;
    uint16_t mgiul = ICU_MGIUINTL, mgiuh = ICU_MGIUINTH;
    uint32_t src[sizeof(girq0_sources)];

    for (unsigned i = 0; i < sizeof(girq0_sources); i++)
        src[i] = KJ_GIRQ0_EN(girq0_sources[i]) & 0x0Fu;
    if (power_save(record->ctx)) {
        /* Back from the SPL: 4 KiB pages, nothing wired, no stale entries
         * (trap_init), and the HALTimer told within its 4 s (main.c). */
        PMU_CNT = PMU_CNT | PMUCNT_HALTIMERRST;
        mtc0($5, PAGEMASK_4K);
        mtc0($6, 0);
        tlb_flush_all();
#ifdef GLAZE_HIBERNATE_VERIFY
        verify_after();
#endif
        free_low();
        kprintf("power: resumed by the SPL (PMUINT 0x%x, %u wakes slept through)\n",
                record->pmuint, record->rests);
        return (int)record->wake;
    }
    record->entry = (uint32_t)(uintptr_t)power_resume_entry;
    record->alarm_hi = (uint32_t)(alarm >> 32);
    record->alarm_lo = (uint32_t)alarm;
    record->wake = 0;
    record->rests = 0;
    record->pmuint = 0;

    /* What CE masks and acknowledges (0x80079ADC), and the alarm, the only
     * wake source besides the switch, programmed. */
    ICU_MSYSINT1 = ICU_MSYSINT2 = ICU_MGIUINTL = ICU_MGIUINTH = 0;
    for (unsigned i = 0; i < sizeof(girq0_sources); i++)
        KJ_GIRQ0_EN(girq0_sources[i]) = KJ_SRC_MODE;
    GIU_INTSTATL = 0xFFFFu;
    GIU_INTSTATH = 0xFFFFu;
    if (alarm)
        alarm_set(alarm);
    RTC_INT = RTCINT_ETIME;
    if (!save_low()) {
        free_low();
        kputs("power: no memory to save low RAM in; suspending\n");
        goto not_asleep;
    }
#ifdef GLAZE_HIBERNATE_VERIFY
    verify_before();
#endif
    /* What the SPL checks once it has put low RAM back. */
    record->text_va = (uint32_t)(uintptr_t)__text_start;
    record->text_len = (uint32_t)(__ro_end - __text_start) & ~3u;
    record->text_sum = gz_resume_sum((const uint32_t *)__text_start, record->text_len);
    record->magic = GZ_RESUME_MAGIC;
    dcache_writeback_all();
    PMU_INT = PMUINT_CLEAR_ALL;
    vr41xx_hibernate();
    /* An interrupt was pending (UM 12.1.2(2)): not asleep after all. */
    record->magic = 0;
    free_low();
    kputs("power: an interrupt kept the CPU from hibernating; suspending\n");
not_asleep:
    for (unsigned i = 0; i < sizeof(girq0_sources); i++)
        KJ_GIRQ0_EN(girq0_sources[i]) = src[i];
    ICU_MGIUINTL = mgiul;
    ICU_MGIUINTH = mgiuh;
    ICU_MSYSINT2 = msysint2;
    ICU_MSYSINT1 = msysint1;
    return 0;
}

void power_boot(void)
{
    if (ram_boot)
        return;                 /* GlazeBoot owns low RAM; no hibernation */
    if (record->magic == GZ_RESUME_MAGIC)
        kputs("power: a hibernation was not resumed\n");
    record->magic = 0;
}

static const char *const mode_names[] = { "", "suspend", "hibernate" };

int power_sleep(int mode, uint32_t alarm_s)
{
    if (mode != GZ_SLEEP_SUSPEND && mode != GZ_SLEEP_HIBERNATE)
        return GZ_EINVAL;
    if (mode == GZ_SLEEP_HIBERNATE && ram_boot)
        mode = GZ_SLEEP_SUSPEND;
    uint64_t alarm_ticks = (uint64_t)alarm_s * RTC_HZ;
    bool scripted = false;
#ifdef GLAZE_INPUT_SCRIPT
    /* The script's clock stops too: wake for its next event, as a press. */
    int next_ms = input_script_next_ms();
    uint64_t next_ticks = (uint64_t)next_ms * RTC_HZ / 1000u + 1;
    if (next_ms >= 0 && (!alarm_ticks || next_ticks < alarm_ticks)) {
        alarm_ticks = next_ticks;
        scripted = true;
    }
#endif
    kprintf("power: %s%s\n", mode_names[mode], alarm_s ? ", with an alarm" : "");

    audio_suspend();
    buzzer_stop();
    battery_suspend();
    input_suspend();
    display_sleep();
    bool watchdog = cmcu_stop();
    uint64_t asleep = timer_suspend();
    uint64_t alarm = alarm_ticks ? asleep + alarm_ticks : 0;

    int wake = 0;
    bool cold = false;
    if (mode == GZ_SLEEP_HIBERNATE) {
        wake = hibernate_until(alarm);
        cold = wake != 0;
        if (!cold)
            mode = GZ_SLEEP_SUSPEND;
    }
    if (!wake)
        wake = suspend_until(alarm);
    if (wake == GZ_WAKE_ALARM && scripted)
        wake = GZ_WAKE_POWER;

    if (cold) {
        companion_gates_on();
        irq_init();
    }
    timer_resume();
    if (cold) {
        cmcu_init();
        usb_resume();
    }
    cmcu_start();
    display_wake();
    input_resume(cold);
    battery_resume();
    audio_resume(cold);
    if (cold)
        pcmcia_resume();
    kprintf("power: awake (%s) after %u s%s\n", wake == GZ_WAKE_ALARM ? "alarm" : "power",
            (uint32_t)(((timer_etime() - asleep) & 0xFFFFFFFFFFFFull) / RTC_HZ),
            watchdog ? "" : "; the watchdog didn't answer its stop");
    return wake | mode << 8;
}
