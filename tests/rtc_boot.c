/* rtc_init's default and sys_rtc_set against an RTC that, like the VR4131's
 * (UM §13.2.1), takes a written count three 32.768 kHz clocks later. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define RTC_ETIME_MODEL
static uint64_t now_us, base, base_us, pending, pending_at, last_write_us = 1000000;
static int has_pending, drop, writes;
uint64_t time_us(void) { return now_us += 7; }
static uint64_t counter(void) { return base + (now_us - base_us) * 32768u / 1000000u; }
static uint64_t etime_read(void)
{
    if (has_pending && now_us >= pending_at) { base = pending; base_us = pending_at; has_pending = 0; }
    return counter();
}
static void etime_write(uint64_t t)
{
    writes++;
    /* A write within 100 us of the last one, or one we drop, is lost. */
    if (drop ? drop-- : now_us - last_write_us < 100) { last_write_us = now_us; return; }
    last_write_us = now_us;
    pending = t; pending_at = now_us + 92; has_pending = 1;
}
#include "../kernel/drivers/rtc.c"
static char said[96];
void kputs(const char *s) { snprintf(said, sizeof(said), "%s", s); }
void kprintf(const char *s, ...) { (void)s; }
static struct gz_datetime user;
int copy_to_as(struct addrspace *as, uint32_t u, const void *p, uint32_t n) { (void)as; (void)u; memcpy(&user, p, n); return 0; }
int copy_from_as(struct addrspace *as, void *p, uint32_t u, uint32_t n) { (void)as; (void)u; memcpy(p, &user, n); return 0; }
static void boot(int dropped)
{
    now_us = base_us = 5000000; base = 64u * 32768u;     /* 64 s after a battery pull */
    has_pending = 0; drop = dropped; writes = 0;
    rtc_init();
    sys_rtc_get(NULL, 0);
}
int main(void)
{
    /* The default lands even though the RTC takes it a few clocks later. */
    boot(0);
    assert(!strcmp(said, "rtc: clock was unset, defaulted\n") && writes == 1);
    assert(user.year == 2009 && user.month == 1 && user.day == 1 && user.hour == 9 && user.minute == 41);
    /* A lost write is written again. */
    boot(1);
    assert(writes == 2 && user.year == 2009);
    /* An RTC that never takes it says so, and sys_rtc_set fails. */
    boot(3);
    assert(writes == 3 && user.year == 1850 && strstr(said, "ignored"));
    user = (struct gz_datetime){ 2026, 10, 31, 12, 0, 0, 0 };
    drop = 3;
    assert(sys_rtc_set(NULL, 0) == GZ_EIO);
    assert(sys_rtc_set(NULL, 0) == 0);
    sys_rtc_get(NULL, 0);
    assert(user.year == 2026 && user.month == 10 && user.day == 31 && user.hour == 12);
    puts("rtc-boot: delayed ETIME writes, lost-write retry, ignored writes PASS");
}
