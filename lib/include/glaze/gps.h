/* GPS receivers on a CF serial card (docs/hardware/GPS.md).
 *
 * The kernel finds the card's baud rate, collects its NMEA-0183 sentences and
 * keeps the latest of each kind. Any process may read that snapshot, as often
 * as it likes and without consuming it, through gz_gps_io(GPS_IO_SNAPSHOT).
 * gps_read() parses it into a struct gps_fix. Everything is integer: the
 * VR4131 has no FPU and nothing links libgcc's soft-float helpers.
 */
#ifndef GLAZE_GPS_H
#define GLAZE_GPS_H
#include <glaze/types.h>

/* gz_gps_io(op, buffer, size). SNAPSHOT fills a struct gps_snapshot (size
 * must be its size). WRITE sends at most GPS_IO_WRITE_MAX bytes to the
 * receiver, such as a $PSRF NMEA command; GZ_EBUSY if it can't take them
 * yet. GZ_ENOENT: no GPS receiver. */
enum { GPS_IO_SNAPSHOT, GPS_IO_WRITE };
#define GPS_IO_WRITE_MAX 128u

#define GPS_SNAPSHOT_VERSION 1u
#define GPS_SENTENCE_MAX 84u    /* NMEA: 82 characters with CR LF, plus NUL */
#define GPS_GSV_MAX 6u          /* GSV sentences in one cycle: 4 satellites each */

enum {
    GPS_ABSENT,         /* no serial card, or one that isn't a GPS receiver */
    GPS_SEARCHING,      /* a serial card, listening for NMEA at each baud rate */
    GPS_STREAMING,      /* valid NMEA arriving at `baud` */
};

/* The latest valid sentence of each kind, without CR LF, and how many
 * milliseconds ago it arrived (GPS_AGE_NEVER if not yet). */
#define GPS_AGE_NEVER 0xFFFFFFFFu
struct gps_snapshot {
    uint32_t version, size;
    uint32_t state, baud;
    uint32_t generation;    /* a new card, or the receiver found again */
    uint32_t sequence;      /* counts valid sentences */
    uint32_t checksum_errors;
    uint32_t rmc_age, gga_age, gsa_age, gsv_age;
    uint32_t gsv_count;     /* sentences in gsv[] (the latest complete cycle) */
    char rmc[GPS_SENTENCE_MAX], gga[GPS_SENTENCE_MAX], gsa[GPS_SENTENCE_MAX];
    char gsv[GPS_GSV_MAX][GPS_SENTENCE_MAX];
};

/* --- libgps.a: parsing and projection (lib/gps/gps.c) ---
 * A static library, linked into the apps that use it (LDLIBS += -lgps with
 * the SDK), so it costs romfs nothing. */

#define GPS_SATS_MAX 24u
struct gps_satellite {
    uint8_t prn, elevation;     /* degrees */
    uint16_t azimuth;           /* degrees */
    int8_t snr;                 /* dB-Hz, -1 when not tracked */
    bool used;                  /* listed in GSA as part of the fix */
};

enum { GPS_FIX_NONE, GPS_FIX_2D, GPS_FIX_3D };

struct gps_fix {
    uint32_t state, baud, sequence;     /* from the snapshot */
    uint32_t age_ms;            /* of the RMC sentence; GPS_AGE_NEVER if none */
    int fix;                    /* GPS_FIX_*; NONE when RMC says void */
    int32_t lat, lon;           /* 1e-7 degrees, north and east positive */
    int32_t altitude_dm;        /* above mean sea level, decimetres (GGA) */
    uint32_t speed_cms;         /* centimetres a second (RMC knots) */
    uint32_t course_cdeg;       /* true course, hundredths of a degree */
    bool course_valid;
    uint32_t hdop_c;            /* HDOP x100, 0 if unknown */
    unsigned sats_used, sats_in_view;
    /* UTC from RMC; year 0 when unknown. */
    uint16_t year; uint8_t month, day, hour, minute, second;
    struct gps_satellite sats[GPS_SATS_MAX];
};

/* Validates one sentence: "$..*hh", hex checksum over the characters
 * between them. Trailing CR/LF is allowed. */
bool gps_checksum_ok(const char *sentence);

/* Parses a snapshot. Returns true if it holds a position (fix != NONE). */
bool gps_parse(const struct gps_snapshot *snap, struct gps_fix *fix);

/* gz_gps_io(SNAPSHOT) plus gps_parse. Returns false with fix->state
 * GPS_ABSENT when there's no receiver. */
bool gps_read(struct gps_fix *fix);

/* Sends one NMEA command, adding the $, checksum and CR LF: for example
 * gps_command("PSRF103,00,00,01,01") sets GGA to once a second on SiRF. */
int gps_command(const char *body);

/* Web Mercator "world" coordinates: the whole map is 2^32 x 2^32, so a
 * zoom-z map of 256-pixel tiles has pixel (wx >> (24 - z), wy >> (24 - z)).
 * Latitude is clamped to +-85.0511 degrees. */
void gps_project(int32_t lat, int32_t lon, uint32_t *wx, uint32_t *wy);

/* Ground distance in metres between two points, equirectangular: within
 * 0.1 % for a few kilometres, which is what a track's segments are. */
uint32_t gps_distance_m(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2);

/* Metres per pixel at a latitude and zoom, x1000 (for a scale bar). */
uint32_t gps_mm_per_pixel(int32_t lat, unsigned zoom);

#endif
