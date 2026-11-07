/* GPS: parsing the kernel's NMEA-0183 snapshot (glaze/gps.h) and integer
 * Web Mercator. Field layouts follow NMEA-0183 v2.3 as SiRF receivers
 * emit them (SiRF NMEA Reference Manual: GGA, GSA, GSV, RMC).
 */
#include <glaze/gps.h>
#include <glaze/sys.h>
#include <string.h>
#include "gps_tables.h"

static int hexdigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool gps_checksum_ok(const char *s)
{
    if (!s || s[0] != '$') return false;
    uint8_t x = 0;
    const char *p = s + 1;
    while (*p && *p != '*') x ^= (uint8_t)*p++;
    if (*p != '*') return false;
    int hi = hexdigit(p[1]), lo = hexdigit(p[2]);
    if (hi < 0 || lo < 0) return false;
    p += 3;
    while (*p == '\r' || *p == '\n') p++;
    return !*p && x == (uint8_t)(hi << 4 | lo);
}

/* Splits a sentence into its comma fields in place (in a copy), dropping
 * the checksum. f[0] is "$GPRMC". Returns the field count. */
#define FIELDS_MAX 24
static int split(const char *s, char *buf, const char *f[FIELDS_MAX])
{
    if (!gps_checksum_ok(s)) return 0;
    strlcpy(buf, s, GPS_SENTENCE_MAX);
    char *star = strchr(buf, '*');
    if (star) *star = 0;
    int n = 0;
    char *p = buf;
    f[n++] = p;
    while ((p = strchr(p, ',')) && n < FIELDS_MAX) {
        *p++ = 0;
        f[n++] = p;
    }
    return n;
}

static const char *field(const char *const *f, int n, int i) { return i < n ? f[i] : ""; }

/* A decimal with up to `places` fractional digits, scaled by 10^places.
 * Sets *ok false for an empty field. */
static int64_t decimal(const char *s, unsigned places, bool *ok)
{
    bool neg = *s == '-';
    if (neg || *s == '+') s++;
    if (!*s) { if (ok) *ok = false; return 0; }
    int64_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    unsigned got = 0;
    if (*s == '.')
        for (s++; *s >= '0' && *s <= '9'; s++)
            if (got < places) { v = v * 10 + (*s - '0'); got++; }
    for (; got < places; got++) v *= 10;
    if (ok) *ok = true;
    return neg ? -v : v;
}

static unsigned two(const char *s) { return (unsigned)(s[0] - '0') * 10 + (unsigned)(s[1] - '0'); }
static bool digits(const char *s, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

/* "ddmm.mmmm" or "dddmm.mmmm" plus a hemisphere letter, to 1e-7 degrees. */
static bool coordinate(const char *v, const char *hemi, int32_t *out)
{
    bool ok;
    int64_t x = decimal(v, 5, &ok);        /* ddmm x 1e5 */
    if (!ok || x < 0) return false;
    int64_t deg = x / 10000000, min_e5 = x % 10000000;
    if (min_e5 >= 6000000) return false;
    int64_t e7 = deg * 10000000 + min_e5 * 10 / 6;   /* minutes/60 in 1e-7 */
    if (*hemi == 'S' || *hemi == 'W') e7 = -e7;
    else if (*hemi != 'N' && *hemi != 'E') return false;
    *out = (int32_t)e7;
    return true;
}

static void parse_rmc(const char *s, struct gps_fix *fix)
{
    char buf[GPS_SENTENCE_MAX];
    const char *f[FIELDS_MAX];
    int n = split(s, buf, f);
    if (n < 10) return;
    /* 1 time hhmmss.sss, 2 status A/V, 3-4 lat, 5-6 lon, 7 knots, 8 course, 9 ddmmyy */
    const char *t = field(f, n, 1), *d = field(f, n, 9);
    if (strlen(t) >= 6 && digits(t, 6)) {
        fix->hour = (uint8_t)two(t); fix->minute = (uint8_t)two(t + 2); fix->second = (uint8_t)two(t + 4);
    }
    if (strlen(d) == 6 && digits(d, 6)) {
        fix->day = (uint8_t)two(d); fix->month = (uint8_t)two(d + 2);
        fix->year = (uint16_t)(2000 + two(d + 4));
        if (fix->year >= 2080) fix->year -= 100;
    }
    if (*field(f, n, 2) != 'A') return;
    int32_t lat, lon;
    if (!coordinate(field(f, n, 3), field(f, n, 4), &lat) ||
        !coordinate(field(f, n, 5), field(f, n, 6), &lon))
        return;
    fix->lat = lat; fix->lon = lon;
    if (fix->fix == GPS_FIX_NONE) fix->fix = GPS_FIX_2D;
    bool ok;
    int64_t knots_e3 = decimal(field(f, n, 7), 3, &ok);
    if (ok && knots_e3 >= 0) fix->speed_cms = (uint32_t)(knots_e3 * 1852 / 36000); /* 1 kn = 1852/3600 m/s */
    int64_t course = decimal(field(f, n, 8), 2, &ok);
    if (ok && course >= 0 && course < 36000) { fix->course_cdeg = (uint32_t)course; fix->course_valid = true; }
}

static void parse_gga(const char *s, struct gps_fix *fix)
{
    char buf[GPS_SENTENCE_MAX];
    const char *f[FIELDS_MAX];
    int n = split(s, buf, f);
    if (n < 10) return;
    /* 6 quality, 7 satellites used, 8 HDOP, 9 altitude, 10 M */
    if (*field(f, n, 6) == '0' || !*field(f, n, 6)) return;
    bool ok;
    int64_t used = decimal(field(f, n, 7), 0, &ok);
    if (ok && used >= 0) fix->sats_used = (unsigned)used;
    int64_t hdop = decimal(field(f, n, 8), 2, &ok);
    if (ok && hdop > 0) fix->hdop_c = (uint32_t)hdop;
    int64_t alt = decimal(field(f, n, 9), 1, &ok);
    if (ok) fix->altitude_dm = (int32_t)alt;
}

static void parse_gsa(const char *s, struct gps_fix *fix, uint8_t used[12], unsigned *nused)
{
    char buf[GPS_SENTENCE_MAX];
    const char *f[FIELDS_MAX];
    int n = split(s, buf, f);
    if (n < 18) return;
    /* 1 mode A/M, 2 fix 1/2/3, 3-14 PRNs, 15 PDOP, 16 HDOP, 17 VDOP */
    char mode = *field(f, n, 2);
    if (mode == '3') fix->fix = GPS_FIX_3D;
    else if (mode == '2' && fix->fix == GPS_FIX_NONE) fix->fix = GPS_FIX_2D;
    for (int i = 3; i <= 14; i++) {
        bool ok;
        int64_t prn = decimal(field(f, n, i), 0, &ok);
        if (ok && prn > 0 && prn < 256 && *nused < 12) used[(*nused)++] = (uint8_t)prn;
    }
    if (!fix->hdop_c) {
        bool ok;
        int64_t hdop = decimal(field(f, n, 16), 2, &ok);
        if (ok && hdop > 0) fix->hdop_c = (uint32_t)hdop;
    }
}

static void parse_gsv(const char *s, struct gps_fix *fix)
{
    char buf[GPS_SENTENCE_MAX];
    const char *f[FIELDS_MAX];
    int n = split(s, buf, f);
    if (n < 4) return;
    bool ok;
    int64_t view = decimal(field(f, n, 3), 0, &ok);
    if (ok && view >= 0) fix->sats_in_view = (unsigned)view;
    /* Groups of four: PRN, elevation, azimuth, SNR (empty when not tracked). */
    for (int i = 4; i < n; i += 4) {
        int64_t prn = decimal(field(f, n, i), 0, &ok);
        if (!ok || prn <= 0 || prn > 255) continue;
        unsigned k = 0;
        while (k < GPS_SATS_MAX && fix->sats[k].prn && fix->sats[k].prn != prn) k++;
        if (k == GPS_SATS_MAX) return;
        struct gps_satellite *sat = &fix->sats[k];
        sat->prn = (uint8_t)prn;
        int64_t el = decimal(field(f, n, i + 1), 0, &ok);
        sat->elevation = ok && el >= 0 && el <= 90 ? (uint8_t)el : 0;
        int64_t az = decimal(field(f, n, i + 2), 0, &ok);
        sat->azimuth = ok && az >= 0 && az < 360 ? (uint16_t)az : 0;
        int64_t snr = decimal(field(f, n, i + 3), 0, &ok);
        sat->snr = ok && snr >= 0 && snr <= 99 ? (int8_t)snr : -1;
    }
}

bool gps_parse(const struct gps_snapshot *snap, struct gps_fix *fix)
{
    memset(fix, 0, sizeof(*fix));
    fix->age_ms = GPS_AGE_NEVER;
    if (!snap || snap->version != GPS_SNAPSHOT_VERSION || snap->size != sizeof(*snap)) return false;
    fix->state = snap->state;
    fix->baud = snap->baud;
    fix->sequence = snap->sequence;
    fix->age_ms = snap->rmc_age;
    /* Sentences must be NUL-terminated within their slots. */
    if (memchr(snap->rmc, 0, GPS_SENTENCE_MAX) == NULL || memchr(snap->gga, 0, GPS_SENTENCE_MAX) == NULL ||
        memchr(snap->gsa, 0, GPS_SENTENCE_MAX) == NULL)
        return false;
    unsigned gsv_count = MIN(snap->gsv_count, GPS_GSV_MAX);
    for (unsigned i = 0; i < gsv_count; i++)
        if (memchr(snap->gsv[i], 0, GPS_SENTENCE_MAX) != NULL) parse_gsv(snap->gsv[i], fix);
    uint8_t used[12];
    unsigned nused = 0;
    parse_gsa(snap->gsa, fix, used, &nused);
    for (unsigned k = 0; k < GPS_SATS_MAX && fix->sats[k].prn; k++)
        for (unsigned j = 0; j < nused; j++)
            if (fix->sats[k].prn == used[j]) fix->sats[k].used = true;
    unsigned mode = fix->fix;
    fix->fix = GPS_FIX_NONE;
    parse_rmc(snap->rmc, fix);
    if (fix->fix == GPS_FIX_NONE) {
        /* RMC void: no position, whatever GSA claims. */
        fix->lat = fix->lon = 0;
        fix->speed_cms = 0; fix->course_valid = false;
        return false;
    }
    if (mode == GPS_FIX_3D) fix->fix = GPS_FIX_3D;
    parse_gga(snap->gga, fix);
    if (!fix->sats_used) fix->sats_used = nused;
    return true;
}

bool gps_read(struct gps_fix *fix)
{
    static struct gps_snapshot snap;
    if (gz_gps_io(GPS_IO_SNAPSHOT, &snap, sizeof(snap)) < 0) {
        memset(fix, 0, sizeof(*fix));
        fix->state = GPS_ABSENT;
        fix->age_ms = GPS_AGE_NEVER;
        return false;
    }
    return gps_parse(&snap, fix);
}

int gps_command(const char *body)
{
    static const char hexchars[] = "0123456789ABCDEF";
    char out[GPS_IO_WRITE_MAX];
    size_t n = strlen(body);
    if (!n || n + 6 > sizeof(out)) return GZ_EINVAL;
    uint8_t x = 0;
    out[0] = '$';
    for (size_t i = 0; i < n; i++) {
        if (body[i] == '$' || body[i] == '*' || body[i] < 32 || body[i] > 126) return GZ_EINVAL;
        out[1 + i] = body[i];
        x ^= (uint8_t)body[i];
    }
    out[1 + n] = '*';
    out[2 + n] = hexchars[x >> 4];
    out[3 + n] = hexchars[x & 15];
    out[4 + n] = '\r';
    out[5 + n] = '\n';
    return gz_gps_io(GPS_IO_WRITE, out, (uint32_t)(n + 6));
}

#define LAT_LIMIT_E7 850511287      /* atan(sinh(pi)): Web Mercator's square */

/* Mercator y for |lat|, in world units (2^31 = pi). Cubic Hermite between
 * the table's quarter-degree samples and their derivatives, t in Q24. */
static int64_t mercator_y(uint32_t lat)
{
    uint32_t i = lat / MERC_STEP_E7;
    int64_t t = (int64_t)(lat % MERC_STEP_E7) * (1 << 24) / MERC_STEP_E7;
    int64_t t2 = t * t >> 24, t3 = t2 * t >> 24;
    int64_t h00 = 2 * t3 - 3 * t2 + (1 << 24), h10 = t3 - 2 * t2 + t;
    int64_t h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
    int64_t y = h00 * MERC_Y[i] + h10 * MERC_D[i] + h01 * MERC_Y[i + 1] + h11 * MERC_D[i + 1];
    return (y + (1 << 23)) >> 24;
}

void gps_project(int32_t lat, int32_t lon, uint32_t *wx, uint32_t *wy)
{
    if (lat > LAT_LIMIT_E7) lat = LAT_LIMIT_E7;
    if (lat < -LAT_LIMIT_E7) lat = -LAT_LIMIT_E7;
    /* x: (lon + 180) / 360 of 2^32; 1800000000 fits in int32 but the sum doesn't. */
    int64_t x = ((int64_t)lon + 1800000000) * 1073741824LL / 900000000LL;
    if (x < 0) x = 0;
    if (x > 0xFFFFFFFFLL) x = 0xFFFFFFFFLL;
    *wx = (uint32_t)x;
    int64_t y = mercator_y((uint32_t)(lat < 0 ? -lat : lat));
    int64_t v = lat < 0 ? 2147483648LL + y : 2147483648LL - y;
    if (v < 0) v = 0;
    if (v > 0xFFFFFFFFLL) v = 0xFFFFFFFFLL;
    *wy = (uint32_t)v;
}

/* cos(|lat|) in Q16, linear between whole degrees. */
static uint32_t cos_q16(int32_t lat)
{
    uint32_t a = (uint32_t)(lat < 0 ? -(int64_t)lat : lat);
    if (a >= 900000000) return 0;
    uint32_t i = a / 10000000, r = a % 10000000;
    return (uint32_t)(COS_Q16[i] - ((int64_t)(COS_Q16[i] - COS_Q16[i + 1]) * r / 10000000));
}

static uint64_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = 1ULL << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

uint32_t gps_distance_m(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2)
{
    int64_t dlat = (int64_t)lat2 - lat1, dlon = (int64_t)lon2 - lon1;
    if (dlon > 1800000000) dlon -= 3600000000LL;
    if (dlon < -1800000000) dlon += 3600000000LL;
    int32_t mid = (int32_t)(((int64_t)lat1 + lat2) / 2);
    int64_t dx = dlon * cos_q16(mid) >> 16;
    /* Beyond ~2000 km the squares overflow; scale down first. */
    unsigned shift = 0;
    while ((dx > 2000000000 || dx < -2000000000 || dlat > 2000000000 || dlat < -2000000000) && shift < 8) {
        dx >>= 1; dlat >>= 1; shift++;
    }
    uint64_t d = isqrt64((uint64_t)(dx * dx) + (uint64_t)(dlat * dlat)) << shift;
    /* Mean Earth radius 6371008.8 m: 1e-7 degree = 0.0111195 m. */
    return (uint32_t)(d * 111195 / 10000000);
}

uint32_t gps_mm_per_pixel(int32_t lat, unsigned zoom)
{
    if (zoom > 24) zoom = 24;
    /* WGS84 equator 40075016.686 m over 256 << zoom pixels. */
    uint64_t mm = (uint64_t)40075016686ULL * cos_q16(lat) >> 16;
    return (uint32_t)(mm / (256ULL << zoom));
}
