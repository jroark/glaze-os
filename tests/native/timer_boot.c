/* Boot calibration must finish even when the RTC tears or stops mid-sample. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define GLAZE_HW_H
#define GLAZE_MIPS_H
static unsigned reads, mode;
static uint64_t etime;          /* mode 4: a running RTC, for sleeps */
static uint32_t count;
static uint16_t rtc_read(void)
{
    unsigned n = reads++;
    if (mode == 4) return (uint16_t)etime;
    if (mode == 0) return 0;             /* stopped before first tick */
    if (mode == 1) return n < 4 ? 0 : 1; /* stops during measurement */
    if (mode == 2) return n & 1;         /* never stable */
    return (uint16_t)(n / 2);           /* stable pairs, including wrap */
}
#define RTC_ETIME_L rtc_read()
#define RTC_ETIME_M ((uint16_t)(etime >> 16))
#define RTC_ETIME_H ((uint16_t)(etime >> 32))
#define RTC_HZ 32768u
static uint32_t read_c0_count(void) { return mode == 4 ? count : reads * 159u; }
static void write_c0_compare(uint32_t v) { (void)v; }
#include "../../kernel/timer.c"
void kputs(const char *s) { (void)s; }
void kprintf(const char *s, ...) { (void)s; }
void cmcu_tick(void) {}
void sched_timer_tick(uint64_t t) { (void)t; }
void input_poll(void) {}
void usb_poll(void) {}
void net_irq(void) {}
void bt_poll(void) {}
void modem_poll(void) {}
void gps_poll(void) {}
bool gps_ready(void) { return false; }
bool bt_ready(void) { return false; }
bool modem_ready(void) { return false; }
void pcmcia1_irq_arm(bool on) { (void)on; }
bool net_up(void) { return false; }
void audio_irq(void) {}
void buzzer_poll(void) {}
void splash_tick(void) {}
void battery_tick(void) {}
int main(void)
{
    for (mode = 0; mode < 3; mode++) {
        reads = 0; rtc_ok = false;
        timer_init();
        assert(!timer_rtc_ok() && count_per_ms == GUESS_COUNT_PER_MS);
        assert(reads <= RTC_POLLS * 2 + 8);
    }
    mode = 3; reads = 131000; rtc_ok = false;
    timer_init();
    assert(timer_rtc_ok() && count_per_ms >= 10000 && count_per_ms <= 11000);

    /* A sleep longer than Count's wrap (about 413 s) is carried on ETIME,
     * whatever Count does meanwhile (stopped, or reset by a HIBERNATE). */
    mode = 4;
    etime = 0x00012345FFF0ull;
    count = 0xFFFFFF00u;
    uint64_t before = time_us();
    assert(timer_suspend() == etime);
    etime += 1000ull * RTC_HZ;
    count = 12345;
    timer_resume();
    uint64_t slept = time_us() - before;
    assert(slept >= 999999000ull && slept <= 1000001000ull);
    count += count_per_ms;      /* and runs on from there */
    assert(time_us() - before - slept >= 999 && time_us() - before - slept <= 1001);
    puts("timer-boot: stopped, mid-sample stop, torn reads, calibration across wrap, sleeps past Count's wrap PASS");
}
