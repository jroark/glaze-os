/* libgfx text rendering from pre-rasterized A8 glyph atlases. */

#include <glaze/gfx.h>
#include <string.h>
#include "blend.h"

bool gfx_font_parse(gfx_font *f, const void *data, size_t size)
{
    const uint8_t *b = data;

    if (size < 16 || memcmp(b, "GZF1", 4))
        return false;
    f->size = b[4] | b[5] << 8;
    f->line_height = b[6] | b[7] << 8;
    f->ascent = (int16_t)(b[8] | b[9] << 8);
    f->descent = (int16_t)(b[10] | b[11] << 8);
    f->first = b[12] | b[13] << 8;
    f->count = b[14] | b[15] << 8;
    if (size < 16 + (size_t)f->count * sizeof(struct gfx_glyph))
        return false;
    f->glyphs = (const struct gfx_glyph *)(b + 16);
    f->bitmap = b + 16 + f->count * sizeof(struct gfx_glyph);
    return true;
}

static const struct gfx_glyph *glyph(const gfx_font *f, unsigned char ch)
{
    int i = (int)ch - f->first;

    if (i < 0 || i >= f->count)
        i = '?' - f->first;
    return &f->glyphs[i];
}

int gfx_text_width_n(const gfx_font *f, const char *str, int n)
{
    int adv = 0;

    for (int i = 0; i < n && str[i]; i++)
        adv += glyph(f, (unsigned char)str[i])->adv16;
    return (adv + 8) >> 4;
}

int gfx_text_width(const gfx_font *f, const char *str)
{
    return gfx_text_width_n(f, str, 0x7FFFFFFF);
}

int gfx_text_n(gfx_surface *s, int x, int y, const gfx_font *f, const char *str, int n, uint32_t rgb, int alpha)
{
    uint16_t c = RGB565(rgb);
    int pen16 = x * 16;
    int baseline = y + f->ascent;

    for (int i = 0; i < n && str[i]; i++) {
        const struct gfx_glyph *g = glyph(f, (unsigned char)str[i]);
        int gx = ((pen16 + 8) >> 4) + g->left, gy = baseline - g->top;
        gfx_rect gr = gfx_r(gx, gy, g->w, g->h), cr;

        pen16 += g->adv16;
        if (!g->w || !gfx_rect_intersect(gr, s->clip, &cr))
            continue;
        const uint8_t *bm = f->bitmap + g->off;
        for (int yy = cr.y; yy < cr.y + cr.h; yy++) {
            const uint8_t *a = bm + (yy - gy) * g->w + (cr.x - gx);
            uint16_t *d = s->px + yy * s->stride + cr.x;
            if (alpha >= 255) {
                for (int xx = 0; xx < cr.w; xx++) {
                    int pa = a[xx];
                    if (pa)
                        d[xx] = pa >= 250 ? c : blend565(d[xx], c, pa);
                }
                continue;
            }
            for (int xx = 0; xx < cr.w; xx++) {
                int pa = a[xx];
                if (!pa)
                    continue;
                pa = pa * alpha / 255;
                d[xx] = pa >= 250 ? c : blend565(d[xx], c, pa);
            }
        }
    }
    return ((pen16 + 8) >> 4) - x;
}

int gfx_text(gfx_surface *s, int x, int y, const gfx_font *f, const char *str, uint32_t rgb, int alpha)
{
    return gfx_text_n(s, x, y, f, str, 0x7FFFFFFF, rgb, alpha);
}

int gfx_text_shadow(gfx_surface *s, int x, int y, const gfx_font *f, const char *str, uint32_t rgb,
                    uint32_t shadow_rgb, int dy, int alpha)
{
    gfx_text(s, x, y + dy, f, str, shadow_rgb, alpha * 3 / 5);
    return gfx_text(s, x, y, f, str, rgb, alpha);
}

void gfx_text_center(gfx_surface *s, int x, int y, int w, const gfx_font *f, const char *str, uint32_t rgb, int alpha)
{
    gfx_text(s, x + (w - gfx_text_width(f, str)) / 2, y, f, str, rgb, alpha);
}

void gfx_text_fit(gfx_surface *s, int x, int y, int max_w, const gfx_font *f, const char *str, uint32_t rgb, int alpha)
{
    int ell = gfx_text_width(f, GFX_CH_ELLIPSIS);
    int adv = 0, n = 0, fit = 0;

    /* Advances are nonnegative. Once the full label exceeds max_w, its
     * remaining suffix cannot fit either; don't measure it or repeatedly
     * remeasure shorter prefixes. Keep rounding identical to text_width. */
    while (str[n]) {
        adv += glyph(f, (unsigned char)str[n])->adv16;
        int width = (adv + 8) >> 4;
        if (width > max_w)
            break;
        n++;
        if (width + ell <= max_w)
            fit = n;
    }
    if (!str[n] && ((adv + 8) >> 4) <= max_w) {
        gfx_text_n(s, x, y, f, str, n, rgb, alpha);
        return;
    }
    int w = gfx_text_n(s, x, y, f, str, fit, rgb, alpha);
    gfx_text(s, x + w, y, f, GFX_CH_ELLIPSIS, rgb, alpha);
}
