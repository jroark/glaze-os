/*
 * Windows CE's NAND page ECC on the BE-300, as its nanddisk.dll checks it
 * (be300-modules 73_nanddisk.dll, 0x019A4718-0x019A5D20): a Reed-Solomon
 * code over GF(2^10) (x^10 + x^3 + 1) with one symbol per byte, correcting
 * up to two bytes of a page's 512 data and first 8 spare bytes. The solver
 * and field arithmetic follow the DLL; the layout was worked out from a real
 * BE-300's pages, because the NAND engine's ECC registers didn't give
 * syndromes that matched: each of the 520 bytes is the symbol
 * bitrev8(b) << 2, at exponent 525 - position; the six parity symbols are
 * spare bytes 8-15, bit-reversed per byte and unpacked as the DLL unpacks
 * them, r0 at exponent 5 down to r5 at 0; a page is clean when c(x^k) = 0
 * for k = 0..5. (Spare bytes 0-3 are AA 55 0F FF and 4-7 the block's
 * logical number.) Plain C over integers so host/ can test it natively.
 */

#include "nand_ecc.h"

#include <stdbool.h>

#define POLY    0x409u          /* x^10 + x^3 + 1 (0x019A5CB0) */
#define SYMBOLS 527u            /* 520 bytes and 6 parity symbols, + 1 */
#define LAST    525u            /* position of symbol j is LAST - j */

/* a*x (0x019A5C98). */
static uint16_t xtime(uint16_t a)
{
    a = (uint16_t)(a << 1);
    return (a & 0x400u) ? (uint16_t)(a ^ POLY) : a;
}

/* a*b (0x019A5CC0). */
static uint16_t mul(uint16_t a, uint16_t b)
{
    uint16_t r = 0;

    for (int i = 0; i < 10; i++) {
        if (b & 1u)
            r ^= a;
        b >>= 1;
        a = xtime(a);
    }
    return r;
}

/* a/b (0x019A5D20): b^-1 by stepping b*x^k to 1. */
static uint16_t gf_div(uint16_t a, uint16_t b)
{
    uint16_t inv = 1;

    b &= 0x3FFu;
    if (!b)
        return a;
    while (b != 1) {
        inv = xtime(inv);
        b = xtime(b);
    }
    return mul(a, inv);
}

static uint8_t bitrev(uint8_t b)
{
    uint8_t r = 0;

    for (int i = 0; i < 8; i++)
        if (b & (1u << i))
            r |= (uint8_t)(0x80u >> i);
    return r;
}

int nand_ecc_solve(const uint16_t s[6], uint32_t pos[2], uint16_t val[2])
{
    uint16_t d0 = mul(s[0], s[2]) ^ mul(s[1], s[1]);
    uint16_t d1 = mul(s[1], s[2]) ^ mul(s[0], s[3]);
    uint16_t d2 = mul(s[1], s[3]) ^ mul(s[2], s[2]);
    uint16_t a = s[0];

    if (!d0 && !d1 && !d2) {
        if (!s[0])
            return 0;
        /* One error (0x019A58A8): S1 = S0 * x^j. */
        for (uint32_t j = 0; j < SYMBOLS; j++, a = xtime(a))
            if (a == s[1]) {
                pos[0] = LAST - j;
                val[0] = s[0];
                return 1;
            }
        return -1;
    }
    if (!d0)
        return -1;
    /* Two errors (0x019A5948): the roots x^j of d0 a^2 + d1 a + d2. */
    int found = 0;
    a = 1;
    for (uint32_t j = 0; j < SYMBOLS; j++, a = xtime(a)) {
        if (mul((uint16_t)(mul(d0, a) ^ d1), a) ^ d2)
            continue;
        pos[found++] = LAST - j;
        if (found == 2) {
            uint16_t e = gf_div(mul((uint16_t)(mul(a, s[0]) ^ s[1]), d0), d1);
            val[0] = e;
            val[1] = s[0] ^ e;
            return 2;
        }
    }
    return -1;
}

/* Log and antilog tables, built on first use. */
static uint16_t exp_t[1023], log_t[1024];

static void tables(void)
{
    uint16_t a = 1;

    if (exp_t[0])
        return;
    for (int i = 0; i < 1023; i++) {
        exp_t[i] = a;
        log_t[a] = (uint16_t)i;
        a = xtime(a);
    }
}

/* b * x^k for k < 1023. */
static uint16_t mul_xk(uint16_t b, uint32_t k)
{
    return b ? exp_t[(log_t[b] + k) % 1023] : 0;
}

static void unpack(const uint8_t raw[8], uint16_t r[6])
{
    uint8_t b[8];

    for (int i = 0; i < 8; i++)
        b[i] = bitrev(raw[i]);
    r[5] = (uint16_t)((b[0] << 2) | ((b[1] & 0xC0u) >> 6));
    r[4] = (uint16_t)(((b[1] & 0x3Fu) << 4) | ((b[2] & 0xF0u) >> 4));
    r[3] = (uint16_t)(((b[2] & 0x0Fu) << 6) | ((b[3] & 0xFCu) >> 2));
    r[2] = (uint16_t)(((b[3] & 0x03u) << 8) | b[4]);
    r[1] = (uint16_t)((b[5] << 2) | ((b[6] & 0xC0u) >> 6));
    r[0] = (uint16_t)(((b[6] & 0x3Fu) << 4) | ((b[7] & 0xF0u) >> 4));
}

int nand_ecc_check(uint8_t page[528])
{
    uint16_t r[6], s[6], val[2];
    uint32_t pos[2];
    bool clean = true;

    tables();
    unpack(page + NAND_ECC_BYTES, r);
    /* Horner over the codeword, highest exponent first, at x^k. */
    for (uint32_t k = 0; k < 6; k++) {
        uint16_t acc = 0;
        for (uint32_t p = 0; p < NAND_ECC_BYTES; p++)
            acc = (uint16_t)(mul_xk(acc, k) ^ (uint16_t)(bitrev(page[p]) << 2));
        for (int i = 0; i < 6; i++)
            acc = (uint16_t)(mul_xk(acc, k) ^ r[i]);
        s[k] = acc;
        clean = clean && !acc;
    }
    if (clean)
        return 0;
    int n = nand_ecc_solve(s, pos, val);
    for (int i = 0; i < n; i++)
        if (pos[i] < NAND_ECC_BYTES)
            page[pos[i]] ^= bitrev((uint8_t)(val[i] >> 2));
    return n;
}
