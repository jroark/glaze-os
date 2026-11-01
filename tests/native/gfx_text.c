/* Compare fitted labels with exhaustive prefix selection, including rounding,
 * clipping, fallback glyphs, and zero-width glyphs. */
#include <glaze/gfx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 240
#define H 60
#define STRIDE 243
static uint16_t actual[STRIDE * H + 8], expected[STRIDE * H + 8];
static unsigned seed = 671;

static unsigned next(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed;
}

static void check(const gfx_font *f, const char *str, int max_w, int iteration)
{
    gfx_surface a, e;
    for (unsigned i = 0; i < sizeof(actual) / sizeof(*actual); i++)
        actual[i] = expected[i] = (uint16_t)next();
    gfx_surface_init(&a, actual + 3, W, H, STRIDE);
    gfx_surface_init(&e, expected + 3, W, H, STRIDE);
    gfx_rect clip = iteration % 2 ? gfx_r(7, 3, 203, 47) : gfx_r(0, 0, W, H);
    gfx_set_clip(&a, clip);
    gfx_set_clip(&e, clip);
    int x = iteration % 9 - 4, y = iteration % 11 - 5;
    int alpha = iteration % 3 ? 255 : 128;
    const uint32_t ink = 0x1974b8;

    if (gfx_text_width(f, str) <= max_w) {
        gfx_text(&e, x, y, f, str, ink, alpha);
    } else {
        int fit = 0, ell = gfx_text_width(f, GFX_CH_ELLIPSIS);
        for (int i = 1; i <= (int)strlen(str); i++)
            if (gfx_text_width_n(f, str, i) + ell <= max_w)
                fit = i;
        int width = gfx_text_n(&e, x, y, f, str, fit, ink, alpha);
        gfx_text(&e, x + width, y, f, GFX_CH_ELLIPSIS, ink, alpha);
    }
    gfx_text_fit(&a, x, y, max_w, f, str, ink, alpha);
    if (memcmp(actual, expected, sizeof(actual)) || memcmp(&a.clip, &e.clip, sizeof(a.clip))) {
        fprintf(stderr, "text fit case %d, width %d, length %zu failed\n", iteration, max_w, strlen(str));
        exit(1);
    }
}

int main(int argc, char **argv)
{
    int cases = 0;
    for (int file = 1; file < argc; file++) {
        FILE *fp = fopen(argv[file], "rb");
        if (!fp)
            return 1;
        fseek(fp, 0, SEEK_END);
        size_t size = (size_t)ftell(fp);
        rewind(fp);
        void *data = malloc(size);
        if (!data || fread(data, 1, size, fp) != size)
            return 1;
        fclose(fp);
        gfx_font f;
        if (!gfx_font_parse(&f, data, size))
            return 1;
        for (int i = 0; i < 300; i++) {
            char str[1024];
            int len = i == 299 ? 1023 : (int)(next() % 120);
            for (int j = 0; j < len; j++)
                str[j] = (char)(1 + next() % 255);
            str[len] = 0;
            int full = gfx_text_width(&f, str);
            int widths[] = {-1, 0, 1, 7, (int)(next() % W), full - 1, full, full + 1};
            for (unsigned j = 0; j < sizeof(widths) / sizeof(*widths); j++)
                check(&f, str, widths[j], cases++);
        }
        free(data);
    }
    /* Fractional and zero advances test the exact 1/16-pixel cutoff. */
    struct gfx_glyph glyphs[256] = {0};
    uint8_t bitmap[4] = {255, 128, 64, 255};
    for (int i = 0; i < 256; i++) {
        glyphs[i].adv16 = i % 4 ? (uint16_t)(i % 79) : 0;
        glyphs[i].w = glyphs[i].h = 2;
    }
    gfx_font f = {.ascent = 2, .count = 256, .glyphs = glyphs, .bitmap = bitmap};
    for (int w = -1; w <= 100; w++) {
        check(&f, "ABCDabcdefgh\x84\xff", w, cases++);
        check(&f, "", w, cases++);
        check(&f, "DDDD", w, cases++);
    }
    printf("text fit: %d pixel comparisons PASS\n", cases);
    return 0;
}
