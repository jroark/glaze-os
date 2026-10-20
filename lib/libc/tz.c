/* Time zones and daylight saving rules (glaze/tz.h). */

#include <glaze/notify.h>
#include <glaze/prefs.h>
#include <glaze/tz.h>

static const struct gz_tz zones[] = {
    { "UTC", 0, TZ_NONE },
    { "Honolulu", -600, TZ_NONE },
    { "Anchorage", -540, TZ_US },
    { "Los Angeles", -480, TZ_US },
    { "Phoenix", -420, TZ_NONE },
    { "Denver", -420, TZ_US },
    { "Chicago", -360, TZ_US },
    { "New York", -300, TZ_US },
    { "S\xe3o Paulo", -180, TZ_NONE },
    { "London", 0, TZ_EU },
    { "Paris", 60, TZ_EU },
    { "Moscow", 180, TZ_NONE },
    { "Dubai", 240, TZ_NONE },
    { "Kolkata", 330, TZ_NONE },
    { "Beijing", 480, TZ_NONE },
    { "Tokyo", 540, TZ_NONE },
    { "Sydney", 600, TZ_AU },
};
#define NZONES  ((int)(sizeof(zones) / sizeof(zones[0])))

int tz_count(void)
{
    return NZONES;
}

const struct gz_tz *tz_get(int i)
{
    return i >= 0 && i < NZONES ? &zones[i] : NULL;
}

int tz_current(void)
{
    int32_t z;

    if (pref_get(TZ_PREF_ZONE, &z, sizeof(z)) != sizeof(z) || z < 0 || z >= NZONES)
        return TZ_LOS_ANGELES;
    return z;
}

/* Day number (glaze/notify.h) of the nth Sunday of a month, counting from
 * 1; n = 0 is the month's last Sunday. */
static int32_t sunday(int year, int month, int n)
{
    static const uint8_t mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (n == 0) {
        int32_t last = gz_days_from_civil(year, month, mdays[month - 1]);
        return last - (last + 6) % 7;               /* 2000-01-01 was a Saturday */
    }
    int32_t first = gz_days_from_civil(year, month, 1);
    return first + (7 - (first + 6) % 7) % 7 + (n - 1) * 7;
}

static uint32_t at(int32_t day, int hour, int offset_min)
{
    return (uint32_t)day * 86400u + (uint32_t)(hour * 3600 - offset_min * 60);
}

int tz_offset_min(int zone, uint32_t utc)
{
    const struct gz_tz *z = tz_get(zone);
    struct gz_datetime dt;

    if (!z)
        return 0;
    int std = z->std_min;
    gz_secs_to_dt(utc + (uint32_t)(std * 60), &dt);
    int y = dt.year;
    bool dst = false;

    switch (z->rule) {
    case TZ_US:
        dst = utc >= at(sunday(y, 3, 2), 2, std) && utc < at(sunday(y, 11, 1), 2, std + 60);
        break;
    case TZ_EU:
        dst = utc >= at(sunday(y, 3, 0), 1, 0) && utc < at(sunday(y, 10, 0), 1, 0);
        break;
    case TZ_AU:     /* southern hemisphere: on from October into April */
        dst = utc >= at(sunday(y, 10, 1), 2, std) || utc < at(sunday(y, 4, 1), 3, std + 60);
        break;
    }
    return std + (dst ? 60 : 0);
}

uint32_t tz_utc_to_local(int zone, uint32_t utc)
{
    return utc + (uint32_t)(tz_offset_min(zone, utc) * 60);
}

uint32_t tz_local_to_utc(int zone, uint32_t local)
{
    const struct gz_tz *z = tz_get(zone);
    uint32_t utc = local - (uint32_t)((z ? z->std_min : 0) * 60);

    return local - (uint32_t)(tz_offset_min(zone, utc) * 60);
}
