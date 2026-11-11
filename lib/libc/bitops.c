/*
 * Bit-counting helpers normally provided by libgcc, for __builtin_popcount,
 * __builtin_ctz/clz and __builtin_bswap32 (littlefs, the file server). The
 * VR4131 has no CLZ, so GCC calls these for MIPS II code; and 64-bit shifts.
 */

#include <stdint.h>

int __popcountsi2(uint32_t x)
{
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    return (int)((x * 0x01010101u) >> 24);
}

/* Undefined for 0, as for the builtins. Five tests each, not a loop over
 * up to 31 bits: littlefs calls these for every block it locates. */
int __ctzsi2(uint32_t x)
{
    int n = 0;

    if (!(x & 0x0000FFFFu)) { n += 16; x >>= 16; }
    if (!(x & 0x000000FFu)) { n += 8; x >>= 8; }
    if (!(x & 0x0000000Fu)) { n += 4; x >>= 4; }
    if (!(x & 0x00000003u)) { n += 2; x >>= 2; }
    return n + !(x & 1u);
}

int __clzsi2(uint32_t x)
{
    int n = 0;

    if (!(x & 0xFFFF0000u)) { n += 16; x <<= 16; }
    if (!(x & 0xFF000000u)) { n += 8; x <<= 8; }
    if (!(x & 0xF0000000u)) { n += 4; x <<= 4; }
    if (!(x & 0xC0000000u)) { n += 2; x <<= 2; }
    return n + !(x & 0x80000000u);
}

uint32_t __bswapsi2(uint32_t x)
{
    return (x >> 24) | ((x >> 8) & 0xFF00u) | ((x << 8) & 0xFF0000u) | (x << 24);
}

/* 64-bit shifts, which GCC calls instead of inlining at -Os (the SPL's
 * Dhara ECC). On 32-bit halves, since a 64-bit shift here would call itself. */
union dword { uint64_t u; struct { uint32_t lo, hi; } w; };    /* little-endian */

uint64_t __ashldi3(uint64_t a, int b)
{
    union dword x = { .u = a }, r;

    if (b == 0)
        return a;
    if (b >= 32) {
        r.w.lo = 0;
        r.w.hi = x.w.lo << (b - 32);
    } else {
        r.w.lo = x.w.lo << b;
        r.w.hi = (x.w.hi << b) | (x.w.lo >> (32 - b));
    }
    return r.u;
}

uint64_t __lshrdi3(uint64_t a, int b)
{
    union dword x = { .u = a }, r;

    if (b == 0)
        return a;
    if (b >= 32) {
        r.w.hi = 0;
        r.w.lo = x.w.hi >> (b - 32);
    } else {
        r.w.hi = x.w.hi >> b;
        r.w.lo = (x.w.lo >> b) | (x.w.hi << (32 - b));
    }
    return r.u;
}

int64_t __ashrdi3(int64_t a, int b)
{
    union dword x = { .u = (uint64_t)a }, r;

    if (b == 0)
        return a;
    if (b >= 32) {
        r.w.hi = (uint32_t)((int32_t)x.w.hi >> 31);
        r.w.lo = (uint32_t)((int32_t)x.w.hi >> (b - 32));
    } else {
        r.w.hi = (uint32_t)((int32_t)x.w.hi >> b);
        r.w.lo = (x.w.lo >> b) | (x.w.hi << (32 - b));
    }
    return (int64_t)r.u;
}
