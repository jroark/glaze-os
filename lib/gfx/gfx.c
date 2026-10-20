/* libgfx core: surfaces, clipping, fills, gradients, shapes. */

#include <glaze/gfx.h>
#include <stdlib.h>
#include <string.h>

/* 4x4 ordered-dither thresholds, 0..15. */
static const uint8_t bayer4[4][4] = {
    { 0, 8, 2, 10 },
    { 12, 4, 14, 6 },
    { 3, 11, 1, 9 },
    { 15, 7, 13, 5 },
};

bool gfx_rect_intersect(gfx_rect a, gfx_rect b, gfx_rect *out)
{
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);

    *out = gfx_r(x0, y0, x1 - x0, y1 - y0);
    return x1 > x0 && y1 > y0;
}

gfx_rect gfx_rect_union(gfx_rect a, gfx_rect b)
{
    if (gfx_rect_empty(a))
        return b;
    if (gfx_rect_empty(b))
        return a;
    int x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y);
    int x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);
    return gfx_r(x0, y0, x1 - x0, y1 - y0);
}

void gfx_surface_init(gfx_surface *s, uint16_t *px, int w, int h, int stride)
{
    s->px = px;
    s->w = w;
    s->h = h;
    s->stride = stride;
    s->clip = gfx_r(0, 0, w, h);
}

void gfx_set_clip(gfx_surface *s, gfx_rect r)
{
    if (!gfx_rect_intersect(r, gfx_r(0, 0, s->w, s->h), &s->clip))
        s->clip = gfx_r(0, 0, 0, 0);
}

void gfx_reset_clip(gfx_surface *s)
{
    s->clip = gfx_r(0, 0, s->w, s->h);
}

uint16_t gfx_blend565(uint16_t dst, uint16_t src, int alpha)
{
    uint32_t a = ((uint32_t)alpha + 4) >> 3;      /* 0..32 */
    uint32_t d = (dst | ((uint32_t)dst << 16)) & 0x07E0F81Fu;
    uint32_t s = (src | ((uint32_t)src << 16)) & 0x07E0F81Fu;
    uint32_t r = (d + (((s - d) * a) >> 5)) & 0x07E0F81Fu;

    return (uint16_t)(r | (r >> 16));
}

static inline uint16_t dither565(uint32_t r, uint32_t g, uint32_t b, int x, int y)
{
    uint32_t t = bayer4[y & 3][x & 3];

    r = (r + (t >> 1)) >> 3;
    g = (g + (t >> 2)) >> 2;
    b = (b + (t >> 1)) >> 3;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static bool clip_rect(const gfx_surface *s, gfx_rect *r)
{
    return gfx_rect_intersect(*r, s->clip, r);
}

void gfx_fill(gfx_surface *s, gfx_rect r, uint32_t rgb)
{
    uint16_t c = RGB565(rgb);
    uint32_t c2 = c | ((uint32_t)c << 16);

    if (!clip_rect(s, &r))
        return;
    for (int y = r.y; y < r.y + r.h; y++) {
        uint16_t *p = s->px + y * s->stride + r.x;
        int n = r.w;
        if (((uintptr_t)p & 2) && n) {
            *p++ = c;
            n--;
        }
        uint32_t *p32 = (uint32_t *)p;
        for (; n >= 2; n -= 2)
            *p32++ = c2;
        if (n)
            *(uint16_t *)p32 = c;
    }
}

void gfx_fill_alpha(gfx_surface *s, gfx_rect r, uint32_t rgb, int alpha)
{
    if (alpha >= 255) {
        gfx_fill(s, r, rgb);
        return;
    }
    if (alpha <= 0 || !clip_rect(s, &r))
        return;
    uint16_t c = RGB565(rgb);
    for (int y = r.y; y < r.y + r.h; y++) {
        uint16_t *p = s->px + y * s->stride + r.x;
        for (int x = 0; x < r.w; x++)
            p[x] = gfx_blend565(p[x], c, alpha);
    }
}

static inline uint32_t lerp8(uint32_t a, uint32_t b, int num, int den)
{
    return (uint32_t)((int)a + ((int)b - (int)a) * num / den);
}

static void gradient_row(uint16_t row[4], gfx_rect r, int y, uint32_t top, uint32_t bottom);
static void shade_span(uint16_t *p, int x, int end, const uint16_t row[4], int alpha);

void gfx_vgradient(gfx_surface *s, gfx_rect r, uint32_t top, uint32_t bottom, int alpha)
{
    gfx_rect c;

    if (alpha <= 0 || !gfx_rect_intersect(r, s->clip, &c))
        return;
    for (int y = c.y; y < c.y + c.h; y++) {
        uint16_t row[4];
        gradient_row(row, r, y, top, bottom);
        shade_span(s->px + y * s->stride, c.x, c.x + c.w, row, alpha);
    }
}

void gfx_hgradient(gfx_surface *s, gfx_rect r, uint32_t left, uint32_t right, int alpha)
{
    gfx_rect c;

    if (alpha <= 0 || !gfx_rect_intersect(r, s->clip, &c))
        return;
    int den = r.w > 1 ? r.w - 1 : 1;
    for (int y = c.y; y < c.y + c.h; y++) {
        uint16_t *p = s->px + y * s->stride;
        for (int x = c.x; x < c.x + c.w; x++) {
            int t = x - r.x;
            uint16_t px = dither565(lerp8(left >> 16 & 0xFF, right >> 16 & 0xFF, t, den),
                                    lerp8(left >> 8 & 0xFF, right >> 8 & 0xFF, t, den),
                                    lerp8(left & 0xFF, right & 0xFF, t, den), x, y);
            p[x] = alpha >= 255 ? px : gfx_blend565(p[x], px, alpha);
        }
    }
}

static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;

    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/*
 * Coverage (0..255) of the pixel at (px, py) by a disc of radius `r16`
 * (1/16 px) centred at (cx16, cy16), estimated from the distance of the
 * pixel centre to the edge.
 */
static int disc_coverage(int px, int py, int cx16, int cy16, int r16)
{
    int dx = px * 16 + 8 - cx16, dy = py * 16 + 8 - cy16;
    int d16 = (int)isqrt((uint32_t)(dx * dx + dy * dy));
    int cov = (r16 - d16) * 16 + 128;     /* 1 px ramp centred on the edge */

    return cov <= 0 ? 0 : cov >= 255 ? 255 : cov;
}

/* Coverage of pixel (x, y) by the rounded rect r with radius rad. */
static int rr_coverage(gfx_rect r, int rad, int x, int y)
{
    int cx, cy;

    if (x < r.x + rad)
        cx = r.x + rad;
    else if (x >= r.x + r.w - rad)
        cx = r.x + r.w - rad;
    else
        return 255;
    if (y < r.y + rad)
        cy = r.y + rad;
    else if (y >= r.y + r.h - rad)
        cy = r.y + r.h - rad;
    else
        return 255;
    return disc_coverage(x, y, cx * 16, cy * 16, rad * 16);
}

/* Four pre-dithered colours cover a whole row. Keep the absolute screen
 * phase so clipped and full draws produce identical pixels. */
static void gradient_row(uint16_t row[4], gfx_rect r, int y, uint32_t top, uint32_t bottom)
{
    int den = r.h > 1 ? r.h - 1 : 1, t = y - r.y;
    uint32_t cr = lerp8(top >> 16 & 0xFF, bottom >> 16 & 0xFF, t, den);
    uint32_t cg = lerp8(top >> 8 & 0xFF, bottom >> 8 & 0xFF, t, den);
    uint32_t cb = lerp8(top & 0xFF, bottom & 0xFF, t, den);

    for (int x = 0; x < 4; x++)
        row[x] = dither565(cr, cg, cb, x, y);
}

/* Interior spans have full coverage: no corner math or per-pixel shader. */
static void shade_span(uint16_t *p, int x, int end, const uint16_t row[4], int alpha)
{
    if (alpha >= 255) {
        if (x < end && ((uintptr_t)(p + x) & 2)) {
            p[x] = row[x & 3];
            x++;
        }
        for (; x + 1 < end; x += 2)
            *(uint32_t *)(p + x) = row[x & 3] | ((uint32_t)row[(x + 1) & 3] << 16);
        if (x < end)
            p[x] = row[x & 3];
    } else {
        for (; x < end; x++)
            p[x] = gfx_blend565(p[x], row[x & 3], alpha);
    }
}

static void shade_corners(uint16_t *p, int x, int end, int y, gfx_rect r, int rad,
                          const uint16_t row[4], int alpha)
{
    for (; x < end; x++) {
        int cov = rr_coverage(r, rad, x, y);
        if (cov) {
            int a = cov * alpha / 255;
            p[x] = a >= 255 ? row[x & 3] : gfx_blend565(p[x], row[x & 3], a);
        }
    }
}

static void round_row(gfx_surface *s, gfx_rect r, gfx_rect c, int rad, int y,
                      const uint16_t row[4], int alpha)
{
    uint16_t *p = s->px + y * s->stride;
    int left = c.x, right = c.x + c.w;

    if (y < r.y + rad || y >= r.y + r.h - rad) {
        int mid_l = MIN(right, MAX(left, r.x + rad));
        int mid_r = MAX(mid_l, MIN(right, r.x + r.w - rad));
        shade_corners(p, left, mid_l, y, r, rad, row, alpha);
        shade_span(p, mid_l, mid_r, row, alpha);
        shade_corners(p, mid_r, right, y, r, rad, row, alpha);
    } else {
        shade_span(p, left, right, row, alpha);
    }
}

void gfx_round_rect(gfx_surface *s, gfx_rect r, int radius, uint32_t rgb, int alpha)
{
    gfx_rect c;
    uint16_t col = RGB565(rgb), row[4] = { col, col, col, col };

    if (radius <= 0) {
        gfx_fill_alpha(s, r, rgb, alpha);
        return;
    }
    if (alpha <= 0 || !gfx_rect_intersect(r, s->clip, &c))
        return;
    radius = MIN(radius, MIN(r.w, r.h) / 2);
    for (int y = c.y; y < c.y + c.h; y++)
        round_row(s, r, c, radius, y, row, alpha);
}

void gfx_round_rect_vgradient(gfx_surface *s, gfx_rect r, int radius, uint32_t top, uint32_t bottom, int alpha)
{
    gfx_rect c;

    if (alpha <= 0 || !gfx_rect_intersect(r, s->clip, &c))
        return;
    radius = MAX(0, MIN(radius, MIN(r.w, r.h) / 2));
    for (int y = c.y; y < c.y + c.h; y++) {
        uint16_t row[4];
        gradient_row(row, r, y, top, bottom);
        round_row(s, r, c, radius, y, row, alpha);
    }
}

void gfx_circle(gfx_surface *s, int cx, int cy, int radius, uint32_t rgb, int alpha)
{
    gfx_rect r = gfx_r(cx - radius - 1, cy - radius - 1, radius * 2 + 2, radius * 2 + 2), c;
    uint16_t col = RGB565(rgb);

    if (alpha <= 0 || !gfx_rect_intersect(r, s->clip, &c))
        return;
    for (int y = c.y; y < c.y + c.h; y++) {
        uint16_t *p = s->px + y * s->stride;
        for (int x = c.x; x < c.x + c.w; x++) {
            int cov = disc_coverage(x, y, cx * 16, cy * 16, radius * 16);
            if (cov) {
                int a = cov * alpha / 255;
                p[x] = gfx_blend565(p[x], col, a);
            }
        }
    }
}

void gfx_line(gfx_surface *s, int x0, int y0, int x1, int y1, uint32_t rgb, int alpha)
{
    uint16_t c = RGB565(rgb);
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;

    for (;;) {
        if (gfx_rect_contains(s->clip, x0, y0)) {
            uint16_t *p = &s->px[y0 * s->stride + x0];
            *p = alpha >= 255 ? c : gfx_blend565(*p, c, alpha);
        }
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void gfx_line_aa(gfx_surface *s, int x0, int y0, int x1, int y1, int width16, uint32_t rgb, int alpha)
{
    /* Distance-to-segment coverage over the segment's bounding box. */
    int half = width16 / 2;
    int minx = (MIN(x0, x1) - half) / 16 - 1, maxx = (MAX(x0, x1) + half) / 16 + 1;
    int miny = (MIN(y0, y1) - half) / 16 - 1, maxy = (MAX(y0, y1) + half) / 16 + 1;
    int dx = x1 - x0, dy = y1 - y0;
    int32_t len2 = dx * dx + dy * dy;           /* < 2^30 for on-screen lines */
    int32_t len2_8 = len2 >> 8;
    uint16_t c = RGB565(rgb);
    gfx_rect cr;

    if (!gfx_rect_intersect(gfx_r(minx, miny, maxx - minx + 1, maxy - miny + 1), s->clip, &cr))
        return;
    for (int y = cr.y; y < cr.y + cr.h; y++) {
        for (int x = cr.x; x < cr.x + cr.w; x++) {
            int px = x * 16 + 8, py = y * 16 + 8;
            int32_t dot = (px - x0) * dx + (py - y0) * dy;
            int32_t t = len2_8 ? dot / len2_8 : 0;
            if (t < 0) t = 0;
            if (t > 256) t = 256;
            int qx = x0 + (int)(dx * t / 256), qy = y0 + (int)(dy * t / 256);
            int ex = px - qx, ey = py - qy;
            int d = (int)isqrt((uint32_t)(ex * ex + ey * ey));
            int cov = (half - d) * 16 + 128;
            if (cov <= 0)
                continue;
            if (cov > 255)
                cov = 255;
            uint16_t *p = &s->px[y * s->stride + x];
            *p = gfx_blend565(*p, c, cov * alpha / 255);
        }
    }
}
