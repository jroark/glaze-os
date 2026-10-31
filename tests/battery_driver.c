/* kernel/drivers/battery.c and glaze/battery.h natively, on fake registers:
 * CE's calibration, averaging and bands, the A/D sample's register
 * sequence and restore, debounced adapter lines, the critical rule,
 * notifications and the status ABI. make -C host battery-test. */
#include "../kernel/kernel.h"
#include "../kernel/drivers.h"
#include "../kernel/hw.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static _Alignas(4) uint8_t companion[0xB000], vr41xx[0x200];
static uint64_t now;
static int notified, conversions;   /* each ends in input_touch_restart */
static uint16_t adc;

static volatile void *mmio(uint32_t pa)
{
    if (pa >= 0x0A000000u && pa < 0x0A000000u + sizeof(companion))
        return &companion[pa - 0x0A000000u];
    assert(pa >= 0x0F000000u && pa < 0x0F000000u + sizeof(vr41xx));
    return &vr41xx[pa - 0x0F000000u];
}
#undef REG16
#undef REG32
#define REG16(pa) (*(volatile uint16_t *)mmio(pa))
#define REG32(pa) (*(volatile uint32_t *)mmio(pa))

uint64_t time_us(void) { return ++now; }      /* the EEPROM delays spin on it */
void kprintf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void input_touch_restart(void) { conversions++; REG16(0x0A000300u) = 0x1306u; }
void input_notify(uint32_t bits) { assert(bits == GZ_NOTIFY_POWER); notified++; }
static struct gz_power_status copied;
int copy_to_as(struct addrspace *as, uint32_t uva, const void *src, uint32_t len)
{
    memcpy(&copied, src, len);
    return 0;
}

#include "../kernel/drivers/battery.c"

static void tick(int n)
{
    for (int i = 0; i < n; i++) {
        now += 10000;
        PIU_ADPORT = adc;
        battery_tick();
    }
}

static struct gz_power_status status(void)
{
    assert(battery_status(NULL, 0, sizeof(struct gz_power_status)) == 0);
    return copied;
}

static void test_arithmetic(void)
{
    struct battery_cal c;
    uint16_t s[BATTERY_SAMPLES] = { 0 };

    assert(!battery_cal_decode(0xFF, 0xFF, &c));
    assert(c.max == 0xD96 && c.min == 0xBF8);
    int span = 0xD96 - 0xBF8;
    assert(c.t10 == 0xBF8 - span / 10 && c.t25 == 0xBF8);
    assert(c.t50 == 0xD96 - span * 4 / 5 && c.t75 == 0xD96 - span * 2 / 5);
    assert(battery_band(c.t10, &c) == 10 && battery_band(c.t10 + 1, &c) == 25);
    assert(battery_band(c.t25 + 1, &c) == 50 && battery_band(c.t75, &c) == 75);
    assert(battery_band(c.t75 + 1, &c) == 100);
    assert(c.t0 == 0xBF8 - span * 3 / 5);
    assert(battery_level(c.t0, &c) == 0 && battery_level(c.max, &c) == 100);
    assert(battery_level(c.t10, &c) == 10 && battery_level(c.t25, &c) == 25);
    assert(battery_level(c.t50, &c) == 50 && battery_level(c.t75, &c) == 75);
    /* CE's band is the level rounded up to its next edge. */
    for (uint16_t v = c.t0 + 1; v <= c.max; v++) {
        int l = battery_level(v, &c), b = battery_band(v, &c);
        assert(b == (l <= 10 ? 10 : l <= 25 ? 25 : l <= 50 ? 50 : l <= 75 ? 75 : 100) ||
               (l == 10 && b == 25) || (l == 25 && b == 50) || (l == 50 && b == 75) || (l == 75 && b == 100));
    }

    assert(battery_cal_decode(0xD9, 0x7E, &c) && c.max == 0xF66 && c.min == 0xDFA);
    assert(battery_cal_decode(0xE0, 0x80, &c) && c.min == ((0x80 << 2) | 0x802));
    assert(!battery_cal_decode(0x50, 0x50, &c) && c.max == 0xD96);

    assert(battery_average(s, BATTERY_SAMPLES) == 0);
    s[0] = 100; s[1] = 200;
    assert(battery_average(s, BATTERY_SAMPLES) == 150);
    for (int i = 0; i < BATTERY_SAMPLES; i++)
        s[i] = 100;
    s[3] = 1000;            /* all ten: the largest is dropped */
    assert(battery_average(s, BATTERY_SAMPLES) == 100);
}

static void test_driver(void)
{
    struct gz_power_status st;

    /* After Windows CE: adapter in and charged, touch waiting for the pen,
     * GPIO8 high, CE's A/D settings, the touch mask. The EEPROM never
     * acknowledges (SDA reads 1), so CE's defaults are used. */
    GIU_PIODL = 0xAAE2u | GPIO_BATT_SENSE;
    PIU_CNT = 0x1306u;
    PIU_INT = 0xDD00u;
    PIU_ADP_CTL = 0;
    PIU_ADP_CFG = 1;
    EEPROM_SDA_IN = 1;
    EEPROM_GATE = 1;
    battery_init();
    assert(EEPROM_GATE == 0 && EEPROM_A140 == 1 && EEPROM_SCL == 1 && EEPROM_SDA_OUT == 1);
    st = status();
    assert(st.version == GZ_POWER_VERSION && st.cal_min == 0xBF8 && st.cal_max == 0xD96);
    assert((st.flags & (GZ_POWER_AC | GZ_POWER_CAL_EEPROM | GZ_POWER_VALID)) == GZ_POWER_AC);
    assert(battery_status(NULL, 0, sizeof(st) - 4) == GZ_EINVAL);

    /* The first sample half a second in; a pen-down defers it. */
    adc = 0xD00;
    PIU_CNT = 0x1706u;               /* state 5: scanning */
    tick(60);
    assert(!conversions && (GIU_PIODL & GPIO_BATT_SENSE));
    PIU_CNT = 0x1306u;
    tick(30);
    assert(conversions == 1);
    assert(!(GIU_PIODL & GPIO_BATT_SENSE));             /* left low, as CE */
    assert(PIU_ADP_CTL == 0 && PIU_ADP_CFG == 1 && PIU_INT == 0xDD00u);
    st = status();
    assert((st.flags & GZ_POWER_VALID) && st.adc_last == 0xD00 && st.samples == 1);
    assert(st.percent == 100 && st.level == 100);       /* charged: CE says 100 */

    /* A touch restarting the sequencer during the settle skips the sample. */
    int reads = conversions, i;
    for (i = 0; i < 400 && (PIU_CNT & PIUCNT_PIUSEQEN); i++)
        tick(1);                     /* until sample_begin stops it */
    assert(i < 400 && settling);
    PIU_CNT = 0x1306u;               /* the touch driver's restart */
    tick(SETTLE_TICKS + 1);
    assert(conversions == reads && !settling);
    tick(30);                        /* retried a quarter second later */
    assert(conversions == reads + 1);

    /* Unplugged: debounced over half a second, then LOW at the 25 % band. */
    notified = 0;
    GIU_PIODL |= GPIO_ON_BATTERY;
    tick(10);
    assert(status().flags & GZ_POWER_AC);
    tick(60);
    st = status();
    assert(!(st.flags & GZ_POWER_AC) && notified >= 1);
    adc = 0xBF0;                     /* between 10 % and 25 % */
    tick(25 * 200);                  /* ten samples, so the average follows */
    st = status();
    assert(st.percent == 25 && (st.flags & GZ_POWER_LOW) && !(st.flags & GZ_POWER_CRITICAL));

    /* Two 10 % readings: CRITICAL, latched until the adapter. */
    adc = 0x900;
    tick(12 * 200);
    st = status();
    assert(st.percent == 10 && st.level < 10 && (st.flags & GZ_POWER_CRITICAL));
    adc = 0xD00;
    tick(12 * 200);
    st = status();
    assert(st.percent >= 75 && (st.flags & GZ_POWER_CRITICAL));
    GIU_PIODL &= (uint16_t)~GPIO_ON_BATTERY;
    GIU_PIODL |= GPIO_CHARGING;
    tick(60);
    st = status();
    assert((st.flags & (GZ_POWER_AC | GZ_POWER_CHARGING)) == (GZ_POWER_AC | GZ_POWER_CHARGING));
    assert(!(st.flags & (GZ_POWER_CRITICAL | GZ_POWER_LOW)));

    /* BATTINTR is reported, never acknowledged. */
    PMU_INT = PMUINT_BATTINTR;
    tick(1);
    assert((status().flags & GZ_POWER_BATTINT) && PMU_INT == PMUINT_BATTINTR);

    battery_idle(1500);
    battery_idle(2500);
    st = status();
    assert(st.idle_ms == 4 && st.standby_count == 2);
}

int main(void)
{
    test_arithmetic();
    test_driver();
    puts("battery: CE calibration, bands, A/D sequence, adapter debounce, critical latch and status ABI PASS");
    return 0;
}
