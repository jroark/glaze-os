#include "hostres.h"

#include <glaze/res.h>

#include <stdio.h>
#include <stdlib.h>

const void *host_load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(*size);
    if (fread(buf, 1, *size, f) != *size)
        exit(1);
    fclose(f);
    return buf;
}

static void put16(FILE *f, unsigned v) { fputc(v & 0xFF, f); fputc(v >> 8, f); }
static void put32(FILE *f, unsigned v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

void host_write_bmp(const char *path, const gfx_surface *s)
{
    FILE *f = fopen(path, "wb");
    unsigned row = (unsigned)(s->w * 3 + 3) & ~3u;

    fputc('B', f); fputc('M', f);
    put32(f, 54 + row * (unsigned)s->h); put32(f, 0); put32(f, 54);
    put32(f, 40); put32(f, (unsigned)s->w); put32(f, (unsigned)s->h);
    put16(f, 1); put16(f, 24); put32(f, 0); put32(f, row * (unsigned)s->h);
    put32(f, 2835); put32(f, 2835); put32(f, 0); put32(f, 0);
    for (int y = s->h - 1; y >= 0; y--) {
        for (int x = 0; x < s->w; x++) {
            uint16_t p = s->px[y * s->stride + x];
            unsigned r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
            fputc((int)(b << 3 | b >> 2), f);
            fputc((int)(g << 2 | g >> 4), f);
            fputc((int)(r << 3 | r >> 2), f);
        }
        for (unsigned pad = (unsigned)s->w * 3; pad < row; pad++)
            fputc(0, f);
    }
    fclose(f);
}

const void *res_map(const char *name, uint32_t *size)
{
    char path[256];
    size_t n;

    snprintf(path, sizeof(path), "../assets/gen/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fclose(f);
    const void *data = host_load(path, &n);
    *size = (uint32_t)n;
    return data;
}
