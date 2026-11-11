/* lib/libc's memcmp and bit-count helpers against the host's, natively. The
 * libc objects are built with their symbols renamed (host/Makefile), so the
 * host's own are still there to compare with. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gz_memcmp(const void *a, const void *b, size_t n);
int gz_ctz(uint32_t x);
int gz_clz(uint32_t x);

static uint32_t state = 0x2545F491u;

static uint32_t next(void)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static int sign(int v) { return (v > 0) - (v < 0); }

int main(void)
{
    static uint8_t a[80], b[80];

    for (int i = 0; i < 32; i++) {
        uint32_t bit = 1u << i;
        if (gz_ctz(bit) != i || gz_clz(bit) != 31 - i || gz_ctz(bit | 0x80000000u) != i || gz_clz(bit | 1u) != 31 - i) {
            printf("libc bits: FAIL bit %d\n", i);
            return 1;
        }
    }
    for (int i = 0; i < 2000000; i++) {
        uint32_t x = next() >> (next() % 32);
        if (x && (gz_ctz(x) != __builtin_ctz(x) || gz_clz(x) != __builtin_clz(x))) {
            printf("libc bits: FAIL %08x\n", x);
            return 1;
        }
    }
    /* Every alignment pair, lengths around the word loop's limits, and a
     * difference at each position including none. */
    for (int i = 0; i < 400000; i++) {
        size_t oa = next() % 8, ob = next() % 8, n = next() % 64;
        for (size_t k = 0; k < n; k++)
            a[oa + k] = b[ob + k] = (uint8_t)next();
        if (next() % 4 && n)
            b[ob + next() % n] ^= (uint8_t)(1u << (next() % 8));
        if (sign(gz_memcmp(a + oa, b + ob, n)) != sign(memcmp(a + oa, b + ob, n))) {
            printf("libc bits: FAIL memcmp at %zu/%zu length %zu\n", oa, ob, n);
            return 1;
        }
    }
    puts("libc bits: memcmp, ctz and clz PASS");
    return 0;
}
