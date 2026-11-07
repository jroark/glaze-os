/* LZ4 block decoder for compressed romfs pages (kernel/romfs.c, written by
 * tools/lz4block.py). Format: lz4 doc/lz4_Block_format.md. Every read and
 * write is bounds-checked: a corrupt page fails, it never overruns. */
#include "kernel.h"
#include "lz4.h"

int lz4_decode(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_len)
{
    const uint8_t *ip = src, *iend = src + src_len;
    uint8_t *op = dst, *oend = dst + dst_len;

    while (ip < iend) {
        unsigned token = *ip++;
        uint32_t lit = token >> 4;
        if (lit == 15) {
            unsigned b;
            do {
                if (ip >= iend) return -1;
                b = *ip++;
                lit += b;
            } while (b == 255);
        }
        if (lit > (uint32_t)(iend - ip) || lit > (uint32_t)(oend - op)) return -1;
        memcpy(op, ip, lit);
        op += lit;
        ip += lit;
        if (ip == iend) break;              /* the last sequence has no match */
        if (iend - ip < 2) return -1;
        uint32_t off = ip[0] | (uint32_t)ip[1] << 8;
        ip += 2;
        if (!off || off > (uint32_t)(op - dst)) return -1;
        uint32_t len = (token & 15) + 4;
        if ((token & 15) == 15) {
            unsigned b;
            do {
                if (ip >= iend) return -1;
                b = *ip++;
                len += b;
            } while (b == 255);
        }
        if (len > (uint32_t)(oend - op)) return -1;
        const uint8_t *m = op - off;
        if (off >= len) {
            memcpy(op, m, len);
            op += len;
        } else {
            while (len--) *op++ = *m++;     /* overlapping: a byte at a time */
        }
    }
    return (int)(op - dst);
}
