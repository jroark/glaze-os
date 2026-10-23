/* Numeric wide scanning used by the PIM date and file-name parsers. */
#include "runtime.h"
static bool scan_space(WCHAR c) { return c == ' ' || (c >= 9 && c <= 13); }
static unsigned scan_digit(WCHAR c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
        c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
}
CE_API(swscanf)
{
    if (!a0 || !a1)
        return (CW)-1;
    const WCHAR *input = PTR(WCHAR, a0), *p = input, *format = PTR(WCHAR, a1);
    CW arguments[] = {a2, a3, a4, a5, a6, a7, a8, a9, a10, a11};
    unsigned argument = 0, assigned = 0;
    while (*format) {
        if (scan_space(*format)) {
            while (scan_space(*format)) format++;
            while (scan_space(*p)) p++;
            continue;
        }
        if (*format != '%' || format[1] == '%') {
            if (*format == '%') format++;
            if (*p != *format)
                return assigned ? assigned : *p ? 0 : (CW)-1;
            p++; format++;
            continue;
        }
        format++;
        bool suppress = *format == '*';
        if (suppress) format++;
        unsigned width = 0;
        while (*format >= '0' && *format <= '9') {
            unsigned digit = *format++ - '0';
            width = MIN(65535u, width * 10 + digit);
        }
        if (!width) width = UINT32_MAX;
        unsigned bytes = 4;
        if (*format == 'h') { bytes = 2; format++; }
        else if (*format == 'l') {
            format++;
            if (*format == 'l') { bytes = 8; format++; }
        }
        WCHAR spec = *format++;
        if (spec != 'd' && spec != 'i' && spec != 'u' && spec != 'x' && spec != 'X' &&
            spec != 'o' && spec != 'n')
            ce_missing("swscanf conversion");
        uint64_t value = 0;
        if (spec == 'n')
            value = (uint32_t)(p - input);
        else {
            while (scan_space(*p)) p++;
            if (!*p)
                return assigned ? assigned : (CW)-1;
            bool negative = *p == '-';
            if ((*p == '-' || *p == '+') && width) { p++; width--; }
            unsigned base = spec == 'o' ? 8 : spec == 'x' || spec == 'X' ? 16 :
                spec == 'i' && *p == '0' ? 8 : 10;
            if ((spec == 'i' || base == 16) && width > 2 && p[0] == '0' &&
                (p[1] == 'x' || p[1] == 'X') && scan_digit(p[2]) < 16) {
                base = 16; p += 2; width -= 2;
            }
            unsigned digits = 0;
            while (width && scan_digit(*p) < base) {
                value = value * base + scan_digit(*p++);
                width--; digits++;
            }
            if (!digits)
                break;
            if (negative) value = 0ull - value;
        }
        if (!suppress) {
            if (argument == ARRAY_SIZE(arguments))
                ce_missing("swscanf: too many arguments");
            void *out = PTR(void, arguments[argument++]);
            if (!out) return assigned;
            if (bytes == 2)
                ce_put16(out, (uint16_t)value);
            else {
                ce_put32(out, (CW)value);
                if (bytes == 8) ce_put32((uint8_t *)out + 4, (CW)(value >> 32));
            }
            if (spec != 'n') assigned++;
        }
    }
    return assigned;
}
void *ce_resolve_scan(const char *name)
{
    return !strcmp(name, "swscanf") ? (void *)ce_swscanf : NULL;
}
