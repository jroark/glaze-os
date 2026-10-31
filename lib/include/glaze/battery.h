/* Battery level arithmetic, shared by the kernel driver
 * (kernel/drivers/battery.c) and its host test (tests/battery_driver.c).
 *
 * It follows Windows CE's battery driver, compiled into gwes.exe
 * (be300-modules 03_gwes.exe.bin, VA = file offset + 0x10000):
 *   calibration   0xB0AF0-0xB0D30, from E2pRead(0x4A) bytes 0 (max), 1 (min)
 *   averaging     0xB05B0-0xB069C, ten samples, the largest dropped
 *   bands         0xB06A0-0xB06FC, and the unused min - 3 * span / 5
 *                 edge (3.4 V, the manual's cut-off) at 0xB0CC0-0xB0D04
 *   critical      0xB0778-0xB07EC, two 10 % readings since the adapter
 * See docs/POWER.md. */
#ifndef GLAZE_BATTERY_H
#define GLAZE_BATTERY_H
#include <glaze/types.h>

#define BATTERY_CAL_MAX_DEFAULT 0x0D96u   /* 4.2 V (gwes 0xB0C08) */
#define BATTERY_CAL_MIN_DEFAULT 0x0BF8u   /* 3.7 V (gwes 0xB0C04) */
#define BATTERY_SAMPLES         10        /* gwes 0xB0624 */

struct battery_cal {
    uint16_t min, max;
    uint16_t t75, t50, t25, t10;    /* a reading at or below tN is N % */
    uint16_t t0;                    /* 3.4 V: empty */
};

/* E2pRead(0x4A) bytes b0, b1 to counts; false when they are unusable and
 * CE's defaults were taken. The 0x802 case is CE's, odd as it looks. */
static inline bool battery_cal_decode(uint8_t b0, uint8_t b1, struct battery_cal *c)
{
    bool eeprom = b1 < b0;

    if (eeprom) {
        c->max = (uint16_t)((b0 << 2) | 0xC02u);
        c->min = (uint16_t)((b1 << 2) | (b1 < 0x7F ? 0xC02u : 0x802u));
    } else {
        c->max = BATTERY_CAL_MAX_DEFAULT;
        c->min = BATTERY_CAL_MIN_DEFAULT;
    }
    int span = c->max - c->min;
    c->t75 = (uint16_t)(c->max - span * 2 / 5);
    c->t50 = (uint16_t)(c->max - span * 4 / 5);
    c->t25 = c->min;
    c->t10 = (uint16_t)(c->min - span / 10);
    int t0 = c->min - span * 3 / 5;
    c->t0 = (uint16_t)(t0 > 0 ? t0 : 0);
    return eeprom;
}

/* Mean of the non-zero samples; of all ten, without the largest. */
static inline uint16_t battery_average(const uint16_t *samples, int n)
{
    uint32_t sum = 0, count = 0, max = 0;

    for (int i = 0; i < n; i++) {
        if (!samples[i])
            continue;
        sum += samples[i];
        count++;
        if (samples[i] > max)
            max = samples[i];
    }
    if (count == BATTERY_SAMPLES)
        return (uint16_t)((sum - max) / (count - 1));
    return count ? (uint16_t)(sum / count) : 0;
}

static inline uint8_t battery_band(uint16_t avg, const struct battery_cal *c)
{
    if (avg <= c->t10) return 10;
    if (avg <= c->t25) return 25;
    if (avg <= c->t50) return 50;
    if (avg <= c->t75) return 75;
    return 100;
}

/* Piecewise linear through CE's own edges: 0 % at 3.4 V, then 10, 25, 50,
 * 75 and 100 % at its band edges (3.65, 3.7, 3.8, 4.0 and 4.2 V with the
 * default calibration). CE's band is then this level rounded up to one of
 * 10, 25, 50, 75 or 100, so the two never disagree. */
static inline uint8_t battery_level(uint16_t avg, const struct battery_cal *c)
{
    const uint16_t at[6] = { c->t0, c->t10, c->t25, c->t50, c->t75, c->max };
    static const uint8_t pct[6] = { 0, 10, 25, 50, 75, 100 };

    if (avg <= at[0])
        return 0;
    for (int i = 1; i < 6; i++) {
        if (avg > at[i])
            continue;
        if (at[i] <= at[i - 1])
            return pct[i];
        return (uint8_t)(pct[i - 1] + (uint32_t)(avg - at[i - 1]) * (pct[i] - pct[i - 1]) /
                         (uint32_t)(at[i] - at[i - 1]));
    }
    return 100;
}

#endif
