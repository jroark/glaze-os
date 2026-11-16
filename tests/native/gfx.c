/* Pixel oracles for the optimized RGB565 paths. Deliberately scalar, with
 * independent clipping, square root, and three-slice source selection. */
#include <glaze/gfx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 83
#define H 61
#define STRIDE 87
#define WORDS (STRIDE * H + 8)
static uint16_t actual[WORDS], expected[WORDS];
static unsigned seed = 0x76312;

static unsigned next(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed;
}

static int root(int n)
{
    int lo = 0, hi = 65536;
    while (lo + 1 < hi) {
        int mid = (lo + hi) / 2;
        if ((uint32_t)mid * mid <= (uint32_t)n)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

static int coverage(gfx_rect r, int rad, int x, int y)
{
    if (rad <= 0 || (x >= r.x + rad && x < r.x + r.w - rad) || (y >= r.y + rad && y < r.y + r.h - rad))
        return 255;
    int cx = x < r.x + rad ? r.x + rad : r.x + r.w - rad;
    int cy = y < r.y + rad ? r.y + rad : r.y + r.h - rad;
    int dx = x * 16 + 8 - cx * 16, dy = y * 16 + 8 - cy * 16;
    int a = (rad * 16 - root(dx * dx + dy * dy)) * 16 + 128;
    return MAX(0, MIN(255, a));
}

static uint16_t gradient(gfx_rect r, int x, int y, uint32_t top, uint32_t bottom)
{
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    int d = r.h > 1 ? r.h - 1 : 1, t = y - r.y, v[3];
    for (int i = 0; i < 3; i++) {
        int a = (top >> (16 - i * 8)) & 255;
        int b = (bottom >> (16 - i * 8)) & 255;
        v[i] = a + (b - a) * t / d;
    }
    int q = bayer[y & 3][x & 3];
    return (MIN(31, (v[0] + (q >> 1)) >> 3) << 11) | (MIN(63, (v[1] + (q >> 2)) >> 2) << 5) |
           MIN(31, (v[2] + (q >> 1)) >> 3);
}

static void reference(gfx_surface *s, gfx_rect r, int rad, uint32_t top, uint32_t bottom, int alpha,
                      bool grad)
{
    rad = MAX(0, MIN(rad, MIN(r.w, r.h) / 2));
    if (alpha <= 0)
        return;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (!gfx_rect_contains(r, x, y) || !gfx_rect_contains(s->clip, x, y))
                continue;
            int cov = coverage(r, rad, x, y);
            if (!cov)
                continue;
            int a = cov * alpha / 255;
            uint16_t col = grad ? gradient(r, x, y, top, bottom) : RGB565(top);
            uint16_t *p = s->px + y * s->stride + x;
            *p = a >= 255 ? col : gfx_blend565(*p, col, a);
        }
    }
}

static void slice_reference(gfx_surface *s, gfx_rect r, const gfx_image *img, int left, int right, int alpha)
{
    if (alpha <= 0)
        return;
    r.w = MAX(r.w, left + right);
    r.h = img->h; /* three-slice stretches horizontally only */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (!gfx_rect_contains(r, x, y) || !gfx_rect_contains(s->clip, x, y))
                continue;
            int xx = x - r.x;
            int sx = xx < left ? xx : xx >= r.w - right ? img->w - (r.w - xx) : left;
            int si = (y - r.y) * img->w + sx;
            int a = img->alpha ? img->alpha[si] : 255;
            if (alpha < 255)
                a = a * alpha / 255;
            if (!a)
                continue;
            uint16_t *p = s->px + y * s->stride + x;
            *p = a >= 255 ? img->px[si] : gfx_blend565(*p, img->px[si], a);
        }
    }
}

/* Counter-clockwise quarter turn: dr pixel (x, y) shows sr column
 * (from sr's right edge) dr.y + dr.h - 1 - y and row x - dr.x. Scaled
 * cases sample centres in 16.16 like gfx_blit_scaled, per pixel. */
static void rot_reference(gfx_surface *d, gfx_rect dr, const gfx_surface *s, gfx_rect sr, int alpha)
{
    if (alpha <= 0 || dr.w <= 0 || dr.h <= 0 || sr.w <= 0 || sr.h <= 0)
        return;
    bool unit = dr.w == sr.h && dr.h == sr.w;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (!gfx_rect_contains(dr, x, y) || !gfx_rect_contains(d->clip, x, y))
                continue;
            int u, v;
            if (unit) {
                u = sr.x + dr.y + dr.h - 1 - y;
                v = sr.y + x - dr.x;
            } else {
                int32_t su = (sr.w << 16) / dr.h, sv = (sr.h << 16) / dr.w;
                u = (sr.x * 65536 + su / 2 + (dr.y + dr.h - 1 - y) * su) >> 16;
                v = (sr.y * 65536 + sv / 2 + (x - dr.x) * sv) >> 16;
            }
            if (u < 0 || v < 0 || u >= s->w || v >= s->h)
                continue;
            uint16_t c = s->px[v * s->stride + u], *p = d->px + y * d->stride + x;
            *p = alpha >= 255 ? c : gfx_blend565(*p, c, alpha);
        }
    }
}

static void check(const char *what, int iteration)
{
    for (int i = 0; i < WORDS; i++) {
        if (actual[i] != expected[i]) {
            fprintf(stderr, "%s case %d word %d: %04x != %04x\n", what, iteration, i, actual[i], expected[i]);
            exit(1);
        }
    }
}

int main(void)
{
    static const int alphas[] = {-1, 0, 1, 30, 128, 220, 254, 255};
    uint16_t pixels[30 * 24];
    uint8_t mask[30 * 24];
    for (unsigned i = 0; i < sizeof(mask); i++) {
        pixels[i] = (uint16_t)next();
        mask[i] = i % 3 == 0 ? 0 : i % 3 == 1 ? 255 : (uint8_t)next();
    }
    for (int i = 0; i < 2000; i++) {
        for (int j = 0; j < WORDS; j++)
            actual[j] = expected[j] = (uint16_t)next();
        gfx_surface a, e;
        /* Offset buffer and odd stride exercise halfword-aligned rows. */
        gfx_surface_init(&a, actual + 3, W, H, STRIDE);
        gfx_surface_init(&e, expected + 3, W, H, STRIDE);
        gfx_rect clip = i % 3 ? gfx_r(next() % W, next() % H, next() % W, next() % H) : gfx_r(0, 0, W, H);
        gfx_set_clip(&a, clip);
        gfx_set_clip(&e, clip);
        gfx_rect r =
            gfx_r((int)(next() % 110) - 20, (int)(next() % 80) - 15, (int)(next() % 110), (int)(next() % 80));
        int rad = next() % 50, alpha = alphas[i % 8];
        uint32_t top = next() & 0xffffff, bottom = next() & 0xffffff;
        for (int mode = 0; mode < 2; mode++) {
            memcpy(expected, actual, sizeof(actual));
            reference(&e, r, rad, top, bottom, alpha, mode);
            if (mode)
                gfx_round_rect_vgradient(&a, r, rad, top, bottom, alpha);
            else
                gfx_round_rect(&a, r, rad, top, alpha);
            check("rounded", i);
        }
        memcpy(expected, actual, sizeof(actual));
        reference(&e, r, 0, top, bottom, alpha, true);
        gfx_vgradient(&a, r, top, bottom, alpha);
        check("gradient", i);
        memcpy(expected, actual, sizeof(actual));
        gfx_image img = {30, 24, pixels, i & 1 ? mask : NULL};
        int left = i % 5 ? 12 : 0, right = i % 7 ? 6 : 0;
        slice_reference(&e, r, &img, left, right, alpha);
        gfx_draw_3slice(&a, r, &img, left, right, alpha);
        check("three-slice", i);
        if (memcmp(&a.clip, &e.clip, sizeof(a.clip)))
            return 1;
    }
    /* Caps that don't fit the image draw nothing (and read nothing past it). */
    static const int bad_caps[][2] = {{30, 0}, {40, 6}, {20, 15}, {-1, 6}, {12, -1}};
    for (unsigned i = 0; i < sizeof(bad_caps) / sizeof(bad_caps[0]); i++) {
        for (int j = 0; j < WORDS; j++)
            actual[j] = expected[j] = (uint16_t)next();
        gfx_surface a;
        gfx_surface_init(&a, actual + 3, W, H, STRIDE);
        gfx_image img = {30, 24, pixels, mask};
        gfx_draw_3slice(&a, gfx_r(4, 4, 60, 24), &img, bad_caps[i][0], bad_caps[i][1], 255);
        check("three-slice bad caps", (int)i);
    }
    /* Rotated blits from an odd-strided source, at 1:1 (the landscape
     * present path), scaled (the zoom), partly outside either surface. */
    static uint16_t source[45 * 37];
    for (unsigned i = 0; i < sizeof(source) / 2; i++)
        source[i] = (uint16_t)next();
    for (int i = 0; i < 3000; i++) {
        for (int j = 0; j < WORDS; j++)
            actual[j] = expected[j] = (uint16_t)next();
        gfx_surface a, e, s;
        gfx_surface_init(&a, actual + 3, W, H, STRIDE);
        gfx_surface_init(&e, expected + 3, W, H, STRIDE);
        gfx_surface_init(&s, source + 1, 41, 36, 45);
        gfx_rect clip = i % 3 ? gfx_r(next() % W, next() % H, next() % W, next() % H) : gfx_r(0, 0, W, H);
        gfx_set_clip(&a, clip);
        gfx_set_clip(&e, clip);
        gfx_rect sr = gfx_r((int)(next() % 50) - 5, (int)(next() % 44) - 4, (int)(next() % 48), (int)(next() % 42));
        gfx_rect dr = gfx_r((int)(next() % 100) - 10, (int)(next() % 76) - 10, sr.h, sr.w);
        if (i % 4 == 3)
            dr.w = (int)(next() % 90), dr.h = (int)(next() % 70);
        int alpha = alphas[i % 8];
        rot_reference(&e, dr, &s, sr, alpha);
        gfx_blit_rot90(&a, dr, &s, sr, alpha);
        check("rot90", i);
    }
    puts("gfx: 11005 pixel comparisons PASS (including padding, guards, bad caps and rotation)");
    return 0;
}
