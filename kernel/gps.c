/* GPS receiver on a CF serial card: cfuart.ko, baud discovery and the latest
 * NMEA-0183 sentences for any process (glaze/gps.h, docs/hardware/GPS.md).
 *
 * A serial card the socket dispatcher (cf.c) can't otherwise place comes
 * here. Nothing is sent to it: the UART listens at each rate in turn until
 * checksummed NMEA arrives. SiRF receivers (the AmbiCom GPS-CF's family)
 * may have been left in SiRF binary, framed A0 A2 ... B0 B3; only on seeing
 * that frame is the receiver sent message 129, "Switch To NMEA Protocol"
 * (SiRF Binary Protocol Reference Manual, 2.3), asking for 4800 baud.
 * A card that shows neither after a few passes is let go.
 */
#include "kernel.h"
#include "drivers.h"
#include "module.h"
#include "uart_card.h"
#include "mips.h"
#include "hw.h"
#include <glaze/gps.h>

/* 4800 first: NMEA-0183's rate and the SiRF CF cards' default. SiRF binary
 * defaults to 38400. Divisors of the 16550's 1.8432 MHz clock. */
static const uint16_t divisors[] = { 24, 12, 3, 6, 2 };
#define RATES (sizeof(divisors) / sizeof(divisors[0]))
#define WINDOW_US     2500000u   /* NMEA receivers talk at least once a second */
#define SILENCE_US    5000000u   /* streaming, then nothing: search again */
#define PASSES        6u         /* full passes over the rates before giving up */
/* Switching rate mid-character gives the right rate a framing error or two
 * too; a wrong rate gives one with nearly every byte. */
#define WINDOW_ERRORS 4u
#define LOCK_SENTENCES 2u

static struct module image;
static const struct uart_driver *driver;
static struct uart_config config;
static bool claimed;
static uint32_t state = GPS_ABSENT, generation, sequence, checksum_errors;
static unsigned rate, passes, valid_in_window, window_errors;
static uint64_t window_start, last_valid, switch_at;
static bool sirf_sent;
static uint8_t sirf_state;      /* 0, A0 seen, A0 A2 seen (in a frame), B0 seen */

static char line[GPS_SENTENCE_MAX];
static unsigned line_len;
static bool line_bad;

/* Stored sentences and when they arrived (time_us). */
static char rmc[GPS_SENTENCE_MAX], gga[GPS_SENTENCE_MAX], gsa[GPS_SENTENCE_MAX];
static char gsv[GPS_GSV_MAX][GPS_SENTENCE_MAX], gsv_next[GPS_GSV_MAX][GPS_SENTENCE_MAX];
static unsigned gsv_count, gsv_next_count, gsv_next_total;
static uint64_t rmc_at, gga_at, gsa_at, gsv_at;

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* NMEA-0183: "$" fields "*" two hex digits, XOR of everything between. */
static bool checksum_ok(const char *s, unsigned n)
{
    if (n < 6 || s[0] != '$' || s[n - 3] != '*') return false;
    uint8_t x = 0;
    for (unsigned i = 1; i < n - 3; i++) x ^= (uint8_t)s[i];
    int hi = hex(s[n - 2]), lo = hex(s[n - 1]);
    return hi >= 0 && lo >= 0 && x == (uint8_t)(hi << 4 | lo);
}

static unsigned small_number(const char *s)
{
    unsigned v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
    return v;
}

static void store(char *to, const char *from, unsigned n)
{
    memcpy(to, from, n);
    to[n] = 0;
}

/* Talker IDs vary ($GP, $GN, $GL); the sentence type is characters 3-5. */
static void sentence(const char *s, unsigned n, uint64_t now)
{
    if (!checksum_ok(s, n)) {
        checksum_errors++;
        return;
    }
    sequence++;
    valid_in_window++;
    last_valid = now;
    const char *type = s + 3;
    if (!memcmp(type, "RMC,", 4)) { store(rmc, s, n); rmc_at = now; }
    else if (!memcmp(type, "GGA,", 4)) { store(gga, s, n); gga_at = now; }
    else if (!memcmp(type, "GSA,", 4)) { store(gsa, s, n); gsa_at = now; }
    else if (!memcmp(type, "GSV,", 4)) {
        /* $GPGSV,total,number,in view,... collected until the cycle ends. */
        unsigned total = small_number(s + 7), number = n > 9 ? small_number(s + 9) : 0;
        if (!total || total > GPS_GSV_MAX || !number || number > total) return;
        if (number == 1) { gsv_next_count = 0; gsv_next_total = total; }
        if (total != gsv_next_total || number != gsv_next_count + 1) return;
        store(gsv_next[gsv_next_count++], s, n);
        if (gsv_next_count == total) {
            memcpy(gsv, gsv_next, sizeof(gsv));
            gsv_count = total;
            gsv_at = now;
            gsv_next_count = 0;
        }
    }
}

/* Re-arms the UART (init clears its error) without stop(), which would drop
 * DTR and RTS: some receivers take their power from them. */
static void rearm(void)
{
    if (driver->init(&config))
        kprintf("gps: UART init failed at divisor %u\n", config.divisor);
    line_len = 0; line_bad = false;
    sirf_state = 0;
}

static void listen_at(unsigned r, uint64_t now)
{
    rate = r;
    config.divisor = divisors[rate];
    rearm();
    window_start = now;
    valid_in_window = window_errors = 0;
}

static uint32_t baud(void) { return 115200u / divisors[rate]; }

/* SiRF message 129: NMEA at 4800 baud, GGA, GSA and RMC every second and
 * GSV every five; GLL, VTG, MSS and ZDA off. Checksum: 15-bit sum of the
 * payload, big-endian, before B0 B3. */
static void sirf_to_nmea(uint64_t now)
{
    static const uint8_t payload[24] = {
        0x81, 0x02, 1, 1, 0, 1, 1, 1, 5, 1, 1, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0x12, 0xC0,
    };
    uint8_t m[32] = { 0xA0, 0xA2, 0x00, sizeof(payload) };
    unsigned sum = 0;
    for (unsigned i = 0; i < sizeof(payload); i++) {
        m[4 + i] = payload[i];
        sum += payload[i];
    }
    sum &= 0x7FFF;
    m[28] = (uint8_t)(sum >> 8); m[29] = (uint8_t)sum; m[30] = 0xB0; m[31] = 0xB3;
    kprintf("gps: SiRF binary at %u baud; asking for NMEA at 4800\n", baud());
    driver->write(m, sizeof(m));
    sirf_sent = true;
    switch_at = now + 200000u;  /* 32 bytes take under 70 ms at 4800 */
}

static void give_up(void)
{
    kputs("gps: no NMEA at any rate; not a GPS receiver\n");
    driver->stop();
    driver = NULL;
    state = GPS_ABSENT;
    generation++;
    module_unload(&image);
}

uint32_t gps_generation(void) { return generation; }
bool gps_ready(void) { return driver && PCMCIA_CARD_PRESENT() && !driver->error(); }

void gps_poll(void)
{
    if (!driver || !PCMCIA_CARD_PRESENT()) return;
    uint64_t now = time_us();
    driver->poll();
    uint8_t bytes[128];
    int got;
    while ((got = driver->read(bytes, sizeof(bytes))) > 0) {
        driver->consume((uint32_t)got);
        for (int i = 0; i < got; i++) {
            uint8_t b = bytes[i];
            /* SiRF binary framing, while searching. */
            if (state == GPS_SEARCHING && !sirf_sent) {
                if (b == 0xA0) sirf_state = 1;
                else if (sirf_state == 1) sirf_state = b == 0xA2 ? 2 : 0;
                else if (sirf_state == 2 && b == 0xB0) sirf_state = 3;
                else if (sirf_state == 3 && b == 0xB3) { sirf_to_nmea(now); sirf_state = 0; }
                else if (sirf_state == 3) sirf_state = 2;
            }
            if (b == '$') { line_len = 0; line_bad = false; }
            if (b == '\r' || b == '\n') {
                if (line_len && !line_bad && line[0] == '$') sentence(line, line_len, now);
                line_len = 0; line_bad = false;
                continue;
            }
            if (b < 32 || b > 126 || line_len + 1 >= GPS_SENTENCE_MAX) { line_bad = true; continue; }
            line[line_len++] = (char)b;
        }
    }
    if (switch_at) {
        if (now >= switch_at) { switch_at = 0; listen_at(0, now); }
        return;
    }
    if (state == GPS_SEARCHING) {
        if (valid_in_window >= LOCK_SENTENCES) {
            state = GPS_STREAMING;
            passes = 0;
            kprintf("gps: NMEA at %u baud\n", baud());
        } else if (driver->error() && ++window_errors <= WINDOW_ERRORS && now - window_start <= WINDOW_US) {
            rearm();    /* likely the half character the switch cut into */
        } else if (driver->error() || now - window_start > WINDOW_US) {
            /* A wrong rate shows up as repeated framing errors, or as noise. */
            unsigned next = (rate + 1) % RATES;
            if (!next && ++passes >= PASSES) { give_up(); return; }
            listen_at(next, now);
        }
    } else if (driver->error()) {
        kprintf("gps: UART error %d at %u baud, restarting\n", driver->error(), baud());
        rearm();
    } else if (now - last_valid > SILENCE_US) {
        kputs("gps: receiver went quiet, searching again\n");
        state = GPS_SEARCHING;
        generation++;
        listen_at(rate, now);
    }
}

void gps_removed(void)
{
    if (!claimed) return;
    if (driver) driver->stop();
    if (driver || image.entry) module_unload(&image);
    driver = NULL; claimed = false; state = GPS_ABSENT; generation++;
}

void gps_insert(uint32_t cor_address, unsigned config_index)
{
    if (claimed) return;
    claimed = true; generation++;
    sequence = checksum_errors = 0;
    rmc_at = gga_at = gsa_at = gsv_at = 0;
    rmc[0] = gga[0] = gsa[0] = 0;
    gsv_count = gsv_next_count = 0;
    sirf_sent = false; switch_at = 0; passes = 0;
    const struct uart_driver *d = uart_card_load(&image);
    if (!d) {
        module_unload(&image);
        kputs("gps: UART module unavailable\n");
        return;
    }
    /* 8N1, FIFO on with a one-byte trigger, DTR, RTS and OUT2 (the card's
     * interrupt enable on PC serial ports). COR: level interrupts. Some
     * receivers take their power from DTR/RTS, as on a PC serial mouse. */
    config = (struct uart_config){
        .cor_address = cor_address, .io_base = 0x3f8, .divisor = divisors[0],
        .cor = (uint8_t)(0x40u | (config_index & 0x3Fu)), .lcr = 0x03, .mcr = 0x0b, .fcr = 7,
    };
    driver = d;
    state = GPS_SEARCHING;
    kprintf("gps: serial card, COR %x at %x: listening for NMEA\n", config.cor, cor_address);
    listen_at(0, time_us());
    last_valid = time_us();
}

static uint32_t age(uint64_t at, uint64_t now)
{
    if (!at) return GPS_AGE_NEVER;
    uint64_t ms = (now - at) / 1000u;
    return ms >= GPS_AGE_NEVER ? GPS_AGE_NEVER - 1 : (uint32_t)ms;
}

static struct gps_snapshot snap;

int sys_gps_io(struct proc *p, uint32_t op, uint32_t buffer, uint32_t size)
{
    if (op == GPS_IO_SNAPSHOT) {
        if (size != sizeof(snap)) return GZ_EINVAL;
        uint32_t s = irq_save();
        gps_poll();
        uint64_t now = time_us();
        memset(&snap, 0, sizeof(snap));
        snap.version = GPS_SNAPSHOT_VERSION;
        snap.size = sizeof(snap);
        snap.state = driver && PCMCIA_CARD_PRESENT() ? state : GPS_ABSENT;
        snap.baud = snap.state == GPS_STREAMING ? baud() : 0;
        snap.generation = generation;
        snap.sequence = sequence;
        snap.checksum_errors = checksum_errors;
        snap.rmc_age = snap.gga_age = snap.gsa_age = snap.gsv_age = GPS_AGE_NEVER;
        if (snap.state == GPS_ABSENT) {    /* nothing from a card that's gone */
            irq_restore(s);
            return copy_to_as(&p->as, buffer, &snap, sizeof(snap)) ? GZ_EINVAL : 0;
        }
        snap.rmc_age = age(rmc_at, now);
        snap.gga_age = age(gga_at, now);
        snap.gsa_age = age(gsa_at, now);
        snap.gsv_age = age(gsv_at, now);
        snap.gsv_count = gsv_count;
        memcpy(snap.rmc, rmc, sizeof(rmc));
        memcpy(snap.gga, gga, sizeof(gga));
        memcpy(snap.gsa, gsa, sizeof(gsa));
        memcpy(snap.gsv, gsv, sizeof(gsv));
        irq_restore(s);
        return copy_to_as(&p->as, buffer, &snap, sizeof(snap)) ? GZ_EINVAL : 0;
    }
    if (op == GPS_IO_WRITE) {
        if (!size || size > GPS_IO_WRITE_MAX) return GZ_EINVAL;
        uint8_t bytes[GPS_IO_WRITE_MAX];
        if (copy_from_as(&p->as, bytes, buffer, size)) return GZ_EINVAL;
        uint32_t s = irq_save();
        int rv = driver && PCMCIA_CARD_PRESENT() && state == GPS_STREAMING
                 ? driver->write(bytes, size) : GZ_ENOENT;
        irq_restore(s);
        return rv;
    }
    return GZ_EINVAL;
}
