/* Time services use Glaze's RTC and selected time zone. Locale formatting is
 * an English baseline; unsupported locale settings return explicit errors. */
#include "runtime.h"
#include "datetime.h"
#include <glaze/notify.h>
#include <glaze/tz.h>

static bool locale_24h;
static unsigned locale_first_day = 6; /* LOCALE_IFIRSTDAYOFWEEK: Sunday */
static struct {
    CW type;
    WCHAR value[128];
} locale_formats[] = {{0x1d, {0}}, {0x1e, {0}}, {0x1f, {0}}, {0x20, {0}}, {0x1003, {0}}};
static WCHAR *locale_format(CW type)
{
    for (unsigned i = 0; i < ARRAY_SIZE(locale_formats); i++)
        if (locale_formats[i].type == type) return locale_formats[i].value;
    return NULL;
}

static void system_time(uint16_t out[8], bool utc)
{
    struct gz_datetime dt = {0};
    if (gz_rtc_get(&dt))
        ce_fatal("Cannot read clock");
    if (utc)
        gz_secs_to_dt(tz_local_to_utc(tz_current(), gz_dt_to_secs(&dt)), &dt);
    out[0] = dt.year; out[1] = dt.month; out[2] = dt.weekday; out[3] = dt.day;
    out[4] = dt.hour; out[5] = dt.minute; out[6] = dt.second; out[7] = 0;
}
CE_API(GetSystemTime)
{
    if (a0) system_time(PTR(uint16_t, a0), true);
    return 0;
}
CE_API(SystemTimeToFileTime)
{
    uint64_t ticks;
    if (!a0 || !a1 || !ce_datetime_ticks(PTR(uint16_t, a0), &ticks)) {
        ce_error = 87;
        return 0;
    }
    ce_put32(PTR(void, a1), (uint32_t)ticks);
    ce_put32(PTR(uint8_t, a1) + 4, (uint32_t)(ticks >> 32));
    return 1;
}
CE_API(FileTimeToSystemTime)
{
    if (!a0 || !a1 || !ce_datetime_from_ticks((uint64_t)ce_u32(PTR(void, a0)) |
        (uint64_t)ce_u32(PTR(uint8_t, a0) + 4) << 32, PTR(uint16_t, a1))) {
        ce_error = 87;
        return 0;
    }
    return 1;
}
static CW format_time(bool date, CW flags, CW input, CW format, CW out, CW capacity)
{
    uint16_t now[8], pattern[64];
    if (!input) {
        system_time(now, false);
        input = WORD(now);
    }
    if ((format && flags) || (!date && flags & ~15u) || (date && flags & ~11u)) {
        ce_error = 1004;
        return 0;
    }
    if (!format) {
        const char *default_format;
        if (date)
            default_format = (flags & 2) ? "dddd, MMMM d, yyyy" : (flags & 8) ? "MMMM yyyy" : "M/d/yyyy";
        else if (flags & 1)
            default_format = ((flags & 8) || locale_24h) ? "HH" : (flags & 4) ? "h" : "h tt";
        else if (flags & 2)
            default_format = ((flags & 8) || locale_24h) ? "HH:mm" : (flags & 4) ? "h:mm" : "h:mm tt";
        else
            default_format = ((flags & 8) || locale_24h) ? "HH:mm:ss" : (flags & 4) ? "h:mm:ss" : "h:mm:ss tt";
        ce_widen(pattern, ARRAY_SIZE(pattern), default_format);
        format = WORD(pattern);
        WCHAR *override = locale_format(date ? ((flags & 2) ? 0x20 : 0x1f) : 0x1003);
        if (override && *override && (date ? !(flags & 8) : !flags))
            format = WORD(override);
    }
    CW result = ce_datetime_format(PTR(uint16_t, input), PTR(uint16_t, format), PTR(uint16_t, out), capacity);
    if (!result) ce_error = capacity ? 122 : 87;
    return result;
}
CE_API(GetDateFormatW)
{
    return format_time(true, a1, a2, a3, a4, a5);
}
CE_API(GetTimeFormatW)
{
    return format_time(false, a1, a2, a3, a4, a5);
}
CE_API(GetLocaleInfoW)
{
    /* LOCALE_NOUSEROVERRIDE and LOCALE_USE_CP_ACP change nothing here;
     * LOCALE_RETURN_NUMBER returns a numeric field as a DWORD. */
    bool number = (a1 & 0x20000000u) != 0;
    CW type = a1 & 0x0fffffffu;
    const char *value;
    char text[16], first_day[] = {(char)('0' + locale_first_day), 0};
    WCHAR *override = locale_format(type);
    if (override && *override && !number) {
        CW required = ce_wlen(override) + 1;
        if (!a3) return required;
        if (!a2 || a3 < required) { ce_error = 122; return 0; }
        memcpy(PTR(void, a2), override, required * 2);
        return required;
    }
    switch (type) {
    case 0xc: value = ","; break;               /* SLIST */
    case 0xd: value = "1"; break;               /* IMEASURE: U.S. */
    case 0xe: value = "."; break;
    case 0xf: value = ","; break;               /* STHOUSAND */
    case 0x10: value = "3;0"; break;            /* SGROUPING */
    case 0x11: value = "2"; break;              /* IDIGITS */
    case 0x12: value = "1"; break;              /* ILZERO */
    case 0x14: value = "$"; break;              /* SCURRENCY */
    case 0x1d: value = "/"; break;
    case 0x1e: value = ":"; break;
    case 0x1f: value = "M/d/yyyy"; break;
    case 0x20: value = "dddd, MMMM d, yyyy"; break;
    case 0x21: case 0x22: value = "0"; break; /* month/day/year */
    case 0x23: value = locale_24h ? "1" : "0"; break;
    case 0x24: value = "1"; break;              /* ICENTURY */
    case 0x25: value = locale_24h ? "1" : "0"; break; /* ITLZERO */
    case 0x26: case 0x27: value = "0"; break;   /* IDAYLZERO, IMONLZERO */
    case 0x28: value = "AM"; break;
    case 0x29: value = "PM"; break;
    case 0x1003: value = "h:mm:ss tt"; break;
    case 0x100c: value = first_day; break;
    default:
        /* SDAYNAME1-7 and SABBREVDAYNAME1-7 start on Monday. */
        if (type >= 0x2a && type <= 0x4f) {
            unsigned i = type < 0x31 ? type - 0x2a : type < 0x38 ? type - 0x31
                       : type < 0x44 ? type - 0x38 : type - 0x44;
            value = type < 0x38 ? ce_day_names[(i + 1) % 7] : ce_month_names[i];
            bool abbreviated = (type >= 0x31 && type < 0x38) || type >= 0x44;
            strlcpy(text, value, abbreviated ? 4 : sizeof(text));
            value = text;
            break;
        }
        printf("wince: unsupported locale field %x\n", a1);
        ce_error = 87;
        return 0;
    }
    if (number) {
        if (value[0] < '0' || value[0] > '9' || (value[1] && (value[1] < '0' || value[1] > '9'))) {
            ce_error = 1004;
            return 0;
        }
        if (!a3) return 2;
        if (!a2 || a3 < 2) { ce_error = 122; return 0; }
        CW n = 0;
        for (const char *p = value; *p; p++)
            n = n * 10 + (CW)(*p - '0');
        ce_put32(PTR(void, a2), n);
        return 2;
    }
    CW required = strlen(value) + 1;
    if (!a3) return required;
    if (!a2 || a3 < required) { ce_error = 122; return 0; }
    ce_widen(PTR(WCHAR, a2), a3, value);
    return required;
}
CE_API(SetLocaleInfoW)
{
    if (!a2) { ce_error = 87; return 0; }
    const WCHAR *value = PTR(WCHAR, a2);
    if (a1 == 0x100c && value[0] >= '0' && value[0] <= '6' && !value[1]) {
        locale_first_day = value[0] - '0';
        return 1;
    }
    if (a1 == 0x23 && (value[0] == '0' || value[0] == '1') && !value[1]) {
        locale_24h = value[0] == '1';
        return 1;
    }
    WCHAR *target = locale_format(a1);
    unsigned length = ce_wlen(value);
    if (target && length && length < 128) {
        memcpy(target, value, (length + 1) * 2);
        return 1;
    }
    printf("wince: unsupported locale update %x\n", a1);
    ce_error = 50;
    return 0;
}
CE_API(CeRunAppAtTime)
{
    if (!a0) { ce_error = 87; return 0; }
    /* No deferred app launch is registered by this runtime. Cancelling a
     * launch is therefore complete; scheduling requires a future service. */
    if (!a1) return 1;
    ce_error = 50;
    printf("wince: deferred app launch is unsupported\n");
    return 0;
}
CE_API(SetTimeZoneInformation)
{
    /* Clock calls this on startup. Its private settings must not silently
     * change Glaze's system-wide time-zone preference. */
    ce_error = 50;
    printf("wince: changing the system time zone is unsupported\n");
    return 0;
}
CE_API(SetDaylightTime)
{
    ce_error = 50;
    return 0;
}
CE_API(SetLocalTime)
{
    ce_error = 50;
    return 0;
}
static const struct ce_export exports[] = {
    CE_EXPORT(GetSystemTime), CE_EXPORT(SystemTimeToFileTime), CE_EXPORT(FileTimeToSystemTime),
    CE_EXPORT(GetDateFormatW), CE_EXPORT(GetTimeFormatW), CE_EXPORT(GetLocaleInfoW),
    CE_EXPORT(SetLocaleInfoW),
    CE_EXPORT(CeRunAppAtTime),
    CE_EXPORT(SetTimeZoneInformation),
    CE_EXPORT(SetDaylightTime),
    CE_EXPORT(SetLocalTime),
    {"SetSystemTime", (void *)ce_SetLocalTime},
};
void *ce_resolve_time(const char *name)
{
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name)) return exports[i].address;
    return NULL;
}
