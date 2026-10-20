/* libgfx blits: surface copies, alpha images, scaling, 3-slice. */

#include <glaze/gfx.h>
#include <string.h>

/* Clip a destination rect at (x, y) of size w x h, adjusting the source
 * origin to match. Returns false when nothing is visible. */
static bool clip_blit(const gfx_surface *dst, int *x, int *y, int *sx, int *sy, int *w, int *h)
{
    gfx_rect c;

    if (!gfx_rect_intersect(gfx_r(*x, *y, *w, *h), dst->clip, &c))
        return false;
    *sx += c.x - *x;
    *sy += c.y - *y;
    *x = c.x;
    *y = c.y;
    *w = c.w;
    *h = c.h;
    return true;
}

void gfx_blit(gfx_surface *dst, int x, int y, const gfx_surface *src, gfx_rect sr)
{
    gfx_rect s;

    if (!gfx_rect_intersect(sr, gfx_r(0, 0, src->w, src->h), &s))
        return;
    x += s.x - sr.x;
    y += s.y - sr.y;
    int sx = s.x, sy = s.y, w = s.w, h = s.h;
    if (!clip_blit(dst, &x, &y, &sx, &sy, &w, &h))
        return;
    for (int row = 0; row < h; row++)
        memcpy(dst->px + (y + row) * dst->stride + x, src->px + (sy + row) * src->stride + sx, (size_t)w * 2);
}

void gfx_blit_alpha(gfx_surface *dst, int x, int y, const gfx_surface *src, gfx_rect sr, int alpha)
{
    if (alpha >= 255) {
        gfx_blit(dst, x, y, src, sr);
        return;
    }
    if (alpha <= 0)
        return;
    gfx_rect s;
    if (!gfx_rect_intersect(sr, gfx_r(0, 0, src->w, src->h), &s))
        return;
    x += s.x - sr.x;
    y += s.y - sr.y;
    int sx = s.x, sy = s.y, w = s.w, h = s.h;
    if (!clip_blit(dst, &x, &y, &sx, &sy, &w, &h))
        return;
    for (int row = 0; row < h; row++) {
        uint16_t *d = dst->px + (y + row) * dst->stride + x;
        const uint16_t *sp = src->px + (sy + row) * src->stride + sx;
        for (int i = 0; i < w; i++)
            d[i] = gfx_blend565(d[i], sp[i], alpha);
    }
}

void gfx_blit_scaled(gfx_surface *dst, gfx_rect dr, const gfx_surface *src, gfx_rect sr, int alpha)
{
    gfx_rect c;

    if (alpha <= 0 || dr.w <= 0 || dr.h <= 0 || !gfx_rect_intersect(dr, dst->clip, &c))
        return;
    /* 16.16 source step per destination pixel, sampling pixel centres. */
    int32_t stepx = (sr.w << 16) / dr.w, stepy = (sr.h << 16) / dr.h;
    int32_t fx0 = sr.x * 65536 + stepx / 2 + (c.x - dr.x) * stepx;
    int32_t fy = sr.y * 65536 + stepy / 2 + (c.y - dr.y) * stepy;

    for (int y = c.y; y < c.y + c.h; y++, fy += stepy) {
        int syi = fy >> 16;
        if (syi < 0 || syi >= src->h)
            continue;
        const uint16_t *srow = src->px + syi * src->stride;
        uint16_t *d = dst->px + y * dst->stride;
        int32_t fx = fx0;
        if (alpha >= 255) {
            for (int x = c.x; x < c.x + c.w; x++, fx += stepx)
                d[x] = srow[fx >> 16];
        } else {
            for (int x = c.x; x < c.x + c.w; x++, fx += stepx)
                d[x] = gfx_blend565(d[x], srow[fx >> 16], alpha);
        }
    }
}

bool gfx_image_parse(gfx_image *img, const void *data, size_t size)
{
    const uint8_t *b = data;

    if (size < 12 || memcmp(b, "GZI1", 4))
        return false;
    img->w = b[4] | b[5] << 8;
    img->h = b[6] | b[7] << 8;
    int flags = b[8] | b[9] << 8;
    size_t npx = (size_t)img->w * (size_t)img->h;
    if (size < 12 + npx * 2 + ((flags & GZI_ALPHA) ? npx : 0))
        return false;
    img->px = (const uint16_t *)(b + 12);
    img->alpha = (flags & GZI_ALPHA) ? b + 12 + npx * 2 : NULL;
    return true;
}

void gfx_draw_image(gfx_surface *dst, int x, int y, const gfx_image *img, int alpha)
{
    int sx = 0, sy = 0, w = img->w, h = img->h;

    if (alpha <= 0 || !clip_blit(dst, &x, &y, &sx, &sy, &w, &h))
        return;
    for (int row = 0; row < h; row++) {
        uint16_t *d = dst->px + (y + row) * dst->stride + x;
        const uint16_t *s = img->px + (sy + row) * img->w + sx;
        if (!img->alpha) {
            if (alpha >= 255) {
                memcpy(d, s, (size_t)w * 2);
            } else {
                for (int i = 0; i < w; i++)
                    d[i] = gfx_blend565(d[i], s[i], alpha);
            }
            continue;
        }
        const uint8_t *a = img->alpha + (sy + row) * img->w + sx;
        for (int i = 0; i < w; i++) {
            int pa = a[i];
            if (!pa)
                continue;
            if (alpha < 255)
                pa = pa * alpha / 255;
            d[i] = pa >= 255 ? s[i] : gfx_blend565(d[i], s[i], pa);
        }
    }
}

void gfx_draw_image_scaled(gfx_surface *dst, gfx_rect dr, const gfx_image *img, int alpha)
{
    gfx_rect c;

    if (alpha <= 0 || dr.w <= 0 || dr.h <= 0 || !gfx_rect_intersect(dr, dst->clip, &c))
        return;
    int32_t stepx = (img->w << 16) / dr.w, stepy = (img->h << 16) / dr.h;
    int32_t fy = stepy / 2 + (c.y - dr.y) * stepy;

    for (int y = c.y; y < c.y + c.h; y++, fy += stepy) {
        int sy = fy >> 16;
        uint16_t *d = dst->px + y * dst->stride;
        int32_t fx = stepx / 2 + (c.x - dr.x) * stepx;
        for (int x = c.x; x < c.x + c.w; x++, fx += stepx) {
            int si = sy * img->w + (fx >> 16);
            int pa = img->alpha ? img->alpha[si] : 255;
            if (!pa)
                continue;
            pa = pa * alpha / 255;
            d[x] = pa >= 255 ? img->px[si] : gfx_blend565(d[x], img->px[si], pa);
        }
    }
}

void gfx_draw_mask(gfx_surface *dst, int x, int y, const gfx_image *img, uint32_t rgb, int alpha)
{
    int sx = 0, sy = 0, w = img->w, h = img->h;
    uint16_t c = RGB565(rgb);

    if (!img->alpha || alpha <= 0 || !clip_blit(dst, &x, &y, &sx, &sy, &w, &h))
        return;
    for (int row = 0; row < h; row++) {
        uint16_t *d = dst->px + (y + row) * dst->stride + x;
        const uint8_t *a = img->alpha + (sy + row) * img->w + sx;
        for (int i = 0; i < w; i++) {
            if (a[i])
                d[i] = gfx_blend565(d[i], c, a[i] * alpha / 255);
        }
    }
}

void gfx_draw_3slice(gfx_surface *dst, gfx_rect dr, const gfx_image *img, int cap_l, int cap_r, int alpha)
{
    int mid_src = cap_l;    /* the column repeated across the middle */
    gfx_rect saved = dst->clip, c;

    /* The body column and the right cap must lie inside the image. */
    if (alpha <= 0 || cap_l < 0 || cap_r < 0 || cap_l >= img->w || cap_l + cap_r > img->w)
        return;
    if (dr.w < cap_l + cap_r)
        dr.w = cap_l + cap_r;
    for (int part = 0; part < 3; part++) {
        int dx, dw, sx;
        if (part == 0) {
            dx = dr.x; dw = cap_l; sx = 0;
        } else if (part == 2) {
            dx = dr.x + dr.w - cap_r; dw = cap_r; sx = img->w - cap_r;
        } else {
            dx = dr.x + cap_l; dw = dr.w - cap_l - cap_r; sx = mid_src;
        }
        if (!gfx_rect_intersect(gfx_r(dx, dr.y, dw, img->h), saved, &c))
            continue;
        dst->clip = c;
        if (part == 1) {
            /* Sample one body column, once per destination pixel. Drawing
             * the whole image here overlaps its caps and alpha edges. */
            for (int y = c.y; y < c.y + c.h; y++) {
                int si = (y - dr.y) * img->w + mid_src;
                int a = img->alpha ? img->alpha[si] : 255;
                if (alpha < 255)
                    a = a * alpha / 255;
                if (!a)
                    continue;
                uint16_t px = img->px[si];
                uint16_t *d = dst->px + y * dst->stride;
                for (int x = c.x; x < c.x + c.w; x++)
                    d[x] = a >= 255 ? px : gfx_blend565(d[x], px, a);
            }
        } else {
            gfx_draw_image(dst, dx - sx, dr.y, img, alpha);
        }
    }
    dst->clip = saved;
}
