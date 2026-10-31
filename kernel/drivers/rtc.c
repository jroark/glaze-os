/*
 * Calendar time on the VR4131 RTC elapsed-time counter.
 *
 * ETIME is a 48-bit count of 32.768 kHz ticks. Glaze follows the WinCE
 * convention the BE-300 firmware and emulator use: local time since
 * 1850-01-01 00:00 (doc: RTC and Timers; be300 CLAUDE.md "RTC And Host Time").
 * A battery-removed cold boot starts the counter at zero.
 */

#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"

#define EPOCH_YEAR      1850
#define EPOCH_WEEKDAY   2       /* 1850-01-01 was a Tuesday */

/* tests/rtc_boot.c supplies these two with a model of the write latency. */
#ifndef RTC_ETIME_MODEL
static uint64_t etime_read(void)
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

static void etime_write(uint64_t t)
{
    RTC_ETIME_L = (uint16_t)t;
    RTC_ETIME_M = (uint16_t)(t >> 16);
    RTC_ETIME_H = (uint16_t)(t >> 32);
}
#endif

/*
 * VR4131 UM §13.2.1: a new count is valid once all three words are written,
 * and setting them again must wait at least 100 us (three 32.768 kHz clocks).
 * The RTC runs in its own clock domain, so a read straight after the write
 * still sees the old count: on a real BE-300 rtc_init's default read back as
 * 1850 (the emulator applies writes at once). Read back only after the RTC
 * has taken the value, and write again if it did not.
 */
#define ETIME_SETTLE_US 200u
static uint64_t written_us;

static void settle(void)
{
    uint64_t t0 = written_us;
    while (time_us() - t0 < ETIME_SETTLE_US)
        ;
}

static bool etime_set(uint64_t t)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        settle();               /* after any previous write */
        etime_write(t);
        written_us = time_us();
        settle();
        uint64_t now = etime_read();
        if (now >= t && now - t < RTC_HZ)
            return true;
    }
    return false;
}

static bool leap(uint32_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static uint32_t month_days(uint32_t y, uint32_t m)
{
    static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return days[m - 1] + (m == 2 && leap(y));
}

static void secs_to_dt(uint64_t secs, struct gz_datetime *dt)
{
    uint32_t days = (uint32_t)(secs / 86400u);
    uint32_t rem = (uint32_t)(secs % 86400u);
    uint32_t y = EPOCH_YEAR, m = 1;

    dt->weekday = (uint8_t)((days + EPOCH_WEEKDAY) % 7);
    while (days >= 365u + leap(y)) {
        days -= 365u + leap(y);
        y++;
    }
    while (days >= month_days(y, m)) {
        days -= month_days(y, m);
        m++;
    }
    dt->year = (uint16_t)y;
    dt->month = (uint8_t)m;
    dt->day = (uint8_t)(days + 1);
    dt->hour = (uint8_t)(rem / 3600);
    dt->minute = (uint8_t)(rem / 60 % 60);
    dt->second = (uint8_t)(rem % 60);
}

static bool dt_valid(const struct gz_datetime *dt)
{
    return dt->year >= EPOCH_YEAR && dt->year < 2200 && dt->month >= 1 && dt->month <= 12 &&
           dt->day >= 1 && dt->day <= month_days(dt->year, dt->month) &&
           dt->hour < 24 && dt->minute < 60 && dt->second < 60;
}

static uint64_t dt_to_secs(const struct gz_datetime *dt)
{
    uint64_t days = 0;

    for (uint32_t y = EPOCH_YEAR; y < dt->year; y++)
        days += 365u + leap(y);
    for (uint32_t m = 1; m < dt->month; m++)
        days += month_days(dt->year, m);
    days += dt->day - 1u;
    return days * 86400u + dt->hour * 3600u + dt->minute * 60u + dt->second;
}

void rtc_init(void)
{
    struct gz_datetime dt;

    secs_to_dt(etime_read() / RTC_HZ, &dt);
    if (dt.year < 2000) {
        /* Battery-removed cold boot: the counter restarted at the epoch.
         * Start the clock at a plausible date until Settings sets it. */
        static const struct gz_datetime def = { 2009, 1, 1, 9, 41, 0, 0 };
        bool ok = etime_set(dt_to_secs(&def) * RTC_HZ);
        secs_to_dt(etime_read() / RTC_HZ, &dt);
        kputs(ok ? "rtc: clock was unset, defaulted\n" : "rtc: clock was unset; the RTC ignored the default\n");
    }
    kprintf("rtc: %u-%02u-%02u %02u:%02u:%02u\n",
            dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
}

int sys_rtc_get(struct addrspace *as, uint32_t uout)
{
    struct gz_datetime dt;

    secs_to_dt(etime_read() / RTC_HZ, &dt);
    return copy_to_as(as, uout, &dt, sizeof(dt));
}

int sys_rtc_set(struct addrspace *as, uint32_t uin)
{
    struct gz_datetime dt;

    if (copy_from_as(as, &dt, uin, sizeof(dt)) || !dt_valid(&dt))
        return GZ_EINVAL;
    return etime_set(dt_to_secs(&dt) * RTC_HZ) ? 0 : GZ_EIO;
}
