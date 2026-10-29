/*
 * Wall-clock seconds for notification times: seconds since 2000-01-01
 * 00:00 in the RTC's local time (it keeps no time zone). Valid 2000-2135.
 */
#ifndef GLAZE_NOTIFY_H
#define GLAZE_NOTIFY_H

#include <glaze/abi.h>

/* Days from 2000-01-01 to y-m-d (Hinnant's days_from_civil, shifted). */
static inline int32_t gz_days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 730425;
}

static inline uint32_t gz_dt_to_secs(const struct gz_datetime *dt)
{
    int32_t days = gz_days_from_civil(dt->year, dt->month, dt->day);
    return (uint32_t)days * 86400u + dt->hour * 3600u + dt->minute * 60u + dt->second;
}

static inline void gz_secs_to_dt(uint32_t secs, struct gz_datetime *dt)
{
    int32_t z = (int32_t)(secs / 86400u) + 730425;
    uint32_t rem = secs % 86400u;
    int era = z / 146097;
    int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    int m = mp + (mp < 10 ? 3 : -9);

    dt->year = (uint16_t)(yoe + era * 400 + (m <= 2));
    dt->month = (uint8_t)m;
    dt->day = (uint8_t)(doy - (153 * mp + 2) / 5 + 1);
    dt->hour = (uint8_t)(rem / 3600);
    dt->minute = (uint8_t)(rem / 60 % 60);
    dt->second = (uint8_t)(rem % 60);
    dt->weekday = (uint8_t)((secs / 86400u + 6) % 7);     /* 2000-01-01 was a Saturday */
}

/* Requests to SpringBoard from processes other than the front app (servers),
 * for the app whose romfs binary is `path`. 0 or a negative GZ_E* code. */
/* Launch a supported CE application directly from the read-only /NAND mount. */
int sb_wince_launch(const char *path);
int sb_badge_set_for(const char *path, int count);
int sb_note_post_for(const char *path, uint32_t id, uint32_t when, const char *title, const char *body,
                     int badge);
int sb_note_cancel_for(const char *path, uint32_t id);

#endif
