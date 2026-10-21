#include "number.h"
bool ce_number_parse(const uint16_t *p, bool signed_value, uint32_t *result)
{
    if (!p || !result)
        return false;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    bool negative = *p == '-';
    if (negative && !signed_value)
        return false;
    if (*p == '+' || *p == '-')
        p++;
    uint32_t limit = signed_value ? negative ? 0x80000000u : 0x7fffffffu : 0xffffffffu;
    uint32_t n = 0;
    bool digit = false;
    while (*p >= '0' && *p <= '9') {
        unsigned d = *p++ - '0';
        if (n > limit / 10 || (n == limit / 10 && d > limit % 10))
            return false;
        n = n * 10 + d;
        digit = true;
    }
    if (!digit)
        return false;
    *result = negative ? 0u - n : n;
    return true;
}
unsigned ce_number_format(uint16_t out[12], uint32_t value, bool signed_value)
{
    unsigned n = 0, start = 0;
    if (signed_value && (int32_t)value < 0) {
        out[n++] = '-';
        value = 0u - value;
        start = n;
    }
    do {
        out[n++] = '0' + value % 10;
        value /= 10;
    } while (value);
    for (unsigned i = start, j = n - 1; i < j; i++, j--) {
        uint16_t ch = out[i];
        out[i] = out[j];
        out[j] = ch;
    }
    out[n] = 0;
    return n;
}
