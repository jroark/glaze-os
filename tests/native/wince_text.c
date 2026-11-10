/* Host checks for the WinCE runtime's wsprintfW formatter and code pages. */
#include "../../apps/wince/text.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t *wide(const char *s)
{
    static uint16_t buffers[4][128];
    static unsigned next;
    uint16_t *w = buffers[next++ % 4];
    unsigned i = 0;
    for (; s[i]; i++)
        w[i] = (uint8_t)s[i];
    w[i] = 0;
    return w;
}
static void check(const char *format, const uintptr_t *args, unsigned count, const char *expected)
{
    uint16_t out[64];
    int n = ce_wformat(out, 64, wide(format), args, count);
    char got[64];
    for (int i = 0; i <= n; i++)
        got[i] = (char)out[i];
    if (n != (int)strlen(expected) || strcmp(got, expected)) {
        fprintf(stderr, "format \"%s\": got \"%s\" (%d), want \"%s\"\n", format, got, n, expected);
        assert(0);
    }
}
#define A(...)                                                                                        \
    ((const uintptr_t[]){__VA_ARGS__}), sizeof((const uintptr_t[]){__VA_ARGS__}) / sizeof(uintptr_t)

int main(void)
{
    check("%d|%i|%u", A(-5, 42, 0xffffffffu), "-5|42|4294967295");
    check("%5d|%-5d|%05d", A(42, 42, -42), "   42|42   |-0042");
    check("%.3d|%x|%#X|%o|%#o", A(7, 0xbeef, 0xbeef, 8, 8), "007|beef|0XBEEF|10|010");
    check("%p|%+d|% d", A(0x1234, 5, 5), "00001234|+5| 5");
    check("%*d|%-*d|%.*s", A(4, 1, 3, 2, 2, (uintptr_t)wide("abc")), "   1|2  |ab");
    check("[%s][%-4s][%4s]", A((uintptr_t)wide("hi"), (uintptr_t)wide("a"), (uintptr_t)wide("b")),
          "[hi][a   ][   b]");
    check("%S|%hs|%ls", A((uintptr_t)"nar", (uintptr_t)"row", (uintptr_t)wide("wide")), "nar|row|wide");
    check("%c%C%lc|%%|%s", A('x', 'y', 'z', 0), "xyz|%|(null)");
    check("%ld|%lu|%.0d|", A(-1, 7, 0), "-1|7||");
    /* Doubles: two words from an even slot (the low word first). */
    union { double d; uint64_t u; } x = {1.25}, y = {-0.000125}, z = {1234567.0}, w = {2.5};
#define D(v) (uintptr_t)(uint32_t)(v).u, (uintptr_t)(uint32_t)((v).u >> 32)
    check("%.1fM|%f|%e", A(D(x), D(x), D(y)), "1.3M|1.250000|-1.250000e-004");
    check("%g|%g|%G|%5.1f|%-6.2f|", A(D(x), D(y), D(z), D(w), D(w)), "1.25|-0.000125|1.23457E+006|  2.5|2.50  |");
    check("%d %f", A(7, 0, D(w)), "7 2.500000"); /* the double skips to an even slot */
    uint16_t out[8];
    assert(ce_wformat(out, 8, wide("%d"), NULL, 0) == -1);       /* missing argument */
    assert(ce_wformat(out, 8, wide("%f"), A(1)) == -1);          /* a double needs two words */
    assert(ce_wformat(out, 8, wide("a%1b"), NULL, 0) == 3 && out[1] == '1' && out[2] == 'b'); /* printed itself */
    assert(ce_wformat(out, 8, wide("x %1"), NULL, 0) == 3 && out[2] == '1');
    assert(ce_wformat(out, 4, wide("abcdef"), NULL, 0) == 3);    /* truncated */
    assert(out[3] == 0);

    assert(ce_cp1252_decode(0x80) == 0x20ac && ce_cp1252_decode(0x92) == 0x2019);
    assert(ce_cp1252_decode(0x81) == 0x81 && ce_cp1252_decode(0xe9) == 0xe9);
    for (unsigned c = 0; c < 256; c++)
        assert(ce_cp1252_encode(ce_cp1252_decode((uint8_t)c)) == (int)c);
    assert(ce_cp1252_encode(0x2603) == -1);

    static const uint8_t utf8[] = {'A', 0xc3, 0xa9, 0xe2, 0x82, 0xac, 0xff, 0xc3};
    uint16_t units[8];
    assert(ce_utf8_decode(utf8, sizeof(utf8), NULL, 0) == 5);
    assert(ce_utf8_decode(utf8, sizeof(utf8), units, 8) == 5);
    assert(units[0] == 'A' && units[1] == 0xe9 && units[2] == 0x20ac && units[3] == 0xfffd &&
           units[4] == 0xfffd); /* invalid byte, truncated sequence */
    uint8_t bytes[8];
    assert(ce_utf8_encode(units, 3, NULL, 0) == 6);
    assert(ce_utf8_encode(units, 3, bytes, 8) == 6 && !memcmp(bytes, utf8, 6));
    assert(ce_utf8_encode(units, 3, bytes, 2) == 6); /* reports the full size */
    puts("wince-text: ALL PASS");
    return 0;
}
