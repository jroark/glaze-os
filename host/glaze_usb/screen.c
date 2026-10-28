#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void be32(uint8_t *p, uint32_t n)
{
    p[0] = n >> 24;
    p[1] = n >> 16;
    p[2] = n >> 8;
    p[3] = n;
}
static int chunk(FILE *f, const char *type, const uint8_t *p, uint32_t n)
{
    uint8_t h[8], crc[4];
    be32(h, n);
    memcpy(h + 4, type, 4);
    be32(crc, ~gu_crc(gu_crc(~0u, type, 4), p, n));
    return fwrite(h, 1, 8, f) != 8 || (n && fwrite(p, 1, n, f) != n) || fwrite(crc, 1, 4, f) != 4 ? -1 : 0;
}
int usb_write_png(FILE *f, const uint8_t *rgb565)
{
    const unsigned row = 1 + 240 * 3, bytes = row * 320;
    uint8_t *raw = malloc(bytes), *z = malloc(bytes + 32);
    if (!raw || !z) {
        free(raw);
        free(z);
        return -1;
    }
    uint32_t a = 1, b = 0;
    for (unsigned y = 0; y < 320; y++) {
        uint8_t *r = raw + y * row;
        *r++ = 0;
        for (unsigned x = 0; x < 240; x++) {
            unsigned at = (y * 240 + x) * 2, v = rgb565[at] | (unsigned)rgb565[at + 1] << 8;
            unsigned red = v >> 11, green = v >> 5 & 63, blue = v & 31;
            *r++ = (red << 3) | (red >> 2);
            *r++ = (green << 2) | (green >> 4);
            *r++ = (blue << 3) | (blue >> 2);
        }
    }
    for (unsigned i = 0; i < bytes; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    unsigned pos = 0, off = 0;
    z[pos++] = 0x78;
    z[pos++] = 1;
    while (off < bytes) {
        unsigned n = bytes - off > 65535 ? 65535 : bytes - off;
        z[pos++] = off + n == bytes ? 1 : 0;
        z[pos++] = n;
        z[pos++] = n >> 8;
        z[pos++] = ~n;
        z[pos++] = ~n >> 8;
        memcpy(z + pos, raw + off, n);
        pos += n;
        off += n;
    }
    be32(z + pos, b << 16 | a);
    pos += 4;
    uint8_t h[13] = {0};
    be32(h, 240);
    be32(h + 4, 320);
    h[8] = 8;
    h[9] = 2;
    int r = fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) != 8 || chunk(f, "IHDR", h, 13) ||
                    chunk(f, "IDAT", z, pos) || chunk(f, "IEND", NULL, 0)
                ? -1
                : 0;
    free(raw);
    free(z);
    return r;
}
int gu_screenshot(struct gu_client *c, const char *path)
{
    if (!(c->caps & GU_CAP_SCREEN)) {
        fputs("This kernel needs the USB screenshot update.\n", stderr);
        return -1;
    }
    int r = gu_call(c, GU_SCREEN_BEGIN, NULL, 0);
    const uint8_t *h = c->frame + GU_HEADER;
    if (r != 16 || gu_get32(h) != 240 || gu_get32(h + 4) != 320 || gu_get32(h + 8) != 1 ||
        gu_get32(h + 12) != GU_SCREEN_BYTES)
        return -1;
    uint8_t *pixels = malloc(GU_SCREEN_BYTES);
    int result = -1;
    if (!pixels)
        goto done;
    for (unsigned off = 0; off < GU_SCREEN_BYTES;) {
        uint8_t request[4];
        gu_put32(request, off);
        r = gu_call(c, GU_SCREEN_READ, request, 4);
        if (r <= 0 || (unsigned)r > GU_SCREEN_BYTES - off)
            goto done;
        memcpy(pixels + off, c->frame + GU_HEADER, (size_t)r);
        off += (unsigned)r;
    }
    char temporary[1024];
    if (snprintf(temporary, sizeof(temporary), "%s.part", path) >= (int)sizeof(temporary))
        goto done;
    FILE *f = fopen(temporary, "wx");
    if (!f) {
        perror(temporary);
        goto done;
    }
    result = usb_write_png(f, pixels);
    if (fclose(f))
        result = -1;
    if (!result && rename(temporary, path)) {
        perror(path);
        result = -1;
    }
    if (result)
        remove(temporary);
    else
        fprintf(stderr, "Screenshot saved to %s (240x320 PNG)\n", path);
done:
    free(pixels);
    if (gu_call(c, GU_SCREEN_END, NULL, 0) < 0)
        result = -1;
    return result;
}
