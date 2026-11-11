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
static uint64_t us_total;       /* the monotonic clock, in microseconds */
static uint32_t us_rem;         /* Count * 1000 short of the next one, < count_per_ms */

/*
 * Every scheduling decision and tick reads this, so it avoids 64-bit
 * division (a subtract-and-shift loop on this CPU): the Count that passed
 * since the last read is converted on its own, with one DIVU, and the
 * remainder is carried, which gives exactly total Count * 1000 /
 * count_per_ms. Only a gap of more than 4,000,000 counts (about 0.4 s,
 * which the 100 Hz tick never leaves) needs the 64-bit form.
 */
uint64_t time_us(void)
{
    uint32_t now = read_c0_count();
    uint32_t delta = now - last_count;

    last_count = now;
    if (delta < 4000000u) {
        uint32_t n = delta * 1000u + us_rem;
        us_total += n / count_per_ms;
        us_rem = n % count_per_ms;
    } else {
        uint64_t n = (uint64_t)delta * 1000u + us_rem;
        us_total += n / count_per_ms;
        us_rem = (uint32_t)(n % count_per_ms);
    }
    return us_total;
}

/* Count's rate if the RTC can't measure it: a real BE-300 measures 10406
 * kHz, the emulator 10372. */
#define GUESS_COUNT_PER_MS  10406u
/* A plausible Count rate (BE-300 Count runs at PClock/16): anything else
 * is a bad measurement. */
#define MIN_COUNT_PER_MS    2000u
#define MAX_COUNT_PER_MS    100000u
/* RTC reads before giving up on it ticking: a tick is ~30 us, a read well
 * under 1 us, so this is seconds. */
#define RTC_POLLS           5000000u

static bool rtc_ok;

bool timer_rtc_ok(void)
{
    return rtc_ok;
}

/* The RTC's elapsed-time counter counts on its own clock: a read can catch
 * it changing, so take a value two reads agree on. */
static bool rtc_ticks(uint16_t *out)
{
    for (uint32_t n = 0; n < 32; n++) {
        uint16_t a = RTC_ETIME_L;
        uint16_t b = RTC_ETIME_L;
        if (a == b) {
            *out = a;
            return true;
        }
    }
    return false;
}

/* Every wait is bounded, including a counter that stops after its first
 * tick or cannot produce a stable read. Count calibration precedes time_us. */
static uint32_t measure(void)
{
    const uint16_t span = RTC_HZ / 10;
    uint16_t start, now;
    uint32_t n = 0;

    if (!rtc_ticks(&start))
        return 0;
    do {
        if (++n == RTC_POLLS || !rtc_ticks(&now))
            return 0;
    } while (now == start);
    start = now;
    uint32_t c0 = read_c0_count();
    n = 0;
    do {
        if (++n == RTC_POLLS || !rtc_ticks(&now))
            return 0;
    } while ((uint16_t)(now - start) < span);
    return (read_c0_count() - c0) / 100u;
}

/* On a real BE-300 one boot in a few measured 187 kHz instead of 10406
 * (the RTC freshly reset, or a torn read), which ran every timeout ~55
 * times fast: the median of three, checked for plausibility. */
static void calibrate(void)
{
    uint32_t m[3];

    count_per_ms = GUESS_COUNT_PER_MS;
    for (int i = 0; i < 3; i++) {
        m[i] = measure();
        if (!m[i]) {
            kputs("timer: the RTC isn't counting; guessing Count's rate\n");
            return;
        }
    }
    for (int i = 0; i < 2; i++)
        for (int k = i + 1; k < 3; k++)
            if (m[k] < m[i]) {
                uint32_t t = m[i];
                m[i] = m[k];
                m[k] = t;
            }
    if (m[1] < MIN_COUNT_PER_MS || m[1] > MAX_COUNT_PER_MS) {
        kprintf("timer: implausible Count rates %u %u %u kHz; guessing\n", m[0], m[1], m[2]);
        return;
    }
    if (m[2] - m[0] > m[1] / 50)
        kprintf("timer: Count rates %u %u %u kHz disagree; using the median\n", m[0], m[1], m[2]);
    count_per_ms = m[1];
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

/*
 * Across a SUSPEND or HIBERNATE, Count stops or restarts from a reset, and
 * one Count delta can't span more than 2^32 counts (about 413 s) anyway.
 * The monotonic clock is carried across on the RTC's elapsed-time counter,
 * which keeps running in both (VR4131 UM table 12-3).
 */
static uint64_t sleep_etime;

uint64_t timer_etime(void)
{
    uint16_t h, m, l, m2;

    do {
        h = RTC_ETIME_H;
        m = RTC_ETIME_M;
        l = RTC_ETIME_L;
        m2 = RTC_ETIME_M;
    } while (m != m2);
    return ((uint64_t)h << 32) | ((uint32_t)m << 16) | l;
}

uint64_t timer_suspend(void)
{
    time_us();
    sleep_etime = timer_etime();
    return sleep_etime;
}

void timer_resume(void)
{
    uint64_t slept = (timer_etime() - sleep_etime) & 0xFFFFFFFFFFFFull;

    /* RTC ticks to microseconds: 1000000 / 32768 = 15625 / 512. */
    _Static_assert(RTC_HZ == 32768u, "the shift below is RTC_HZ / 64");
    us_total += slept * 15625u >> 9;
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
    usb_poll();
    net_irq();
    bt_poll();
    modem_poll();
    gps_poll();
    pcmcia1_irq_arm(net_up() || bt_ready() || modem_ready() || gps_ready());
    audio_irq();
    buzzer_poll();
    battery_tick();
    splash_tick();
}
