/* Glaze development protocol over USB or paired TLS. Encoding is ABI-independent. */
#ifndef GLAZE_USB_WIRE_H
#define GLAZE_USB_WIRE_H
#include <stdint.h>
#include <stddef.h>

#define GU_VERSION 1u
#define GU_HEADER 24u
#define GU_PAYLOAD 4096u
#define GU_FRAME (GU_HEADER + GU_PAYLOAD)
#define GU_REPLY 0x8000u
enum {
    GU_HELLO = 1, GU_PUT_BEGIN, GU_PUT_DATA, GU_PUT_END, GU_GET,
    GU_RUN, GU_LOG, GU_BOOT_BEGIN, GU_BOOT_DATA, GU_BOOT_COMMIT,
    GU_ABORT, GU_LOG_READ, GU_SCREEN_BEGIN, GU_SCREEN_READ, GU_SCREEN_END,
    GU_SCRIPT_BEGIN, GU_SCRIPT_DATA, GU_SCRIPT_START, GU_SCRIPT_STATUS,
    GU_SCRIPT_CANCEL, GU_DEBUG_STATUS, GU_REBOOT_CE, GU_ENTROPY, GU_NET_ENABLE,
    GU_USB_NET_OPEN, GU_USB_NET_EXCHANGE, GU_USB_NET_CLOSE
};
enum { GU_CAP_FILES = 1, GU_CAP_RUN = 2, GU_CAP_LOGS = 4, GU_CAP_RELOAD = 8,
       GU_CAP_SCREEN = 16, GU_CAP_SCRIPT = 32, GU_CAP_STATUS = 64, GU_CAP_REBOOT_CE = 128,
       GU_CAP_ENTROPY = 256, GU_CAP_NETWORK = 512, GU_CAP_USB_NET = 1024 };
#define GU_SCRIPT_MAX 2048u
#define GU_SCRIPT_MS 3600000u
#define GU_SCREEN_BYTES (240u * 320u * 2u)
enum { GU_SCRIPT_EMPTY, GU_SCRIPT_LOADING, GU_SCRIPT_READY, GU_SCRIPT_RUNNING, GU_SCRIPT_DONE };
enum { GU_DEBUG_CONNECTED = 1, GU_DEBUG_COPY = 2, GU_DEBUG_RUN = 4, GU_DEBUG_SCRIPT = 8, GU_DEBUG_NETWORK = 16 };

static inline uint32_t gu_get32(const void *vp)
{
    const uint8_t *p = vp;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void gu_put32(void *vp, uint32_t n)
{
    uint8_t *p = vp;
    p[0] = n; p[1] = n >> 8; p[2] = n >> 16; p[3] = n >> 24;
}
/* Incremental reflected CRC-32; initialize with ~0u, complement at end. */
static inline uint32_t gu_crc(uint32_t c, const void *vp, size_t n)
{
    const uint8_t *p = vp;
    while (n--) {
        c ^= *p++;
        for (unsigned k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return c;
}
static inline size_t gu_encode(uint8_t *p, uint32_t op, uint32_t id,
                               int32_t status, const void *data, uint32_t n)
{
    if (n > GU_PAYLOAD) return 0;
    gu_put32(p, 0x42555a47u); /* GZUB */
    gu_put32(p + 4, GU_VERSION | op << 16);
    gu_put32(p + 8, id); gu_put32(p + 12, n); gu_put32(p + 16, (uint32_t)status);
    for (uint32_t i = 0; i < n; i++) p[GU_HEADER + i] = ((const uint8_t *)data)[i];
    uint32_t crc = gu_crc(~0u, p, 20);
    gu_put32(p + 20, ~gu_crc(crc, p + GU_HEADER, n));
    return GU_HEADER + n;
}
struct gu_parser { uint8_t bytes[GU_FRAME]; uint32_t used; };
/* Feed one byte. 1 = complete valid frame, -1 = corrupt frame, 0 = pending.
 * The caller consumes bytes before the next call. Bad headers resynchronize. */
static inline int gu_feed(struct gu_parser *s, uint8_t b)
{
    if (s->used == GU_FRAME) s->used = 0;
    s->bytes[s->used++] = b;
    if (s->used < 4) return 0;
    if (gu_get32(s->bytes) != 0x42555a47u) {
        for (uint32_t i = 1; i < s->used; i++) s->bytes[i - 1] = s->bytes[i];
        s->used--; return 0;
    }
    if (s->used < GU_HEADER) return 0;
    uint32_t n = gu_get32(s->bytes + 12);
    if ((gu_get32(s->bytes + 4) & 65535u) != GU_VERSION || n > GU_PAYLOAD) {
        s->used = 0; return -1;
    }
    if (s->used < GU_HEADER + n) return 0;
    uint32_t c = gu_crc(~0u, s->bytes, 20);
    s->used = 0;
    return ~gu_crc(c, s->bytes + GU_HEADER, n) == gu_get32(s->bytes + 20) ? 1 : -1;
}
#endif
