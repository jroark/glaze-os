/*
 * MD5 (RFC 1321), for BUCP's password hash. Packages don't link wolfSSL.
 */

#include <string.h>
#include "oscar.h"

struct md5 {
    uint32_t h[4];
    uint32_t len;               /* bytes; BUCP hashes are short */
    uint8_t buf[64];
};

static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t R[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void block(struct md5 *m, const uint8_t *p)
{
    uint32_t w[16], a = m->h[0], b = m->h[1], c = m->h[2], d = m->h[3];

    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 | (uint32_t)p[i * 4 + 2] << 16 |
               (uint32_t)p[i * 4 + 3] << 24;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        uint32_t t = d;
        d = c;
        c = b;
        f += a + K[i] + w[g];
        b += f << R[i] | f >> (32 - R[i]);
        a = t;
    }
    m->h[0] += a;
    m->h[1] += b;
    m->h[2] += c;
    m->h[3] += d;
}

static void update(struct md5 *m, const uint8_t *p, size_t n)
{
    size_t have = (size_t)(m->len & 63);

    m->len += n;
    while (n) {
        size_t take = 64 - have < n ? 64 - have : n;
        memcpy(m->buf + have, p, take);
        have += take;
        p += take;
        n -= take;
        if (have == 64) {
            block(m, m->buf);
            have = 0;
        }
    }
}

void oscar_md5(const void *data, size_t len, uint8_t out[16])
{
    struct md5 m = { { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 }, 0, { 0 } };
    uint8_t pad[72] = { 0x80 };
    size_t padn;

    update(&m, data, len);
    padn = (size_t)((m.len & 63) < 56 ? 56 - (m.len & 63) : 120 - (m.len & 63));
    for (int i = 0; i < 4; i++)
        pad[padn + i] = (uint8_t)(m.len << 3 >> (8 * i));
    pad[padn + 4] = (uint8_t)(m.len >> 29);
    update(&m, pad, padn + 8);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = (uint8_t)(m.h[i] >> (8 * j));
}
