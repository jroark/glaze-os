/* The date picker keeps SYSTEMTIME values in local time. Changes made by the
 * application are silent; user changes emit DTN_DATETIMECHANGE. */
#include "runtime.h"
#include "datetime.h"
#include <glaze/notify.h>

static void date_init(struct ce_window *w)
{
    if (w->date.initialized) return;
    struct gz_datetime now = {0};
    if (gz_rtc_get(&now)) ce_fatal("Cannot read clock");
    uint16_t value[] = {now.year, now.month, now.weekday, now.day,
        now.hour, now.minute, now.second, 0};
    memcpy(w->date.value, value, sizeof(value));
    w->date.initialized = true;
}
static bool date_valid(struct ce_window *w, const uint16_t value[8])
{
    uint64_t ticks, bound;
    if (!ce_datetime_ticks(value, &ticks)) return false;
    if ((w->date.range & 1) && ce_datetime_ticks(w->date.minimum, &bound) && ticks < bound) return false;
    if ((w->date.range & 2) && ce_datetime_ticks(w->date.maximum, &bound) && ticks > bound) return false;
    return true;
}
bool ce_date_message(struct ce_window *w, CW msg, CW wp, CW lp, CW *result)
{
    if (strcasecmp(w->klass, "SysDateTimePick32")) return false;
    date_init(w);
    *result = 0;
    switch (msg) {
    case 0x1001: /* DTM_GETSYSTEMTIME */
        if (w->date.none) *result = 1;
        else if (!lp) *result = (CW)-1;
        else memcpy(PTR(void, lp), w->date.value, 16);
        return true;
    case 0x1002: /* DTM_SETSYSTEMTIME */
        if (wp == 1 && (w->style & 2)) w->date.none = true;
        else if (wp || !lp || !date_valid(w, PTR(uint16_t, lp))) return true;
        else {
            memcpy(w->date.value, PTR(void, lp), 16);
            w->date.none = false;
        }
        w->dirty = true;
        *result = 1;
        return true;
    case 0x1003: /* DTM_GETRANGE */
        if (lp) {
            memcpy(PTR(void, lp), w->date.minimum, 16);
            memcpy(PTR(uint8_t, lp) + 16, w->date.maximum, 16);
        }
        *result = w->date.range;
        return true;
    case 0x1004: { /* DTM_SETRANGE */
        uint64_t low = 0, high = UINT64_MAX;
        if ((wp & ~3u) || (wp && !lp)) return true;
        const uint16_t *values = PTR(uint16_t, lp);
        if ((wp & 1) && !ce_datetime_ticks(values, &low)) return true;
        if ((wp & 2) && !ce_datetime_ticks(values + 8, &high)) return true;
        if (low > high) return true;
        if (wp & 1) memcpy(w->date.minimum, values, 16);
        if (wp & 2) memcpy(w->date.maximum, values + 8, 16);
        w->date.range = wp;
        uint64_t ticks;
        if (ce_datetime_ticks(w->date.value, &ticks)) {
            if (ticks < low) memcpy(w->date.value, w->date.minimum, 16);
            if (ticks > high) memcpy(w->date.value, w->date.maximum, 16);
        }
        w->dirty = true;
        *result = 1;
        return true;
    }
    case 0x1032: /* DTM_SETFORMATW */
        if (lp && ce_wlen(PTR(WCHAR, lp)) >= ARRAY_SIZE(w->date.format)) return true;
        memset(w->date.format, 0, sizeof(w->date.format));
        if (lp) memcpy(w->date.format, PTR(void, lp), ce_wlen(PTR(WCHAR, lp)) * 2);
        w->dirty = true;
        *result = 1;
        return true;
    case 0x100:
        if (wp != 0x26 && wp != 0x28) return false;
        /* The bounded picker uses Up/Down to move one day. */
        if (!w->date.none) {
            uint64_t ticks;
            uint16_t value[8];
            if (!ce_datetime_ticks(w->date.value, &ticks)) return true;
            uint64_t step = UINT64_C(864000000000);
            if (wp == 0x28 && ticks < step) return true;
            if (wp == 0x26 && UINT64_MAX - ticks < step) return true;
            ticks = wp == 0x26 ? ticks + step : ticks - step;
            if (!ce_datetime_from_ticks(ticks, value) || !date_valid(w, value)) return true;
            memcpy(w->date.value, value, 16);
            w->dirty = true;
            struct { CW window, id, code, flags; uint16_t time[8]; } notice =
                {w->handle, w->id, (CW)-759, 0, {0}};
            memcpy(notice.time, value, 16);
            ce_send(w->parent, 0x4e, w->id, WORD(&notice));
        }
        return true;
    default:
        return false;
    }
}
void ce_date_text(struct ce_window *w, WCHAR *out, unsigned capacity)
{
    date_init(w);
    static const WCHAR format[] = {'y','y','y','y','-','M','M','-','d','d',0};
    if (w->date.none) { if (capacity) *out = 0; return; }
    ce_datetime_format(w->date.value, w->date.format[0] ? w->date.format : format, out, capacity);
}
