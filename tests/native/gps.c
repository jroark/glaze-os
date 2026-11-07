/* kernel/gps.c's baud search, SiRF switch and snapshot, and lib/gps/gps.c's
 * NMEA parsing and integer Mercator, natively against a mock 16550. */
#define GLAZE_HW_H
static int socket_present = 1;
#define PCMCIA_CARD_PRESENT() (socket_present != 0)
#define GLAZE_MIPS_H
static unsigned irq_off;
static inline unsigned irq_save(void) { return irq_off++; }
static inline void irq_restore(unsigned s) { irq_off = s; }
#include "../../kernel/gps.c"
#include <assert.h>
#include <math.h>
#include <stdio.h>

/* --- the mock UART: a receiver talking at `talk_divisor` --- */
static const struct uart_config *configured;
static unsigned inits, stops, unloaded;
static int talk_divisor = 24, uart_fault;
static unsigned glitches;           /* framing errors to give at the right rate */
static char stream[4096]; static unsigned stream_len, stream_pos;
static uint8_t sent[256]; static unsigned sent_len;
static int initialize(const struct uart_config *c) { configured = c; inits++; uart_fault = 0; stream_pos = stream_len; return 0; }
static void poll_uart(void) {}
static void stop_uart(void) { stops++; }
static int error_uart(void) { return uart_fault; }
static void feed(const char *s) { size_t n = strlen(s); memcpy(stream + stream_len, s, n); stream_len += n; }
static void feed_bytes(const uint8_t *s, unsigned n) { memcpy(stream + stream_len, s, n); stream_len += n; }
static int read_uart(uint8_t *p, uint32_t n)
{
    if (uart_fault) return uart_fault;
    /* At the wrong rate a real UART sees framing errors. */
    if (stream_pos < stream_len && configured->divisor != talk_divisor) { uart_fault = GZ_EIO; return uart_fault; }
    if (stream_pos < stream_len && glitches) { glitches--; stream_pos++; uart_fault = GZ_EIO; return uart_fault; }
    unsigned left = stream_len - stream_pos; if (n > left) n = left;
    memcpy(p, stream + stream_pos, n); return (int)n;
}
static void consume_uart(uint32_t n) { stream_pos += n; }
static int write_uart(const uint8_t *p, uint32_t n) { memcpy(sent, p, n); sent_len = n; return (int)n; }
static void set_lines(uint8_t m) {}
static uint8_t msr(void) { return 0; }
static struct uart_driver descriptor = { UART_DRIVER_MAGIC, UART_DRIVER_ABI, sizeof(struct uart_driver),
    initialize, poll_uart, stop_uart, error_uart, read_uart, consume_uart, write_uart, set_lines, msr };
int module_load(const char *path, const char *symbol, struct module *m)
{
    assert(!strcmp(path, "drivers/cfuart.ko") && !strcmp(symbol, "glaze_driver"));
    m->entry = &descriptor; return 0;
}
void module_unload(struct module *m) { if (m->entry) unloaded++; memset(m, 0, sizeof(*m)); }
bool module_contains(const struct module *m, const void *p, uint32_t size) { return p == &descriptor; }
bool module_executable(const struct module *m, const void *p) { return p != NULL; }
void kputs(const char *s) { (void)s; }
void kprintf(const char *f, ...) { (void)f; }
static uint64_t clock_us = 1;
uint64_t time_us(void) { return clock_us; }
static void advance(uint64_t us) { clock_us += us; }

/* User memory: address 1 is `user`. */
static uint8_t user[sizeof(struct gps_snapshot)];
static const void *user_source;
int copy_from_as(struct addrspace *as, void *p, uint32_t source, uint32_t size) { memcpy(p, user_source, size); return 0; }
int copy_to_as(struct addrspace *as, uint32_t target, const void *p, uint32_t size)
{ assert(target == 1 && size <= sizeof(user)); memcpy(user, p, size); return 0; }
static struct proc proc;

/* libc's gps.c, with the syscall going to sys_gps_io. */
#define GLAZE_SYS_H
static int gz_gps_io(uint32_t op, void *buffer, uint32_t size)
{
    if (op == GPS_IO_WRITE) { user_source = buffer; return sys_gps_io(&proc, op, 1, size); }
    int rv = sys_gps_io(&proc, op, 1, size);
    if (!rv) memcpy(buffer, user, size);
    return rv;
}
#include "../../lib/gps/gps.c"

static void nmea(const char *body)
{
    uint8_t x = 0;
    for (const char *p = body; *p; p++) x ^= (uint8_t)*p;
    char line[100];
    snprintf(line, sizeof(line), "$%s*%02X\r\n", body, x);
    feed(line);
}

/* A SiRF receiver's fix, one second's worth. */
static void second_of_fix(void)
{
    nmea("GPGGA,184353.07,3723.2475,N,12158.3416,W,1,07,1.0,9.0,M,-28.0,M,,0000");
    nmea("GPGSA,A,3,07,02,26,27,09,04,15,,,,,,1.8,1.0,1.5");
    nmea("GPGSV,2,1,07,07,79,048,42,02,51,062,43,26,36,256,42,27,27,138,42");
    nmea("GPGSV,2,2,07,09,23,313,42,04,19,159,41,15,12,041,");
    nmea("GPRMC,184353.07,A,3723.2475,N,12158.3416,W,0.13,309.62,120598,,");
}

static void pump(unsigned ticks) { for (unsigned i = 0; i < ticks; i++) { gps_poll(); advance(10000); } }

static void driver_tests(void)
{
    /* 4800 NMEA: locked on the first rate. */
    talk_divisor = 24;
    gps_insert(0x200, 1);
    assert(configured && configured->divisor == 24 && configured->cor == 0x41 && configured->io_base == 0x3f8);
    second_of_fix();
    pump(5);
    assert(state == GPS_STREAMING && baud() == 4800);
    struct gps_fix fix;
    assert(gps_read(&fix));
    assert(fix.state == GPS_STREAMING && fix.baud == 4800);
    /* 37 deg 23.2475' N, 121 deg 58.3416' W */
    assert(fix.lat == 373874583 && fix.lon == -1219723600);
    assert(fix.fix == GPS_FIX_3D && fix.sats_used == 7 && fix.hdop_c == 100);
    assert(fix.altitude_dm == 90 && fix.sats_in_view == 7);
    assert(fix.year == 1998 && fix.month == 5 && fix.day == 12 && fix.hour == 18 && fix.minute == 43 && fix.second == 53);
    assert(fix.course_valid && fix.course_cdeg == 30962);
    assert(fix.speed_cms == 6);    /* 0.13 kn */
    assert(fix.sats[0].prn == 7 && fix.sats[0].elevation == 79 && fix.sats[0].azimuth == 48 &&
           fix.sats[0].snr == 42 && fix.sats[0].used);
    assert(fix.sats[6].prn == 15 && fix.sats[6].snr == -1 && fix.sats[6].used);
    assert(fix.age_ms <= 60);
    /* Commands go out with $, checksum and CR LF. */
    assert(gps_command("PSRF103,00,00,01,01") == 25);
    assert(!memcmp(sent, "$PSRF103,00,00,01,01*25\r\n", 25));
    assert(gps_command("bad*") == GZ_EINVAL);
    /* A bad checksum is counted and ignored. */
    uint32_t seq = sequence;
    feed("$GPRMC,1,V,,,,,,,,,*FF\r\n");
    pump(1);
    assert(sequence == seq && checksum_errors == 1);
    /* Silence: back to searching, then found again. */
    pump(600);
    assert(state == GPS_SEARCHING);
    second_of_fix(); pump(5);
    assert(state == GPS_STREAMING);
    /* Removal. */
    gps_removed();
    assert(gz_gps_io(GPS_IO_SNAPSHOT, user, sizeof(struct gps_snapshot)) == 0 &&
           ((struct gps_snapshot *)user)->state == GPS_ABSENT);
    assert(!gps_read(&fix) && fix.state == GPS_ABSENT);

    /* The right rate, switched to mid-character: a framing error or two at
     * first doesn't send the search on to the next rate (hardware, 2026-11-06). */
    stream_len = stream_pos = 0;
    talk_divisor = 24;
    gps_insert(0x200, 1);
    glitches = 2;
    second_of_fix(); pump(3);
    assert(configured->divisor == 24 && state == GPS_SEARCHING);
    /* Each re-arm drops what was buffered, so the second glitch costs a second too. */
    second_of_fix(); pump(3);
    second_of_fix(); pump(3);
    assert(state == GPS_STREAMING && baud() == 4800);
    /* A glitch while streaming re-arms the UART at the same rate. */
    glitches = 1;
    second_of_fix(); pump(3);
    second_of_fix(); pump(3);
    assert(state == GPS_STREAMING && baud() == 4800 && configured->divisor == 24);
    gps_removed();

    /* 9600: the 4800 window fails on framing errors, 9600 locks. */
    stream_len = stream_pos = 0;
    talk_divisor = 12;
    gps_insert(0x200, 1);
    for (unsigned i = 0; i < 8 && configured->divisor != 12; i++) { second_of_fix(); pump(1); }
    if (configured->divisor != 12) pump(260);
    assert(state == GPS_SEARCHING && configured->divisor == 12);
    second_of_fix();
    pump(3);
    assert(state == GPS_STREAMING && baud() == 9600);
    gps_removed();

    /* SiRF binary at 38400: switched to NMEA at 4800. */
    stream_len = stream_pos = 0;
    talk_divisor = 3;
    gps_insert(0x200, 1);
    static const uint8_t mid4[] = { 0xA0, 0xA2, 0x00, 0x02, 0x04, 0x00, 0x00, 0x04, 0xB0, 0xB3 };
    for (unsigned i = 0; i < 6 && configured->divisor != 3; i++) {
        feed_bytes(mid4, sizeof(mid4)); pump(1);
        if (configured->divisor != 3) { stream_pos = stream_len; pump(260); }
    }
    assert(configured->divisor == 3);
    feed_bytes(mid4, sizeof(mid4)); pump(1);
    assert(sent_len == 32 && sent[0] == 0xA0 && sent[1] == 0xA2 && sent[4] == 0x81 && sent[26] == 0x12 &&
           sent[27] == 0xC0 && sent[30] == 0xB0 && sent[31] == 0xB3);
    unsigned sum = 0;
    for (unsigned i = 4; i < 28; i++) sum += sent[i];
    assert(sent[28] == ((sum & 0x7fff) >> 8) && sent[29] == (sum & 0xff));
    talk_divisor = 24;
    pump(25);
    assert(configured->divisor == 24);
    second_of_fix(); pump(3);
    assert(state == GPS_STREAMING && baud() == 4800);
    gps_removed();

    /* Not a GPS receiver: silent at every rate, let go after PASSES passes. */
    stream_len = stream_pos = 0;
    unsigned before = unloaded;
    gps_insert(0x200, 1);
    pump(PASSES * RATES * 260);
    assert(state == GPS_ABSENT && !driver && unloaded > before);
    gps_removed();
}

static void parse_tests(void)
{
    assert(gps_checksum_ok("$GPRMC,184353.07,A,3723.2475,N,12158.3416,W,0.13,309.62,120598,,*2B"));
    assert(gps_checksum_ok("$GPRMC,184353.07,A,3723.2475,N,12158.3416,W,0.13,309.62,120598,,*2b\r\n"));
    assert(!gps_checksum_ok("$GPRMC,184353.07,A,3723.2475,N,12158.3416,W,0.13,309.62,120598,,*2C"));
    assert(!gps_checksum_ok("GPRMC,*00") && !gps_checksum_ok("$GPRMC") && !gps_checksum_ok("$GP*0") && !gps_checksum_ok(""));

    struct gps_snapshot s;
    memset(&s, 0, sizeof(s));
    s.version = GPS_SNAPSHOT_VERSION; s.size = sizeof(s); s.state = GPS_STREAMING;
    struct gps_fix f;
    /* No fix yet: void RMC with empty fields, time still read. */
    strcpy(s.rmc, "$GPRMC,000104.00,V,,,,,,,060180,,*00");
    uint8_t x = 0; for (char *p = s.rmc + 1; *p != '*'; p++) x ^= (uint8_t)*p;
    snprintf(strchr(s.rmc, '*'), 4, "*%02X", x);
    assert(!gps_parse(&s, &f) && f.fix == GPS_FIX_NONE && f.hour == 0 && f.minute == 1 && f.year == 1980);
    /* Southern and eastern hemispheres (Sydney Opera House). */
    strcpy(s.rmc, "$GPRMC,010203,A,3351.4140,S,15112.9200,E,,,010126,,");
    x = 0; for (char *p = s.rmc + 1; *p; p++) x ^= (uint8_t)*p;
    snprintf(s.rmc + strlen(s.rmc), 4, "*%02X", x);
    assert(gps_parse(&s, &f) && f.lat == -338569000 && f.lon == 1512153333 && !f.course_valid && f.year == 2026);
    /* Unterminated slots are refused. */
    memset(s.gga, 'x', sizeof(s.gga));
    assert(!gps_parse(&s, &f));
    s.gga[0] = 0;
    /* Malformed minutes. */
    strcpy(s.rmc, "$GPRMC,010203,A,3375.0000,S,15112.9200,E,,,010126,,");
    x = 0; for (char *p = s.rmc + 1; *p; p++) x ^= (uint8_t)*p;
    snprintf(s.rmc + strlen(s.rmc), 4, "*%02X", x);
    assert(!gps_parse(&s, &f));
}

static void mercator_tests(void)
{
    /* Against double-precision Web Mercator, at many latitudes. */
    double worst = 0;
    for (int64_t lat = -850511287; lat <= 850511287; lat += 1234567) {
        for (int64_t lon = -1800000000; lon < 1800000000; lon += 777777777) {
            uint32_t wx, wy;
            gps_project((int32_t)lat, (int32_t)lon, &wx, &wy);
            double phi = lat * 1e-7 * M_PI / 180;
            double ex = (lon * 1e-7 + 180) / 360 * 4294967296.0;
            double ey = (1 - log(tan(M_PI / 4 + phi / 2)) / M_PI) / 2 * 4294967296.0;
            double ex_err = fabs(wx - ex), ey_err = fabs(wy - ey);
            assert(ex_err <= 1.0);
            if (ey_err > worst) worst = ey_err;
        }
    }
    /* World units are 2^-24 of a zoom-0 pixel... zoom 18 pixel = 64 units. */
    printf("mercator: worst y error %.1f world units (%.2f px at z18)\n", worst, worst / 64);
    assert(worst < 64);
    uint32_t wx, wy;
    gps_project(0, 0, &wx, &wy);
    assert(wx == 0x80000000u && wy == 0x80000000u);
    gps_project(900000000, 0, &wx, &wy);
    assert(wy < 64);   /* the clamp, within a z18 pixel of the top */
    /* Distances: 1 degree of latitude ~ 111.2 km; London-Paris ~ 343 km. */
    assert(gps_distance_m(0, 0, 10000000, 0) == 111195);
    uint32_t d = gps_distance_m(515074000, -1278000, 488566000, 23522000);
    assert(d > 340000 && d < 346000);
    assert(gps_distance_m(373874583, -1219723600, 373874583, -1219723600) == 0);
    uint32_t short_d = gps_distance_m(373874583, -1219723600, 373883583, -1219723600);
    assert(short_d >= 99 && short_d <= 101);   /* 0.0009 deg = 100 m */
    /* Ground resolution at the equator, zoom 0: 156543.03 m/px. */
    assert(gps_mm_per_pixel(0, 0) == 156543033);
    assert(gps_mm_per_pixel(600000000, 10) > 76000 && gps_mm_per_pixel(600000000, 10) < 76500);
}

int main(void)
{
    parse_tests();
    mercator_tests();
    driver_tests();
    puts("gps-test: ok");
    return 0;
}
