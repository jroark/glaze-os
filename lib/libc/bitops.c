/*
 * Bit-counting helpers normally provided by libgcc, for __builtin_popcount,
 * __builtin_ctz/clz and __builtin_bswap32 (littlefs, the file server). The
 * VR4131 has no CLZ, so GCC calls these for MIPS II code.
 */

#include <stdint.h>

int __popcountsi2(uint32_t x)
{
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    return (int)((x * 0x01010101u) >> 24);
}

/* Undefined for 0, as for the builtins. */
int __ctzsi2(uint32_t x)
{
    int n = 0;

    while (!(x & 1u)) {
        x >>= 1;
        n++;
    }
    return n;
}

int __clzsi2(uint32_t x)
{
    int n = 0;

    while (!(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return n;
}

uint32_t __bswapsi2(uint32_t x)
{
    return (x >> 24) | ((x >> 8) & 0xFF00u) | ((x << 8) & 0xFF0000u) | (x << 24);
}
