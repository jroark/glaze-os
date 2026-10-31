/* Boot calibration must finish even when the RTC tears or stops mid-sample. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define GLAZE_HW_H
#define GLAZE_MIPS_H
static unsigned reads, mode;
static uint16_t rtc_read(void)
{
    unsigned n = reads++;
    if (mode == 0) return 0;             /* stopped before first tick */
    if (mode == 1) return n < 4 ? 0 : 1; /* stops during measurement */
    if (mode == 2) return n & 1;         /* never stable */
    return (uint16_t)(n / 2);           /* stable pairs, including wrap */
}
#define RTC_ETIME_L rtc_read()
#define RTC_HZ 32768u
static uint32_t read_c0_count(void) { return reads * 159u; }
static void write_c0_compare(uint32_t v) { (void)v; }
#include "../kernel/timer.c"
void kputs(const char *s) { (void)s; }
void kprintf(const char *s, ...) { (void)s; }
void cmcu_tick(void) {}
void sched_timer_tick(uint64_t t) { (void)t; }
void input_poll(void) {}
void usb_poll(void) {}
void net_irq(void) {}
void audio_irq(void) {}
void buzzer_poll(void) {}
void splash_tick(void) {}
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
    puts("timer-boot: stopped, mid-sample stop, torn reads, calibration across wrap PASS");
}
