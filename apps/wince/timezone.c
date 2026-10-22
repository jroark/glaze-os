/* CE time-zone layouts over Glaze's selected zone and daylight rules. */
#include "runtime.h"
#include "datetime.h"
#include <glaze/notify.h>
#include <glaze/tz.h>

CE_API(GetTimeZoneInformation)
{
    if (!a0) {
        ce_error = 87;
        return (CW)-1;
    }
    int zone = tz_current();
    const struct gz_tz *z = tz_get(zone);
    uint8_t *out = PTR(uint8_t, a0);
    memset(out, 0, 172);
    ce_put32(out, (CW)-z->std_min);
    ce_widen((WCHAR *)(out + 4), 32, z->name);
    ce_widen((WCHAR *)(out + 88), 32, z->name);
    if (z->rule == TZ_NONE)
        return 0;
    uint16_t standard[8] = {0}, daylight[8] = {0};
    if (z->rule == TZ_US) {
        standard[1] = 11; standard[3] = 1; standard[4] = 2;
        daylight[1] = 3; daylight[3] = 2; daylight[4] = 2;
    } else if (z->rule == TZ_EU) {
        standard[1] = 10; standard[3] = 5; standard[4] = 2 + z->std_min / 60;
        daylight[1] = 3; daylight[3] = 5; daylight[4] = 1 + z->std_min / 60;
    } else if (z->rule == TZ_AU) {
        standard[1] = 4; standard[3] = 1; standard[4] = 3;
        daylight[1] = 10; daylight[3] = 1; daylight[4] = 2;
    }
    memcpy(out + 68, standard, sizeof(standard));
    memcpy(out + 152, daylight, sizeof(daylight));
    ce_put32(out + 168, (CW)-60);
    struct gz_datetime now;
    if (gz_rtc_get(&now)) {
        ce_error = 31;
        return (CW)-1;
    }
    uint32_t utc = tz_local_to_utc(zone, gz_dt_to_secs(&now));
    return tz_offset_min(zone, utc) == z->std_min ? 1 : 2;
}
static CW convert_file_time(CW input, CW output, bool to_local)
{
    if (!input || !output) {
        ce_error = 87;
        return 0;
    }
    const uint64_t epoch = 125911584000000000ull;
    const uint64_t second = 10000000u;
    uint64_t ticks = ce_u32(PTR(void, input)) | (uint64_t)ce_u32(PTR(uint8_t, input) + 4) << 32;
    int zone = tz_current();
    int32_t delta = tz_get(zone)->std_min * 60;
    if (ticks >= epoch && (ticks - epoch) / second <= UINT32_MAX) {
        uint32_t seconds = (uint32_t)((ticks - epoch) / second);
        delta = to_local ? tz_offset_min(zone, seconds) * 60 :
            (int32_t)(tz_local_to_utc(zone, seconds) - seconds);
    } else if (!to_local)
        delta = -delta;
    int64_t adjustment = (int64_t)delta * (int64_t)second;
    if ((adjustment < 0 && ticks < (uint64_t)-adjustment) ||
        (adjustment > 0 && ticks > UINT64_MAX - (uint64_t)adjustment)) {
        ce_error = 87;
        return 0;
    }
    uint64_t result = ticks + adjustment;
    ce_put32(PTR(void, output), (CW)result);
    ce_put32(PTR(uint8_t, output) + 4, (CW)(result >> 32));
    return 1;
}
CE_API(FileTimeToLocalFileTime) { return convert_file_time(a0, a1, true); }
CE_API(LocalFileTimeToFileTime) { return convert_file_time(a0, a1, false); }
void *ce_resolve_timezone(const char *name)
{
    static const struct ce_export exports[] = {CE_EXPORT(GetTimeZoneInformation),
        CE_EXPORT(FileTimeToLocalFileTime), CE_EXPORT(LocalFileTimeToFileTime)};
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return NULL;
}
