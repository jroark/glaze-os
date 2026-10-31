/*
 * Battery, AC adapter and charger, read as Windows CE's battery driver does
 * (gwes.exe BatteryPDDGetStatus 0xB02CC-0xB09DC; docs/POWER.md):
 *
 *   GIUPIODL bit 12   1 on battery, 0 with the adapter in
 *   GIUPIODL bit 2    with the adapter: 1 charging, 0 charged
 *   PIU 0x390         the battery voltage, 12-bit, through an A/D port scan
 *   EEPROM 0x94/0x95  calibration (E2pRead selector 0x4A), else 4.2/3.7 V
 *
 * Everything runs from the timer tick with interrupts off. CE's 10 ms wait
 * after GPIO8 goes low becomes a two-tick gap, with the touch sequencer
 * stopped meanwhile as CE stops it; if the touch driver restarted it, the
 * sample is skipped. The averaging, bands and critical rule are in
 * glaze/battery.h.
 */

#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"
#include <glaze/battery.h>

#define TICK_HZ         100u
#define SAMPLE_TICKS    (2 * TICK_HZ)   /* between battery samples */
#define SETTLE_TICKS    2               /* >= 10 ms after GPIO8 (gwes 0xB041C) */
#define DEBOUNCE_TICKS  (TICK_HZ / 2)   /* adapter/charger lines held steady */
#define ADP_POLLS       3000            /* gwes 0xB0498 */

static struct gz_power_status st;
static struct battery_cal cal;
static uint16_t samples[BATTERY_SAMPLES];
static unsigned next_sample, low_readings;
static uint32_t ticks, next_due, settle_at;
static bool settling;
static uint16_t lines, pending_lines;
static uint32_t pending_ticks;
static uint64_t idle_us;

/* ---- serial EEPROM ------------------------------------------------------ */

/* eeprom.dll waits n * 6 Count ticks (01A21870), well under a microsecond
 * each at 5; a microsecond per unit is slow and safe for the part. */
static void ee_delay(uint32_t units)
{
    uint64_t start = time_us();
    while (time_us() - start <= units)
        ;
}

static void ee_scl(int v) { EEPROM_SCL = (uint32_t)v; }
static void ee_sda(int v) { EEPROM_SDA_OUT = (uint32_t)v; }

static void ee_start(void)          /* 01A21510 */
{
    ee_scl(1); ee_sda(1); ee_delay(20);
    ee_scl(1); ee_delay(5);
    ee_sda(0); ee_delay(5);
    ee_scl(0); ee_delay(5);
    ee_sda(1);
}

static void ee_bit_out(int bit)     /* 01A21574 */
{
    ee_scl(0); ee_sda(bit); ee_delay(5);
    ee_scl(1); ee_delay(10);
    ee_scl(0); ee_delay(5);
}

static int ee_bit_in(void)          /* 01A215D4 */
{
    ee_scl(0); ee_sda(1); ee_delay(5);
    ee_scl(1); ee_delay(5);
    int v = (int)(EEPROM_SDA_IN & 1u);
    ee_delay(5);
    ee_scl(0); ee_delay(5);
    return v;
}

static bool ee_send(uint8_t byte)   /* 01A21654: true when acknowledged */
{
    for (int bit = 7; bit >= 0; bit--)
        ee_bit_out((byte >> bit) & 1);
    ee_delay(5);
    return ee_bit_in() == 0;
}

static uint8_t ee_recv(int last)    /* 01A21784 */
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (uint8_t)((v << 1) | ee_bit_in());
    ee_delay(5);
    ee_bit_out(last);
    return v;
}

static void ee_stop(void)           /* 01A216FC */
{
    ee_sda(0); ee_scl(0); ee_delay(5);
    ee_scl(1); ee_delay(5);
    ee_sda(1); ee_delay(5);
}

/* E2pRead(0x4A, 0, buf, 2): device 0xA0, word address twice the selector.
 * CE ignores missing acknowledges; here they mean no usable calibration. */
static bool ee_read_cal(uint8_t *b0, uint8_t *b1)
{
    bool ok;

    EEPROM_GATE = 0;                /* 01A2136C..1A21380 */
    EEPROM_A140 = 1;
    EEPROM_A194 = 0;
    ee_start();
    ok = ee_send(0xA0);
    ok = ee_send((uint8_t)(0x4A * 2)) && ok;
    ee_start();
    ok = ee_send(0xA1) && ok;
    *b0 = ee_recv(0);
    *b1 = ee_recv(1);
    ee_stop();
    return ok;
}

/* ---- sampling ----------------------------------------------------------- */

static bool piu_idle(void)
{
    uint16_t state = PIU_CNT & PIUCNT_STATE;
    return state == PIUCNT_STATE_STBY || state == PIUCNT_STATE_WAIT;
}

/* gwes 0xB0388-0xB0414: stop the sequencer, GPIO8 low. GPIO8 is left low,
 * as CE leaves it. */
static void sample_begin(void)
{
    PIU_CNT &= (uint16_t)~PIUCNT_PADATSTART;
    PIU_CNT &= (uint16_t)~(PIUCNT_PADATSTART | PIUCNT_PIUSEQEN | PIUCNT_PADRST);
    GIU_PIODL &= (uint16_t)~GPIO_BATT_SENSE;
}

/* gwes 0xB0428-0xB0574. CE writes PIUINT back with its pending byte, which
 * would acknowledge touch events still pending; here only the mask byte
 * and bit 6 are written. 0 for no result. */
static uint16_t sample_convert(void)
{
    uint32_t ctl = PIU_ADP_CTL, cfg = PIU_ADP_CFG;
    uint16_t mask = PIU_INT & 0xFF00u;
    uint16_t v = 0;
    int n;

    PIU_ADP_CTL = 5;
    PIU_ADP_CFG = 0x14E;
    PIU_CNT |= PIUCNT_PIUMODE_ADP;
    PIU_INT = (uint16_t)(mask & ~((PIUINT_ADPORT << 8) | PIUINT_MASK_PENCHG));
    PIU_INT = (uint16_t)((PIU_INT & 0xFF00u) | PIUINT_ADPORT);
    PIU_CNT |= PIUCNT_PIUSEQEN;
    for (n = 0; n < ADP_POLLS && !(PIU_INT & PIUINT_ADPORT); n++)
        ;
    if (n < ADP_POLLS)
        v = (uint16_t)(PIU_ADPORT & 0x0FFFu);
    PIU_CNT &= (uint16_t)~PIUCNT_PIUSEQEN;
    PIU_INT = (uint16_t)((PIU_INT & 0xFF00u) | PIUINT_ADPORT);
    PIU_CNT &= (uint16_t)~PIUCNT_PIUMODE_ADP;
    PIU_ADP_CTL = ctl;
    PIU_ADP_CFG = cfg;
    PIU_INT = mask;
    input_touch_restart();
    return v;
}

/* gwes 0xB0704-0xB0868, as flags: on battery LOW at the 25 % band or
 * below and CRITICAL after two 10 % readings; with the adapter in and
 * charged, CE reports 100 % whatever the A/D reads. */
static void update_status(void)
{
    uint16_t avg = battery_average(samples, BATTERY_SAMPLES);
    uint32_t flags = st.flags & (GZ_POWER_VALID | GZ_POWER_CAL_EEPROM | GZ_POWER_BATTINT);

    if (avg) {
        st.adc_avg = avg;
        st.percent = battery_band(avg, &cal);
        st.level = battery_level(avg, &cal);
    }
    if (!(lines & GPIO_ON_BATTERY)) {
        flags |= GZ_POWER_AC;
        low_readings = 0;
        if (lines & GPIO_CHARGING)
            flags |= GZ_POWER_CHARGING;
        else
            st.percent = st.level = 100;
    } else if (flags & GZ_POWER_VALID) {
        if (st.percent <= 25)
            flags |= GZ_POWER_LOW;
        if (low_readings >= 2)
            flags |= GZ_POWER_CRITICAL;
    }
    st.flags = flags;
}

static void notify_if_changed(uint32_t flags, uint8_t percent, uint8_t level)
{
    if (st.flags != flags || st.percent != percent || st.level != level)
        input_notify(GZ_NOTIFY_POWER);
}

static void sample_end(void)
{
    uint32_t flags = st.flags;
    uint8_t percent = st.percent, level = st.level;
    uint16_t v;

    /* The touch driver restarted the scan meanwhile: try again soon. */
    if (PIU_CNT & PIUCNT_PIUSEQEN) {
        next_due = ticks + TICK_HZ / 4;
        return;
    }
    v = sample_convert();
    next_due = ticks + SAMPLE_TICKS;
    if (!v)
        return;                 /* CE keeps its last percentage */
    st.adc_last = v;
    st.samples++;
    samples[next_sample] = v;
    next_sample = (next_sample + 1) % BATTERY_SAMPLES;
    /* CE counts 10 % readings on battery until the adapter is back. */
    if ((lines & GPIO_ON_BATTERY) &&
        battery_band(battery_average(samples, BATTERY_SAMPLES), &cal) <= 10)
        low_readings++;
    st.flags |= GZ_POWER_VALID;
    update_status();
    notify_if_changed(flags, percent, level);
}

void battery_tick(void)
{
    uint16_t now = GIU_PIODL & (GPIO_ON_BATTERY | GPIO_CHARGING);

    ticks++;
    if (now != pending_lines) {
        pending_lines = now;
        pending_ticks = 0;
    } else if (now != lines && ++pending_ticks >= DEBOUNCE_TICKS) {
        uint32_t flags = st.flags;
        uint8_t percent = st.percent, level = st.level;
        lines = now;
        update_status();
        notify_if_changed(flags, percent, level);
    }
    if (!(st.flags & GZ_POWER_BATTINT) && (PMU_INT & PMUINT_BATTINTR)) {
        st.flags |= GZ_POWER_BATTINT;      /* read only, never acknowledged */
        input_notify(GZ_NOTIFY_POWER);
    }

    if (settling) {
        if (ticks - settle_at >= SETTLE_TICKS) {
            settling = false;
            sample_end();
        }
    } else if ((int32_t)(ticks - next_due) >= 0) {
        if (piu_idle()) {
            sample_begin();
            settling = true;
            settle_at = ticks;
        } else {
            next_due = ticks + TICK_HZ / 4;    /* the pen is down */
        }
    }
}

void battery_idle(uint32_t us)
{
    idle_us += us;
    st.standby_count++;
}

int battery_status(struct addrspace *as, uint32_t uout, uint32_t size)
{
    if (size != sizeof(st))
        return GZ_EINVAL;
    st.idle_ms = (uint32_t)(idle_us / 1000u);
    return copy_to_as(as, uout, &st, sizeof(st)) ? GZ_EINVAL : 0;
}

void battery_init(void)
{
    uint8_t b0 = 0xFF, b1 = 0xFF;
    bool acked = ee_read_cal(&b0, &b1);

    if (battery_cal_decode(b0, b1, &cal) && acked)
        st.flags |= GZ_POWER_CAL_EEPROM;
    else
        battery_cal_decode(0xFF, 0xFF, &cal);
    st.version = GZ_POWER_VERSION;
    st.cal_min = cal.min;
    st.cal_max = cal.max;
    st.percent = 100;
    st.level = 100;
    lines = pending_lines = GIU_PIODL & (GPIO_ON_BATTERY | GPIO_CHARGING);
    update_status();
    next_due = TICK_HZ / 2;    /* the first sample half a second in */
    kprintf("battery: %s, calibration %s 0x%x-0x%x (EEPROM %02x %02x%s)\n",
            lines & GPIO_ON_BATTERY ? "on battery" :
            lines & GPIO_CHARGING ? "charging" : "adapter, charged",
            st.flags & GZ_POWER_CAL_EEPROM ? "EEPROM" : "default",
            cal.min, cal.max, b0, b1, acked ? "" : ", no ack");
}
