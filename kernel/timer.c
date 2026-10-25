/*
 * Scheduler tick on CP0 Count/Compare (IP7) and the monotonic clock.
 *
 * The Count rate is calibrated at boot against the VR4131 RTC elapsed-time
 * counter, which ticks at 32.768 kHz (VR4131 UM RTC chapter; doc: RTC and
 * Timers).
 */

#include "kernel.h"
#include "hw.h"
#include "mips.h"
#include "proc.h"
#include "drivers.h"

#define TICK_HZ         100u

static uint32_t count_per_ms;
static uint32_t count_per_tick;
static uint32_t next_compare;
static uint32_t last_count;
static uint64_t count_total;

static uint64_t clock_update(void)
{
    uint32_t now = read_c0_count();

    count_total += (uint32_t)(now - last_count);
    last_count = now;
    return count_total;
}

uint64_t time_us(void)
{
    return clock_update() * 1000u / count_per_ms;
}

/* Count's rate in the emulator, if the RTC can't measure it. */
#define GUESS_COUNT_PER_MS  10372u
/* RTC reads before giving up on it ticking: a tick is ~30 us, a read well
 * under 1 us, so this is seconds. */
#define RTC_POLLS           5000000u

static bool rtc_ok;

bool timer_rtc_ok(void)
{
    return rtc_ok;
}

static void calibrate(void)
{
    /* Measure Count across 100 ms of RTC elapsed time (3277 ticks). */
    const uint16_t span = RTC_HZ / 10;
    uint16_t start = RTC_ETIME_L;
    uint32_t n = 0;

    count_per_ms = GUESS_COUNT_PER_MS;
    while (RTC_ETIME_L == start)
        if (++n == RTC_POLLS) {
            kputs("timer: the RTC isn't counting; guessing Count's rate\n");
            return;
        }
    start = RTC_ETIME_L;
    uint32_t c0 = read_c0_count();
    while ((uint16_t)(RTC_ETIME_L - start) < span)
        ;
    uint32_t c1 = read_c0_count();

    count_per_ms = (c1 - c0) / 100u;
    if (count_per_ms == 0)
        count_per_ms = 1;
    rtc_ok = true;
}

void timer_init(void)
{
    calibrate();
    count_per_tick = count_per_ms * (1000u / TICK_HZ);
    kprintf("timer: Count runs at %u kHz, tick every %u counts\n",
            count_per_ms, count_per_tick);

    last_count = read_c0_count();
    next_compare = last_count + count_per_tick;
    write_c0_compare(next_compare);
}

void timer_irq(void)
{
    uint32_t now = read_c0_count();

    next_compare += count_per_tick;
    if ((int32_t)(next_compare - now) <= 0)
        next_compare = now + count_per_tick;
    write_c0_compare(next_compare);     /* also acknowledges IP7 */
    cmcu_tick();
    sched_timer_tick(time_us());
    input_poll();
    netdev_irq();
    audio_irq();
}
