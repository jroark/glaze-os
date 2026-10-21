#include "../apps/wince/datetime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef uint16_t char16_t;

static void formatted(uint16_t st[8], const char16_t *pattern, const char *expected)
{
    uint16_t output[256];
    unsigned size = strlen(expected) + 1;
    assert(ce_datetime_format(st, (const uint16_t *)pattern, NULL, 0) == size);
    memset(output, 0xab, sizeof(output));
    assert(!ce_datetime_format(st, (const uint16_t *)pattern, output, size - 1));
    assert(output[0] == 0xabab);
    assert(ce_datetime_format(st, (const uint16_t *)pattern, output, size) == size);
    for (unsigned i = 0; i < size; i++) assert(output[i] == (uint8_t)expected[i]);
}
int main(void)
{
    uint16_t st[8] = {1601,1,0,1,0,0,0,0}, actual[8];
    uint64_t ticks;
    assert(ce_datetime_ticks(st, &ticks) && !ticks);
    assert(ce_datetime_from_ticks(0, actual) && actual[2] == 1);
    st[0] = 1970;
    assert(ce_datetime_ticks(st, &ticks) && ticks == 116444736000000000ull);
    st[0] = 2000;
    assert(ce_datetime_ticks(st, &ticks) && ticks == 125911584000000000ull);
    st[0] = 1900; st[1] = 2; st[3] = 29;
    assert(!ce_datetime_ticks(st, &ticks));
    st[0] = 2000;
    assert(ce_datetime_ticks(st, &ticks));
    st[1] = 13;
    assert(!ce_datetime_ticks(st, &ticks));
    st[1] = 2; st[7] = 1000;
    assert(!ce_datetime_ticks(st, &ticks));
    assert(!ce_datetime_from_ticks(UINT64_MAX, actual));
    /* Sample every year, all months, and subsecond precision. */
    for (unsigned y = 1601; y <= 9999; y += 7) for (unsigned m = 1; m <= 12; m++) {
        uint16_t value[8] = {y,m,0,28,23,59,58,999};
        assert(ce_datetime_ticks(value, &ticks));
        assert(ce_datetime_from_ticks(ticks, actual));
        for (unsigned i = 0; i < 8; i++) if (i != 2) assert(actual[i] == value[i]);
    }
    uint16_t sample[8] = {2026,10,0,20,23,5,9,123};
    formatted(sample, u"dddd, MMMM d, yyyy", "Tuesday, October 20, 2026");
    formatted(sample, u"M/d/yy h:mm:ss tt", "10/20/26 11:05:09 PM");
    formatted(sample, u"HH:mm 'o''clock'", "23:05 o'clock");
    formatted(sample, u"ddd MMM dd yyyy t", "Tue Oct 20 2026 P");
    assert(!ce_datetime_format(sample, (const uint16_t *)u"'unfinished", actual, 8));
    puts("wince-datetime: ALL PASS");
    return 0;
}
