/*
 * Time zones (libc). The RTC keeps local time, so a zone is what turns it
 * into UTC and back: NTP sets the clock through it, and Clock's world
 * clocks compare zones with it. Times are glaze/notify.h seconds (since
 * 2000-01-01 00:00); UTC ones are the same count in UTC.
 *
 * A zone is a city with a standard offset and a daylight saving rule.
 * Settings keeps the chosen one in TZ_PREF_ZONE, an index into the table.
 */
#ifndef GLAZE_TZ_H
#define GLAZE_TZ_H

#include <glaze/types.h>

#define TZ_PREF_ZONE    "set.zone"      /* int32 zone index */
#define TZ_PREF_AUTO    "set.ntp"       /* int32: set the clock by NTP (default on) */
#define TZ_PREF_SYNCED  "set.ntp_last"  /* uint32: local time of the last NTP sync */

enum {
    TZ_NONE,
    TZ_US,      /* 2nd Sunday of March 02:00 to 1st Sunday of November 02:00, local */
    TZ_EU,      /* last Sunday of March to last Sunday of October, 01:00 UTC */
    TZ_AU,      /* 1st Sunday of October 02:00 to 1st Sunday of April 03:00, local */
};

struct gz_tz {
    const char *name;
    int16_t std_min;            /* standard offset from UTC, minutes */
    uint8_t rule;
};

#define TZ_LOS_ANGELES  3       /* the default zone */

int tz_count(void);
/* NULL if i is out of range. */
const struct gz_tz *tz_get(int i);
/* The zone chosen in Settings, or TZ_LOS_ANGELES. */
int tz_current(void);
/* Minutes from UTC in the zone at a UTC time, daylight saving included. */
int tz_offset_min(int zone, uint32_t utc);
uint32_t tz_utc_to_local(int zone, uint32_t utc);
/* Around a daylight saving change, a local time that repeats maps to the
 * later instant and one that is skipped to an hour earlier. */
uint32_t tz_local_to_utc(int zone, uint32_t local);

#endif
