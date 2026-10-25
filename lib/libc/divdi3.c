/*
 * Division helpers normally provided by libgcc. The distro libgcc is built
 * for MIPS32 and uses CLZ, which the VR4131 (MIPS III) does not have.
 *
 * With -mfix-vr4120 (the Makefile's optional VR_FIX) GCC divides 32-bit values only
 * through __udivsi3/__umodsi3 and __vr4120_divsi3/__vr4120_modsi3: on the
 * VR4120 core, DIV can give a wrong result when an operand is negative. So
 * these divide with DIVU in assembly, on magnitudes, as libgcc's VR4120
 * versions do.
 */

#include <stdint.h>

static uint32_t divu32(uint32_t n, uint32_t d, uint32_t *rem)
{
    uint32_t q, r;

    if (!d) {
        if (rem)
            *rem = n;
        return ~0u;
    }
    /* Two instructions after MFHI before the next multiply or divide
     * (MIPS II HI/LO hazard), which the compiler can't see in here. */
    __asm__ volatile("divu $0, %2, %3\n\tmflo %0\n\tmfhi %1\n\tnop\n\tnop"
                     : "=r"(q), "=r"(r) : "r"(n), "r"(d) : "hi", "lo");
    if (rem)
        *rem = r;
    return q;
}

uint32_t __udivsi3(uint32_t n, uint32_t d)
{
    return divu32(n, d, 0);
}

uint32_t __umodsi3(uint32_t n, uint32_t d)
{
    uint32_t r;

    divu32(n, d, &r);
    return r;
}

int32_t __vr4120_divsi3(int32_t n, int32_t d)
{
    uint32_t q = divu32(n < 0 ? -(uint32_t)n : (uint32_t)n, d < 0 ? -(uint32_t)d : (uint32_t)d, 0);

    return (n < 0) != (d < 0) ? -(int32_t)q : (int32_t)q;
}

int32_t __vr4120_modsi3(int32_t n, int32_t d)
{
    uint32_t r;

    divu32(n < 0 ? -(uint32_t)n : (uint32_t)n, d < 0 ? -(uint32_t)d : (uint32_t)d, &r);
    return n < 0 ? -(int32_t)r : (int32_t)r;
}

int32_t __divsi3(int32_t n, int32_t d)
{
    return __vr4120_divsi3(n, d);
}

int32_t __modsi3(int32_t n, int32_t d)
{
    return __vr4120_modsi3(n, d);
}

static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0;

    if (d == 0) {
        if (rem)
            *rem = 0;
        return ~0ull;
    }
    if ((n >> 32) == 0 && (d >> 32) == 0) {
        uint32_t r32, q32 = divu32((uint32_t)n, (uint32_t)d, &r32);
        if (rem)
            *rem = r32;
        return q32;
    }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= 1ull << i;
        }
    }
    if (rem)
        *rem = r;
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d)
{
    return udivmod64(n, d, 0);
}

uint64_t __umoddi3(uint64_t n, uint64_t d)
{
    uint64_t r;

    udivmod64(n, d, &r);
    return r;
}

int64_t __divdi3(int64_t n, int64_t d)
{
    int neg = (n < 0) ^ (d < 0);
    uint64_t q = udivmod64(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, 0);

    return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t n, int64_t d)
{
    uint64_t r;

    udivmod64(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, &r);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}
