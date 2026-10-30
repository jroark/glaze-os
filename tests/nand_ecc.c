/*
 * Host test for kernel/drivers/nand_ecc.c, the Reed-Solomon decoder ported
 * from Windows CE's nanddisk.dll: syndromes made from known error patterns
 * (S_k = sum v * x^(k*j), the byte at position 525 - j) must give back the
 * same positions and values, for no, one and two errors; and pages encoded
 * in the layout a real BE-300 uses must check clean, and be corrected back
 * after one or two bytes are changed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/drivers/nand_ecc.h"

static int failures;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint16_t xt(uint16_t a)
{
    a = (uint16_t)(a << 1);
    return (a & 0x400) ? (uint16_t)(a ^ 0x409) : a;
}

static uint16_t gmul(uint16_t a, uint16_t b)
{
    uint16_t r = 0;
    for (int i = 0; i < 10; i++, b >>= 1, a = xt(a))
        if (b & 1)
            r ^= a;
    return r;
}

static uint16_t gpow(uint32_t e)        /* x^e */
{
    uint16_t a = 1;
    while (e--)
        a = xt(a);
    return a;
}

static void make(int n, const uint32_t *pos, const uint16_t *val, uint16_t s[6])
{
    for (int k = 0; k < 6; k++) {
        s[k] = 0;
        for (int i = 0; i < n; i++)
            s[k] ^= gmul(val[i], gpow((uint32_t)k * (525 - pos[i])));
    }
}

static void check(int n, uint32_t p0, uint16_t v0, uint32_t p1, uint16_t v1)
{
    uint32_t pos[2] = { p0, p1 }, got_pos[2] = { 0, 0 };
    uint16_t val[2] = { v0, v1 }, got_val[2] = { 0, 0 }, s[6];

    make(n, pos, val, s);
    int got = nand_ecc_solve(s, got_pos, got_val);
    CHECK(got == n);
    if (got != n)
        return;
    for (int i = 0; i < n; i++) {
        int j = got_pos[0] == pos[i] ? 0 : 1;
        CHECK(got_pos[j] == pos[i]);
        CHECK(got_val[j] == val[i]);
    }
}

static uint8_t br8(uint8_t b)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++)
        if (b & (1u << i))
            r |= (uint8_t)(0x80u >> i);
    return r;
}

/* A page in Windows CE's layout: 520 bytes as symbols bitrev8(b) << 2,
 * parity r(x) = d(x) x^6 mod prod (x - x^k), k = 0..5, packed into spare
 * bytes 8-15 as nand_ecc.c unpacks them. */
static void encode(uint8_t page[528])
{
    uint16_t g[7] = { 1 }, rem[6] = { 0 };

    for (int k = 0; k < 6; k++) {           /* g(x) *= (x + x^k) */
        uint16_t root = gpow((uint32_t)k);
        for (int i = k + 1; i > 0; i--)
            g[i] = (uint16_t)(g[i - 1] ^ gmul(g[i], root));
        g[0] = gmul(g[0], root);
    }
    for (int p = 0; p < 520; p++) {         /* LFSR division, highest first */
        uint16_t f = (uint16_t)((br8(page[p]) << 2) ^ rem[5]);
        for (int i = 5; i > 0; i--)
            rem[i] = (uint16_t)(rem[i - 1] ^ gmul(f, g[i]));
        rem[0] = gmul(f, g[0]);
    }
    uint16_t r0 = rem[5], r1 = rem[4], r2 = rem[3], r3 = rem[2], r4 = rem[1], r5 = rem[0];
    uint8_t b[8] = {
        (uint8_t)(r5 >> 2), (uint8_t)(((r5 & 3) << 6) | (r4 >> 4)),
        (uint8_t)(((r4 & 0xF) << 4) | (r3 >> 6)), (uint8_t)(((r3 & 0x3F) << 2) | (r2 >> 8)),
        (uint8_t)(r2 & 0xFF), (uint8_t)(r1 >> 2),
        (uint8_t)(((r1 & 3) << 6) | (r0 >> 4)), (uint8_t)((r0 & 0xF) << 4),
    };
    for (int i = 0; i < 8; i++)
        page[520 + i] = br8(b[i]);
}

static void check_pages(void)
{
    static uint8_t page[528], orig[528];

    for (int t = 0; t < 300; t++) {
        for (int i = 0; i < 520; i++)
            page[i] = (uint8_t)rand();
        page[512] = 0xAA; page[513] = 0x55; page[514] = 0x0F; page[515] = 0xFF;
        encode(page);
        memcpy(orig, page, sizeof(page));
        memset(page + 520, 0xFF, 8);
        nand_ecc_encode(page);
        CHECK(!memcmp(orig, page, sizeof(page)));
        CHECK(nand_ecc_check(page) == 0);
        unsigned a = (unsigned)rand() % 520, b = (a + 1 + (unsigned)rand() % 519) % 520;
        page[a] ^= (uint8_t)(1 + rand() % 255);
        CHECK(nand_ecc_check(page) == 1 && !memcmp(page, orig, 520));
        page[a] ^= (uint8_t)(1 + rand() % 255);
        page[b] ^= (uint8_t)(1 + rand() % 255);
        CHECK(nand_ecc_check(page) == 2 && !memcmp(page, orig, 520));
    }
}

/* Packed parity faults can straddle two RS symbols. Exercise every bit,
 * mixed data/parity faults, and reject uncorrectable pages without mutation. */
static void check_parity_and_rejection(void)
{
    uint8_t good[528], bad[528], before[528];
    memset(good, 0x5A, 520);
    encode(good);
    for (unsigned off = 520; off < 528; off++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            if (off == 527 && bit >= 4) continue; /* unused bits after bitrev */
            memcpy(bad, good, sizeof(bad));
            bad[off] ^= (uint8_t)(1u << bit);
            CHECK(nand_ecc_check(bad) > 0);
            CHECK(!memcmp(bad, good, sizeof(bad)));
            CHECK(nand_ecc_check(bad) == 0);
            memcpy(bad, good, sizeof(bad));
            bad[17] ^= 0xA5;
            bad[off] ^= (uint8_t)(1u << bit);
            CHECK(nand_ecc_check(bad) == 2);
            CHECK(!memcmp(bad, good, sizeof(bad)));
        }
    }
    for (unsigned t = 0; t < 1000; t++) {
        memcpy(bad, good, sizeof(bad));
        bad[t % 170] ^= 0x31;
        bad[170 + t % 170] ^= 0x53;
        bad[340 + t % 170] ^= 0x97;
        memcpy(before, bad, sizeof(bad));
        CHECK(nand_ecc_check(bad) == -1);
        CHECK(!memcmp(bad, before, sizeof(bad)));
    }
    /* A nonzero S4/S5 with S0..S3 zero must not be accepted. A parity
     * polynomial x^0..x^3 factor has exactly that leading-zero property. */
    uint16_t g[5] = {1};
    for (int k = 0; k < 4; k++) {
        uint16_t root = gpow((uint32_t)k);
        for (int i = k + 1; i > 0; i--) g[i] = g[i-1] ^ gmul(g[i], root);
        g[0] = gmul(g[0], root);
    }
    memcpy(bad, good, sizeof(bad));
    /* Inject the polynomial through parity's six packed symbols by
     * encoding it as the parity bit stream, in the independent layout. */
    uint16_t r0=0, r1=g[4], r2=g[3], r3=g[2], r4=g[1], r5=g[0];
    uint8_t b[8] = { (uint8_t)(r5>>2), (uint8_t)((r5<<6)|(r4>>4)),
        (uint8_t)((r4<<4)|(r3>>6)), (uint8_t)((r3<<2)|(r2>>8)),
        (uint8_t)r2, (uint8_t)(r1>>2), (uint8_t)((r1<<6)|(r0>>4)), (uint8_t)(r0<<4) };
    for (int i=0;i<8;i++) bad[520+i] ^= br8(b[i]);
    memcpy(before,bad,sizeof(bad));
    CHECK(nand_ecc_check(bad) == -1);
    CHECK(!memcmp(bad,before,sizeof(bad)));
}

int main(void)
{
    uint16_t s[6] = { 0 };
    uint32_t pos[2];
    uint16_t val[2];

    CHECK(nand_ecc_solve(s, pos, val) == 0);
    check(1, 0, 0x155, 0, 0);
    check(1, 519, 0x3FC, 0, 0);
    check(1, 525, 1, 0, 0);
    check(2, 3, 0x2A8, 500, 0x054);
    check(2, 0, 0x3FC, 519, 0x004);
    srand(1);
    for (int t = 0; t < 2000; t++) {
        uint32_t a = (uint32_t)rand() % 526, b = (uint32_t)rand() % 526;
        uint16_t va = (uint16_t)(1 + rand() % 1023), vb = (uint16_t)(1 + rand() % 1023);
        check(1, a, va, 0, 0);
        if (a != b)
            check(2, a, va, b, vb);
    }
    /* There is no codeword symbol at exponent 526. */
    for (unsigned k = 0; k < 6; k++) s[k] = gmul(0x155, gpow(k * 526));
    CHECK(nand_ecc_solve(s, pos, val) == -1);
    check_pages();
    check_parity_and_rejection();
    if (failures) {
        fprintf(stderr, "nand-ecc: %d failures\n", failures);
        return 1;
    }
    puts("nand-ecc: syndromes, single and double errors, encoded pages PASS");
    return 0;
}
