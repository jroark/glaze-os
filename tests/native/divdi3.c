/* lib/libc/divdi3.c's 64-bit division against the host's, natively. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

uint64_t __udivdi3(uint64_t n, uint64_t d);
uint64_t __umoddi3(uint64_t n, uint64_t d);
int64_t __divdi3(int64_t n, int64_t d);
int64_t __moddi3(int64_t n, int64_t d);

static uint64_t state = 0x9E3779B97F4A7C15ull;

static uint64_t next(void)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

/* A value with a random number of significant bits, so every path (small
 * divisors, 32-bit ones, 64-bit ones) and the edges between them are hit. */
static uint64_t value(void)
{
    static const uint64_t edge[] = { 1, 2, 999, 1000, 10406, 0xFFFF, 0x10000, 0x10001, 0xFFFFFFFFu,
                                     0x100000000ull, 0x100000001ull, ~0ull, 0x8000000000000000ull };
    uint64_t r = next();

    if (r % 7 == 0)
        return edge[(r >> 8) % (sizeof(edge) / sizeof(edge[0]))];
    return next() >> (r % 64);
}

int main(void)
{
    for (long i = 0; i < 20000000; i++) {
        uint64_t n = value(), d = value();

        if (!d)
            continue;
        if (__udivdi3(n, d) != n / d || __umoddi3(n, d) != n % d) {
            printf("divdi3: FAIL %llx / %llx\n", (unsigned long long)n, (unsigned long long)d);
            return 1;
        }
        int64_t sn = (int64_t)n, sd = (int64_t)d;
        if (sn == INT64_MIN || sd == INT64_MIN)
            continue;
        if (__divdi3(sn, sd) != sn / sd || __moddi3(sn, sd) != sn % sd) {
            printf("divdi3: FAIL signed %llx / %llx\n", (unsigned long long)n, (unsigned long long)d);
            return 1;
        }
    }
    puts("divdi3: 64-bit quotients and remainders PASS");
    return 0;
}
