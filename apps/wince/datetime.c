/* Pure Gregorian calendar/FILETIME and English formatting, shared by the
 * WinCE adapters and host tests. FILETIME counts 100ns ticks since 1601. */
#include "datetime.h"
#include <glaze/notify.h>
#include <stdio.h>
#include <string.h>

bool ce_datetime_ticks(const uint16_t value[8], uint64_t *ticks)
{
    static const uint8_t lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned y = value[0], m = value[1], d = value[3];
    bool leap = !(y % 4) && (y % 100 || !(y % 400));
    if (y < 1601 || y > 9999 || !m || m > 12 || !d ||
        d > lengths[m - 1] + (unsigned)(m == 2 && leap) || value[4] > 23 ||
        value[5] > 59 || value[6] > 59 || value[7] > 999)
        return false;
    uint64_t days = (uint32_t)(gz_days_from_civil((int)y, (int)m, (int)d) + 145731);
    *ticks = ((days * 86400 + value[4] * 3600u + value[5] * 60u + value[6]) * 1000 + value[7]) * 10000;
    return true;
}
bool ce_datetime_from_ticks(uint64_t ticks, uint16_t value[8])
{
    uint64_t seconds = ticks / 10000000;
    if (seconds / 86400 > 3067670)
        return false;
    int days = (int)(seconds / 86400), z = days + 584694;
    int era = z / 146097, doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int m = mp + (mp < 10 ? 3 : -9);
    value[0] = (uint16_t)(yoe + era * 400 + (m <= 2));
    value[1] = (uint16_t)m;
    value[2] = (uint16_t)((days + 1) % 7);
    value[3] = (uint16_t)(doy - (153 * mp + 2) / 5 + 1);
    uint32_t rem = (uint32_t)(seconds % 86400);
    value[4] = (uint16_t)(rem / 3600);
    value[5] = (uint16_t)(rem / 60 % 60);
    value[6] = (uint16_t)(rem % 60);
    value[7] = (uint16_t)(ticks / 10000 % 1000);
    return value[0] <= 9999;
}
const char *const ce_month_names[12] = {"January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"};
const char *const ce_day_names[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
    "Saturday"};
unsigned ce_datetime_format(const uint16_t value[8], const uint16_t *format,
                            uint16_t *out, unsigned capacity)
{
    uint64_t ticks;
    uint16_t normalized[8];
    if (!value || !format || !ce_datetime_ticks(value, &ticks) ||
        !ce_datetime_from_ticks(ticks, normalized))
        return 0;
    uint16_t result[512];
    unsigned length = 0, pos = 0;
    bool quoted = false;
    while (pos < 256 && format[pos]) {
        uint16_t ch = format[pos++];
        if (ch == '\'') {
            if (format[pos] == '\'')
                pos++;
            else {
                quoted = !quoted;
                continue;
            }
        }
        unsigned count = 1;
        if (!quoted)
            while (pos < 256 && format[pos] == ch) {
                count++;
                pos++;
            }
        char part[40];
        const char *word = NULL;
        int number = -1;
        if (!quoted) switch (ch) {
        case 'd':
            if (count >= 3) word = ce_day_names[normalized[2]];
            else number = value[3];
            break;
        case 'M':
            if (count >= 3) word = ce_month_names[value[1] - 1];
            else number = value[1];
            break;
        case 'y': number = count <= 2 ? value[0] % 100 : value[0]; break;
        case 'H': number = value[4]; break;
        case 'h': number = value[4] % 12 ? value[4] % 12 : 12; break;
        case 'm': number = value[5]; break;
        case 's': number = value[6]; break;
        case 't': word = value[4] < 12 ? "AM" : "PM"; break;
        }
        if (word) {
            unsigned n = (count == 3 && ch != 't') ? 3 : ch == 't' && count == 1 ? 1 : strlen(word);
            memcpy(part, word, n); part[n] = 0;
        } else if (number >= 0) {
            snprintf(part, sizeof(part), count >= 4 && ch == 'y' ? "%04d" : count >= 2 ? "%02d" : "%d", number);
        } else {
            if (length + count >= ARRAY_SIZE(result)) return 0;
            for (unsigned i = 0; i < count; i++) result[length++] = ch;
            continue;
        }
        unsigned n = strlen(part);
        if (length + n >= ARRAY_SIZE(result)) return 0;
        for (unsigned i = 0; i < n; i++) result[length++] = (uint8_t)part[i];
    }
    if (pos == 256 || quoted) return 0;
    result[length++] = 0;
    if (capacity) {
        if (!out || capacity < length) return 0;
        memcpy(out, result, length * 2);
    }
    return length;
}
