/* RGB565 GDI adapter. A device context draws to the shared surface (a window
 * DC, offset to its window) or to the bitmap selected into it (a memory DC).
 * Bitmaps of any depth can be read; RGB565 bitmaps can also be drawn into,
 * and other depths take blits through a per-pixel path. Fonts map onto
 * Glaze's sans and mono faces. Resource bitmaps are drawn from the original
 * PE resource data. */
#include "runtime.h"

struct ce_bitmap {
    int w, h, bpp;
    int32_t stride; /* bytes from row to row; negative for bottom-up DIBs */
    uint8_t *bits;  /* the top row */
    uint8_t *block; /* owned allocation, or NULL for resource data */
    bool ddb;       /* CreateBitmap/CompatibleBitmap: mono uses DC colours */
    bool rgb565;
    uint32_t masks[3];
    unsigned colors;
    uint32_t palette[256];
    const uint8_t *and_mask; /* an icon's: set bits are transparent */
    uint32_t and_stride;
    gfx_surface surf; /* valid when rgb565 */
};
struct image_list {
    int cx, cy;
    unsigned count, capacity;
    uint16_t *px; /* the images side by side, cx * capacity pixels wide */
    uint8_t *opaque;
};
struct dc {
    bool used;
    CW window, bitmap, font, brush, pen, palette;
    uint32_t text, back;
    int mode, rop2;
    bool clipped;
    struct ce_rect clip;
};

static struct ce_object objects[512];
static struct dc dcs[32], direct;
static CW stock_brushes[6], stock_pens[3], stock_font, stock_bitmap;

struct ce_object *ce_object(CW handle, int kind)
{
    unsigned i = handle - 0x76000000u;
    return i < ARRAY_SIZE(objects) && objects[i].kind && (!kind || objects[i].kind == kind) ? &objects[i]
                                                                                            : NULL;
}
CW ce_new_object(int kind, CW value, void *data, uint32_t bytes)
{
    for (unsigned i = 0; i < ARRAY_SIZE(objects); i++)
        if (!objects[i].kind) {
            objects[i] = (struct ce_object){kind, value, data, bytes, 0, 0, false};
            return 0x76000000u + i;
        }
    ce_error = 8;
    return 0;
}
uint32_t ce_color(CW color)
{
    return (color & 255) << 16 | (color & 0xff00) | (color >> 16 & 255);
}
static CW colorref(uint32_t rgb)
{
    return ce_color(rgb); /* the byte swap is its own inverse */
}
static uint32_t rgb_of565(uint16_t v)
{
    return ((v >> 11) * 255 / 31) << 16 | ((v >> 5 & 63) * 255 / 63) << 8 | (v & 31) * 255 / 31;
}

/* ---- bitmaps ------------------------------------------------------------ */

static struct ce_bitmap *bitmap(CW handle)
{
    struct ce_object *o = ce_object(handle, CE_OBJ_BITMAP);
    return o ? o->data : NULL;
}
static void bitmap_free(struct ce_bitmap *b)
{
    if (b)
        free(b->block);
    free(b);
}
static unsigned mask_shift(uint32_t mask)
{
    unsigned n = 0;
    while (mask && !(mask & 1)) {
        mask >>= 1;
        n++;
    }
    return n;
}
static uint32_t mask_channel(uint32_t value, uint32_t mask)
{
    if (!mask)
        return 0;
    uint32_t v = (value & mask) >> mask_shift(mask), max = mask >> mask_shift(mask);
    return v * 255 / max;
}
static uint32_t read_pixel(const struct ce_bitmap *b, int x, int y)
{
    const uint8_t *row = b->bits + (int32_t)y * b->stride;
    switch (b->bpp) {
    case 1:
    case 4:
    case 8: {
        unsigned index = b->bpp == 8 ? row[x] : b->bpp == 4 ? (row[x / 2] >> (x & 1 ? 0 : 4)) & 15
                                                            : (row[x / 8] >> (7 - x % 8)) & 1;
        return index < b->colors ? b->palette[index] : 0;
    }
    case 16: {
        uint32_t v = ce_u16(row + x * 2);
        if (b->rgb565)
            return rgb_of565((uint16_t)v);
        return mask_channel(v, b->masks[0]) << 16 | mask_channel(v, b->masks[1]) << 8 |
               mask_channel(v, b->masks[2]);
    }
    case 24:
        return (uint32_t)row[x * 3 + 2] << 16 | row[x * 3 + 1] << 8 | row[x * 3];
    default: {
        uint32_t v = ce_u32(row + x * 4);
        if (b->masks[0])
            return mask_channel(v, b->masks[0]) << 16 | mask_channel(v, b->masks[1]) << 8 |
                   mask_channel(v, b->masks[2]);
        return v & 0xffffff;
    }
    }
}
static unsigned nearest_index(const struct ce_bitmap *b, uint32_t rgb)
{
    unsigned best = 0, best_distance = UINT32_MAX;
    for (unsigned i = 0; i < b->colors; i++) {
        int dr = (int)(b->palette[i] >> 16) - (int)(rgb >> 16);
        int dg = (int)(b->palette[i] >> 8 & 255) - (int)(rgb >> 8 & 255);
        int db = (int)(b->palette[i] & 255) - (int)(rgb & 255);
        unsigned d = (unsigned)(dr * dr + dg * dg + db * db);
        if (d < best_distance) {
            best = i;
            best_distance = d;
        }
    }
    return best;
}
static void write_pixel(struct ce_bitmap *b, int x, int y, uint32_t rgb)
{
    uint8_t *row = b->bits + (int32_t)y * b->stride;
    switch (b->bpp) {
    case 1: {
        unsigned bit = 7 - x % 8;
        row[x / 8] = (uint8_t)((row[x / 8] & ~(1u << bit)) | (nearest_index(b, rgb) & 1) << bit);
        break;
    }
    case 4: {
        unsigned shift = x & 1 ? 0 : 4;
        row[x / 2] = (uint8_t)((row[x / 2] & ~(15u << shift)) | (nearest_index(b, rgb) & 15) << shift);
        break;
    }
    case 8:
        row[x] = (uint8_t)nearest_index(b, rgb);
        break;
    case 16: {
        static const uint32_t rgb565_masks[3] = {0xf800, 0x07e0, 0x001f};
        uint32_t v = 0;
        for (unsigned c = 0; c < 3; c++) {
            uint32_t mask = b->rgb565 ? rgb565_masks[c] : b->masks[c];
            uint32_t max = mask >> mask_shift(mask), channel = rgb >> (16 - 8 * c) & 255;
            v |= (channel * max / 255) << mask_shift(mask);
        }
        ce_put16(row + x * 2, (uint16_t)v);
        break;
    }
    case 24:
        row[x * 3] = (uint8_t)rgb;
        row[x * 3 + 1] = (uint8_t)(rgb >> 8);
        row[x * 3 + 2] = (uint8_t)(rgb >> 16);
        break;
    default:
        ce_put32(row + x * 4, rgb);
        break;
    }
}
static struct ce_bitmap *bitmap_new(int w, int h, int bpp, bool top_down, unsigned row_align)
{
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 ||
        (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32))
        return NULL;
    struct ce_bitmap *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    uint32_t stride = ALIGN_UP((uint32_t)w * (uint32_t)bpp, row_align * 8) / 8;
    b->block = calloc(1, stride * (uint32_t)h);
    if (!b->block) {
        free(b);
        return NULL;
    }
    b->w = w;
    b->h = h;
    b->bpp = bpp;
    b->stride = top_down ? (int32_t)stride : -(int32_t)stride;
    b->bits = top_down ? b->block : b->block + stride * (uint32_t)(h - 1);
    b->rgb565 = bpp == 16;
    if (bpp == 1) {
        b->colors = 2;
        b->palette[1] = 0xffffff;
    }
    return b;
}
/* The gfx view of an RGB565 bitmap; negative strides work too. */
static void bitmap_surface(struct ce_bitmap *b)
{
    if (b->rgb565 && b->bpp == 16)
        gfx_surface_init(&b->surf, (uint16_t *)(void *)b->bits, b->w, b->h, b->stride / 2);
}
/* A DIB: BITMAPINFOHEADER, colour table or bitfields, then pixels, which are
 * copied or, for resources, drawn in place. */
static struct ce_bitmap *bitmap_from_dib(const uint8_t *info, uint32_t info_bytes, const uint8_t *pixels,
                                         uint32_t pixel_bytes, const uint32_t *palette, bool copy)
{
    if (info_bytes < 40)
        return NULL;
    uint32_t header = ce_u32(info), planes = ce_u16(info + 12), bpp = ce_u16(info + 14),
             compression = ce_u32(info + 16), used = ce_u32(info + 32);
    int32_t w = (int32_t)ce_u32(info + 4), h = (int32_t)ce_u32(info + 8);
    if (header < 40 || header > info_bytes || planes != 1 || h == INT32_MIN ||
        (compression && compression != 3) || (compression == 3 && bpp != 16 && bpp != 32))
        return NULL;
    unsigned colors = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    uint32_t table = compression == 3 && header == 40 ? 12 : colors * 4;
    if (colors > 256 || !ce_range(header, table, info_bytes))
        return NULL;
    struct ce_bitmap *b = bitmap_new(w, h < 0 ? -h : h, (int)bpp, h < 0, 4);
    if (!b)
        return NULL;
    uint32_t stride = (uint32_t)(b->stride < 0 ? -b->stride : b->stride);
    if (pixels && !ce_range(0, stride * (uint32_t)b->h, pixel_bytes)) {
        bitmap_free(b);
        return NULL;
    }
    if (pixels && copy)
        memcpy(b->block, pixels, stride * (uint32_t)b->h);
    else if (pixels) {
        free(b->block);
        b->block = NULL;
        b->bits = (uint8_t *)pixels + (h < 0 ? 0 : stride * (uint32_t)(b->h - 1));
    }
    b->colors = colors;
    for (unsigned i = 0; i < colors; i++) {
        const uint8_t *p = info + header + i * 4;
        b->palette[i] = palette ? palette[i] : (uint32_t)p[2] << 16 | p[1] << 8 | p[0];
    }
    if (compression == 3) {
        for (unsigned c = 0; c < 3; c++)
            b->masks[c] = header >= 52 ? ce_u32(info + 40 + c * 4) : ce_u32(info + header + c * 4);
    } else if (bpp == 16) {
        b->masks[0] = 0x7c00;
        b->masks[1] = 0x03e0;
        b->masks[2] = 0x001f;
    }
    b->rgb565 = bpp == 16 && compression == 3 && b->masks[0] == 0xf800 && b->masks[1] == 0x07e0 &&
                b->masks[2] == 0x001f;
    bitmap_surface(b);
    return b;
}
/* Resource pixels are read-only: a resource bitmap selected into a DC gets
 * an owned RGB565 copy first. */
static bool bitmap_owned(struct ce_bitmap *b)
{
    if (b->block)
        return true;
    struct ce_bitmap *c = bitmap_new(b->w, b->h, 16, true, 4);
    if (!c)
        return false;
    for (int y = 0; y < b->h; y++)
        for (int x = 0; x < b->w; x++)
            ce_put16(c->bits + y * c->stride + x * 2, RGB565(read_pixel(b, x, y)));
    *b = *c;
    free(c);
    bitmap_surface(b);
    return true;
}
static CW bitmap_object(struct ce_bitmap *b)
{
    if (!b)
        return 0;
    bitmap_surface(b);
    CW handle = ce_new_object(CE_OBJ_BITMAP, 0, b, sizeof(*b));
    if (!handle)
        bitmap_free(b);
    return handle;
}
static CW resource_bitmap(CW module, CW id)
{
    uint32_t size;
    const uint8_t *data = ce_resource(module, 2, (uint16_t)id, &size);
    if (!data || size < 40)
        return 0;
    uint32_t header = ce_u32(data), bpp = ce_u16(data + 14), used = ce_u32(data + 32),
             compression = ce_u32(data + 16);
    uint32_t offset = header + (bpp <= 8 ? (used ? used : 1u << bpp) * 4 : 0) +
                      (compression == 3 && header == 40 ? 12 : 0);
    if (offset > size)
        ce_fatal("Bitmap pixels exceed resource");
    struct ce_bitmap *b = bitmap_from_dib(data, size, data + offset, size - offset, NULL, false);
    if (!b)
        ce_missing("unsupported bitmap resource layout");
    return bitmap_object(b);
}
/* An RT_GROUP_ICON's first entry names its RT_ICON image: a DIB of twice the
 * icon's height, the colour rows and then the AND mask, used in place. */
static CW resource_icon(CW module, CW id)
{
    uint32_t size;
    const uint8_t *group = ce_resource(module, 14, (uint16_t)id, &size);
    if (!group || size < 20 || ce_u16(group + 2) != 1 || !ce_u16(group + 4))
        return 0;
    const uint8_t *data = ce_resource(module, 3, ce_u16(group + 18), &size);
    if (!data || size < 40)
        return 0;
    uint32_t header = ce_u32(data), bpp = ce_u16(data + 14), used = ce_u32(data + 32);
    int32_t h = (int32_t)ce_u32(data + 8);
    uint32_t offset = header + (bpp <= 8 ? (used ? used : 1u << bpp) * 4 : 0);
    uint8_t info[40 + 256 * 4];
    if (header != 40 || h <= 0 || (h & 1) || offset > sizeof(info) || offset > size)
        return 0;
    memcpy(info, data, offset);
    ce_put32(info + 8, (CW)(h / 2));
    struct ce_bitmap *b = bitmap_from_dib(info, offset, data + offset, size - offset, NULL, false);
    if (!b)
        return 0;
    uint32_t stride = (uint32_t)(b->stride < 0 ? -b->stride : b->stride);
    b->and_stride = ALIGN_UP((uint32_t)b->w, 32u) / 8;
    if (!ce_range(offset + stride * (uint32_t)b->h, b->and_stride * (uint32_t)b->h, size)) {
        bitmap_free(b);
        return 0;
    }
    b->and_mask = data + offset + stride * (uint32_t)b->h;
    CW handle = ce_new_object(CE_OBJ_ICON, 1, b, sizeof(*b));
    if (!handle)
        bitmap_free(b);
    return handle;
}
static bool icon_transparent(const struct ce_bitmap *b, int x, int y)
{
    return b->and_mask && (b->and_mask[(uint32_t)(b->h - 1 - y) * b->and_stride + (unsigned)x / 8] & (0x80u >> (x & 7)));
}

/* ---- device contexts ---------------------------------------------------- */

static void dc_defaults(struct dc *d, CW window)
{
    *d = (struct dc){true, window, 0, 0, 0, 0, 0, 0, 0xffffff, 2, 13, false, {0, 0, 0, 0}};
}
static struct dc *dc_get(CW handle)
{
    unsigned i = handle - 0x75000000u;
    if (i < ARRAY_SIZE(dcs))
        return dcs[i].used ? &dcs[i] : NULL;
    /* The runtime's own controls draw with their window handle, with
     * transparent text. */
    if (!direct.used) {
        dc_defaults(&direct, 0);
        direct.mode = 1;
    }
    direct.window = handle;
    return &direct;
}
static CW dc_new(CW window)
{
    for (unsigned i = 0; i < ARRAY_SIZE(dcs); i++)
        if (!dcs[i].used) {
            dc_defaults(&dcs[i], window);
            return 0x75000000u + i;
        }
    ce_error = 8;
    return 0;
}
#define MEMORY_DC 0xffffffffu
static bool is_memory_dc(const struct dc *d)
{
    return d->window == MEMORY_DC;
}
static gfx_rect window_bounds(CW window)
{
    return window && window != 0x74000000u ? ce_client_rect(window) : gfx_r(0, 0, ui.surf.w, ui.surf.h);
}
struct target {
    struct dc *dc;
    gfx_surface *s;
    struct ce_bitmap *bitmap; /* memory DC: the selected bitmap */
    gfx_rect saved;
    int x, y;
};
/* Resolves a DC for drawing: its surface, origin and clip (the DC's clip
 * rectangle within its window or bitmap). end() restores the clip. */
static bool begin(CW handle, struct target *t)
{
    memset(t, 0, sizeof(*t));
    t->dc = dc_get(handle);
    if (!t->dc)
        return false;
    gfx_rect bounds;
    if (is_memory_dc(t->dc)) {
        t->bitmap = bitmap(t->dc->bitmap);
        if (!t->bitmap || !t->bitmap->rgb565)
            return false;
        t->s = &t->bitmap->surf;
        bounds = gfx_r(0, 0, t->bitmap->w, t->bitmap->h);
    } else {
        t->s = &ui.surf;
        bounds = window_bounds(t->dc->window);
        t->x = bounds.x;
        t->y = bounds.y;
        ce_screen_dirty = true;
    }
    t->saved = t->s->clip;
    if (t->dc->clipped) {
        gfx_rect c = gfx_r(t->x + t->dc->clip.left, t->y + t->dc->clip.top, t->dc->clip.right - t->dc->clip.left,
                           t->dc->clip.bottom - t->dc->clip.top);
        if (!gfx_rect_intersect(bounds, c, &bounds))
            bounds = gfx_r(0, 0, 0, 0);
    }
    gfx_set_clip(t->s, bounds);
    return true;
}
static void end(struct target *t)
{
    if (t->s)
        t->s->clip = t->saved;
}
static bool clipped_out(const gfx_rect *c, int x, int y)
{
    return x < c->x || y < c->y || x >= c->x + c->w || y >= c->y + c->h;
}
/* One pixel at surface coordinates, combined by an R2_* mode. */
static void plot(struct target *t, int x, int y, uint32_t rgb, int rop2)
{
    if (clipped_out(&t->s->clip, x, y))
        return;
    uint16_t *p = &t->s->px[y * t->s->stride + x], pen = RGB565(rgb);
    switch (rop2) {
    case 1: /* R2_BLACK */
        *p = 0;
        break;
    case 6: /* R2_NOT */
        *p = (uint16_t)~*p;
        break;
    case 7: /* R2_XORPEN */
        *p ^= pen;
        break;
    case 11: /* R2_NOP */
        break;
    case 16: /* R2_WHITE */
        *p = 0xffff;
        break;
    default: /* R2_COPYPEN */
        *p = pen;
        break;
    }
}
static void fill(struct target *t, int x, int y, int w, int h, uint32_t rgb)
{
    gfx_fill(t->s, gfx_r(t->x + x, t->y + y, w, h), rgb);
}
/* A line in DC coordinates; like Windows, the last point is excluded
 * unless `last` is set. */
static void line(struct target *t, int x0, int y0, int x1, int y1, uint32_t rgb, int rop2, bool last)
{
    x0 += t->x;
    y0 += t->y;
    x1 += t->x;
    y1 += t->y;
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (unsigned n = 0; n < 8192; n++) {
        if (x0 == x1 && y0 == y1) {
            if (last)
                plot(t, x0, y0, rgb, rop2);
            return;
        }
        plot(t, x0, y0, rgb, rop2);
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
static uint32_t brush_color(const struct dc *d)
{
    struct ce_object *b = ce_object(d->brush ? d->brush : stock_brushes[0], CE_OBJ_BRUSH);
    return b ? b->value : 0xffffff;
}
static struct ce_object *pen_of(const struct dc *d)
{
    return ce_object(d->pen ? d->pen : stock_pens[1], CE_OBJ_PEN);
}

CE_API(BeginPaint)
{
    struct ce_window *w = ce_window(a0);
    if (!w || !a1)
        return 0;
    CW hdc = dc_new(a0);
    if (!hdc)
        return 0;
    memset(PTR(void, a1), 0, 64);
    CW *p = PTR(CW, a1);
    p[0] = hdc;
    p[1] = 1;
    *PTR(struct ce_rect, a1 + 8) =
        (struct ce_rect){0, 0, w->rect.right - w->rect.left, w->rect.bottom - w->rect.top};
    w->dirty = false;
    ce_screen_dirty = true;
    return hdc;
}
static CW release_dc(CW handle)
{
    unsigned i = handle - 0x75000000u;
    if (i >= ARRAY_SIZE(dcs) || !dcs[i].used || is_memory_dc(&dcs[i]))
        return 0;
    dcs[i].used = false;
    return 1;
}
CE_API(EndPaint)
{
    if (a1)
        release_dc(ce_u32(PTR(void, a1)));
    return ce_window(a0) != NULL;
}
CE_API(GetDC)
{
    ce_screen_dirty = true;
    return dc_new(a0 ? a0 : 0x74000000u);
}
CE_API(ReleaseDC)
{
    return release_dc(a1);
}
CE_API(CreateCompatibleDC)
{
    CW handle = dc_new(MEMORY_DC);
    struct dc *d = handle ? dc_get(handle) : NULL;
    if (d)
        d->bitmap = stock_bitmap;
    return handle;
}
CE_API(DeleteDC)
{
    unsigned i = a0 - 0x75000000u;
    if (i >= ARRAY_SIZE(dcs) || !dcs[i].used)
        return 0;
    dcs[i].used = false;
    return 1;
}
CE_API(GetDeviceCaps)
{
    switch (a1) {
    case 2: /* TECHNOLOGY: DT_RASDISPLAY */
        return 1;
    case 4: /* HORZSIZE, mm */
        return 58;
    case 6: /* VERTSIZE */
        return 77;
    case 8:
        return 240;
    case 10:
        return 320;
    case 12:
        return 16;
    case 14:
        return 1;
    case 24: /* NUMCOLORS: not palettized */
        return 0xffffffffu;
    case 38: /* RASTERCAPS: RC_BITBLT | RC_STRETCHBLT */
        return 0x801;
    case 88:
    case 90:
        return 96;
    case 108: /* COLORRES */
        return 16;
    default:
        return 0;
    }
}
CE_API(GetSysColor)
{
    switch (a0) {
    case 0:
    case 1:
    case 15:
        return 0xc0c0c0;
    case 5:
    case 14:
    case 20:
        return 0xffffff;
    case 2:
    case 13:
        return 0x800000;
    case 16:
        return 0x808080;
    default:
        return 0;
    }
}
static CW stock(CW handle)
{
    struct ce_object *o = ce_object(handle, 0);
    if (o)
        o->stock = true;
    return handle;
}
CE_API(GetSysColorBrush)
{
    static CW brushes[32];
    if (a0 >= ARRAY_SIZE(brushes))
        return 0;
    if (!ce_object(brushes[a0], CE_OBJ_BRUSH))
        brushes[a0] = stock(ce_new_object(CE_OBJ_BRUSH,
            ce_color(ce_GetSysColor(a0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)), NULL, 0));
    return brushes[a0];
}
CE_API(GetStockObject)
{
    if (a0 <= 5) {
        static const CW colors[] = {0xffffff, 0xc0c0c0, 0x808080, 0x404040, 0, 0xffffffffu};
        if (!ce_object(stock_brushes[a0], CE_OBJ_BRUSH))
            stock_brushes[a0] = stock(ce_new_object(CE_OBJ_BRUSH, colors[a0], NULL, 0));
        return stock_brushes[a0];
    }
    if (a0 <= 8) {
        unsigned index = a0 - 6;
        if (!ce_object(stock_pens[index], CE_OBJ_PEN)) {
            stock_pens[index] = stock(ce_new_object(CE_OBJ_PEN, index ? 0 : 0xffffff, NULL, 0));
            struct ce_object *pen = ce_object(stock_pens[index], CE_OBJ_PEN);
            if (pen) {
                pen->width = 1;
                pen->height = index == 2 ? 5 : 0;
            }
        }
        return stock_pens[index];
    }
    if (!ce_object(stock_font, CE_OBJ_FONT))
        stock_font = stock(ce_new_object(CE_OBJ_FONT, FONT_R12, NULL, 0));
    return stock_font;
}
static void stock_init(void)
{
    if (stock_bitmap)
        return;
    for (CW i = 0; i <= 8; i++)
        ce_GetStockObject(i, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    ce_GetStockObject(13, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    stock_bitmap = stock(bitmap_object(bitmap_new(1, 1, 1, true, 2)));
}
CE_API(CreateSolidBrush)
{
    return ce_new_object(CE_OBJ_BRUSH, ce_color(a0), NULL, 0);
}
CE_API(CreatePen)
{
    if (a0 > 5 || a1 > 16) {
        ce_error = 50;
        return 0;
    }
    CW handle = ce_new_object(CE_OBJ_PEN, ce_color(a2), NULL, 0);
    struct ce_object *pen = ce_object(handle, CE_OBJ_PEN);
    if (pen) {
        pen->width = MAX(1u, a1);
        pen->height = (int)a0;
    }
    return handle;
}
CE_API(CreatePenIndirect)
{
    if (!a0) {
        ce_error = 87;
        return 0;
    }
    const uint8_t *p = PTR(uint8_t, a0); /* LOGPEN: style, width POINT, colour */
    return ce_CreatePen(ce_u32(p), ce_u32(p + 4), ce_u32(p + 12), 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* ---- fonts -------------------------------------------------------------- */

struct face {
    int font, size;
};
static const struct face regular[] = {{FONT_R9, 9},   {FONT_R10, 10}, {FONT_R11, 11}, {FONT_R12, 12},
                                      {FONT_R13, 13}, {FONT_R14, 14}, {FONT_R17, 17}, {FONT_R24, 24},
                                      {FONT_R44, 44}};
static const struct face bold[] = {{FONT_B8, 8},   {FONT_B9, 9},   {FONT_B10, 10},
                                   {FONT_B11, 11}, {FONT_B12, 12}, {FONT_B13, 13},
                                   {FONT_B14, 14}, {FONT_B17, 17}, {FONT_B20, 20}};
static const struct face italic[] = {{FONT_I10, 10}, {FONT_I11, 11}, {FONT_I12, 12}, {FONT_I13, 13},
                                     {FONT_I14, 14}};
static const struct face mono[] = {{FONT_M10, 10}, {FONT_M11, 11}, {FONT_M12, 12}};
static bool is_mono(int font)
{
    return font == FONT_M10 || font == FONT_M11 || font == FONT_M12;
}
static bool is_bold(int font)
{
    return font >= FONT_B8 && font <= FONT_B20;
}
static bool is_italic(int font)
{
    return font >= FONT_I10 && font <= FONT_I14;
}
/* The nearest face to a LOGFONT. Its height is the character height when
 * negative and the cell height when positive. */
static int pick_font(const uint8_t *logfont)
{
    int32_t height = (int32_t)ce_u32(logfont);
    int32_t weight = (int32_t)ce_u32(logfont + 16);
    char face[64];
    ce_narrow(face, sizeof(face), (const WCHAR *)(const void *)(logfont + 28));
    const struct face *set = regular;
    unsigned count = ARRAY_SIZE(regular);
    if ((logfont[27] & 3) == 1 || (logfont[27] & 0xf0) == 0x30 || strstr(face, "Courier") ||
        strstr(face, "Mono")) {
        set = mono;
        count = ARRAY_SIZE(mono);
    } else if (weight >= 600) {
        set = bold;
        count = ARRAY_SIZE(bold);
    } else if (logfont[20]) {
        set = italic;
        count = ARRAY_SIZE(italic);
    }
    int want = height ? abs(height) : 12;
    int best = set[0].font, distance = INT32_MAX;
    for (unsigned i = 0; i < count; i++) {
        int size = height > 0 ? ui_font(set[i].font)->line_height : set[i].size;
        if (abs(size - want) < distance) {
            distance = abs(size - want);
            best = set[i].font;
        }
    }
    return best;
}
CE_API(CreateFontIndirectW)
{
    if (!a0)
        return 0;
    uint8_t *copy = malloc(92);
    if (!copy)
        return 0;
    memcpy(copy, PTR(void, a0), 92);
    CW handle = ce_new_object(CE_OBJ_FONT, (CW)pick_font(copy), copy, 92);
    if (!handle)
        free(copy);
    return handle;
}
static int dc_font_index(const struct dc *d)
{
    struct ce_object *o = ce_object(d && d->font ? d->font : stock_font, CE_OBJ_FONT);
    return o ? (int)o->value : FONT_R12;
}
static const gfx_font *dc_font(const struct dc *d)
{
    return ui_font(dc_font_index(d));
}
/* TEXTMETRICW, 60 bytes (NEWTEXTMETRICW is 72). */
static void text_metrics(uint8_t *tm, int font)
{
    const gfx_font *f = ui_font(font);
    memset(tm, 0, 60);
    ce_put32(tm, (CW)f->line_height);
    ce_put32(tm + 4, (CW)f->ascent);
    ce_put32(tm + 8, (CW)abs(f->descent));
    ce_put32(tm + 12, (CW)MAX(0, f->line_height - f->size));
    ce_put32(tm + 20, (CW)gfx_text_width(f, "x"));
    ce_put32(tm + 24, (CW)gfx_text_width(f, "W"));
    ce_put32(tm + 28, is_bold(font) ? 700 : 400);
    ce_put32(tm + 36, 96);
    ce_put32(tm + 40, 96);
    ce_put16(tm + 44, 32);
    ce_put16(tm + 46, 255);
    ce_put16(tm + 48, '?');
    ce_put16(tm + 50, ' ');
    tm[52] = is_italic(font);
    tm[55] = is_mono(font) ? 0x30 : 0x21; /* TMPF_FIXED_PITCH set means variable pitch */
}
CE_API(GetTextMetricsW)
{
    struct dc *d = dc_get(a0);
    if (!a1 || !d)
        return 0;
    text_metrics(PTR(uint8_t, a1), dc_font_index(d));
    return 1;
}
CE_API(EnumFontFamiliesW)
{
    static const char *const families[] = {"Tahoma", "Courier New"};
    char wanted[64] = "";
    if (a1)
        ce_narrow(wanted, sizeof(wanted), PTR(WCHAR, a1));
    CW result = 1;
    for (unsigned i = 0; i < ARRAY_SIZE(families); i++) {
        if (wanted[0] && strcasecmp(wanted, families[i]))
            continue;
        /* ENUMLOGFONT (LOGFONT, full name, style) and NEWTEXTMETRIC. */
        uint8_t logfont[92 + 128 + 64] = {0}, metrics[72] = {0};
        ce_put32(logfont, (CW)-12);
        ce_put32(logfont + 16, 400);
        logfont[27] = i ? 0x31 : 0x22; /* FIXED_PITCH | FF_MODERN, VARIABLE_PITCH | FF_SWISS */
        ce_widen((WCHAR *)(void *)(logfont + 28), 32, families[i]);
        ce_widen((WCHAR *)(void *)(logfont + 92), 64, families[i]);
        ce_widen((WCHAR *)(void *)(logfont + 220), 32, "Regular");
        text_metrics(metrics, i ? FONT_M12 : FONT_R12);
        result = ce_call(a2, WORD(logfont), WORD(metrics), 4 /* TRUETYPE_FONTTYPE */, a3);
        if (!result)
            break;
    }
    return result;
}

/* ---- objects ------------------------------------------------------------ */

CE_API(DeleteObject)
{
    struct ce_object *o = ce_object(a0, 0);
    if (!o)
        return 0;
    if (o->stock)
        return 1;
    for (unsigned i = 0; i < ARRAY_SIZE(dcs); i++)
        if (dcs[i].used && (dcs[i].bitmap == a0 || dcs[i].font == a0 || dcs[i].brush == a0 ||
                            dcs[i].pen == a0 || dcs[i].palette == a0))
            return 0; /* objects selected into a DC stay */
    if (o->kind == CE_OBJ_BITMAP || o->kind == CE_OBJ_ICON)
        bitmap_free(o->data);
    else if (o->kind == CE_OBJ_IMAGE_LIST) {
        struct image_list *l = o->data;
        if (l) {
            free(l->px);
            free(l->opaque);
        }
        free(l);
    } else if (o->kind == CE_OBJ_FONT || o->kind == CE_OBJ_REGION || o->kind == CE_OBJ_PALETTE)
        free(o->data);
    memset(o, 0, sizeof(*o));
    return 1;
}
CE_API(SelectObject)
{
    stock_init();
    struct dc *d = dc_get(a0);
    struct ce_object *o = ce_object(a1, 0);
    if (!d || !o)
        return 0;
    CW *slot;
    switch (o->kind) {
    case CE_OBJ_FONT:
        slot = &d->font;
        break;
    case CE_OBJ_BRUSH:
        slot = &d->brush;
        break;
    case CE_OBJ_PEN:
        slot = &d->pen;
        break;
    case CE_OBJ_BITMAP:
        if (!is_memory_dc(d) || !bitmap_owned(o->data))
            return 0;
        slot = &d->bitmap;
        break;
    case CE_OBJ_REGION:
        d->clipped = true;
        d->clip = *(const struct ce_rect *)o->data;
        return 2; /* SIMPLEREGION */
    default:
        return 0;
    }
    CW old = *slot;
    *slot = a1;
    if (!old)
        old = o->kind == CE_OBJ_FONT ? stock_font : o->kind == CE_OBJ_BRUSH ? stock_brushes[0] : stock_pens[1];
    return old;
}
CE_API(GetCurrentObject)
{
    stock_init();
    struct dc *d = dc_get(a0);
    if (!d)
        return 0;
    switch (a1) {
    case 1: /* OBJ_PEN */
        return d->pen ? d->pen : stock_pens[1];
    case 2: /* OBJ_BRUSH */
        return d->brush ? d->brush : stock_brushes[0];
    case 5: /* OBJ_PAL */
        return d->palette;
    case 6: /* OBJ_FONT */
        return d->font ? d->font : stock_font;
    case 7: /* OBJ_BITMAP */
        return is_memory_dc(d) ? d->bitmap : 0;
    default:
        return 0;
    }
}
CE_API(GetObjectW)
{
    struct ce_object *o = ce_object(a0, 0);
    if (!o)
        return 0;
    uint8_t out[92] = {0};
    unsigned bytes;
    switch (o->kind) {
    case CE_OBJ_BITMAP: {
        const struct ce_bitmap *b = o->data;
        uint32_t stride = (uint32_t)(b->stride < 0 ? -b->stride : b->stride);
        const uint8_t *lowest = b->stride < 0 ? b->bits + b->stride * (b->h - 1) : b->bits;
        ce_put32(out + 4, (CW)b->w);
        ce_put32(out + 8, (CW)b->h);
        ce_put32(out + 12, stride);
        ce_put16(out + 16, 1);
        ce_put16(out + 18, (uint16_t)b->bpp);
        ce_put32(out + 20, b->ddb ? 0 : WORD(lowest));
        bytes = 24;
        if (a1 >= 84 && !b->ddb) { /* DIBSECTION */
            ce_put32(out + 24, 40);
            ce_put32(out + 28, (CW)b->w);
            ce_put32(out + 32, (CW)(b->stride < 0 ? b->h : -b->h));
            ce_put16(out + 36, 1);
            ce_put16(out + 38, (uint16_t)b->bpp);
            ce_put32(out + 40, b->masks[0] && !(b->bpp == 16 && !b->rgb565 && b->masks[0] == 0x7c00) ? 3 : 0);
            ce_put32(out + 44, stride * (uint32_t)b->h);
            ce_put32(out + 56, b->colors);
            for (unsigned c = 0; c < 3; c++)
                ce_put32(out + 64 + c * 4, b->masks[c]);
            bytes = 84;
        }
        break;
    }
    case CE_OBJ_FONT:
        if (o->data)
            memcpy(out, o->data, 92);
        else {
            ce_put32(out, (CW)-12);
            ce_put32(out + 16, 400);
        }
        bytes = 92;
        break;
    case CE_OBJ_PEN: /* LOGPEN */
        ce_put32(out, (CW)o->height);
        ce_put32(out + 4, (CW)o->width);
        ce_put32(out + 12, colorref(o->value));
        bytes = 16;
        break;
    case CE_OBJ_BRUSH: /* LOGBRUSH */
        ce_put32(out, o->value == 0xffffffffu ? 1 : 0);
        ce_put32(out + 4, colorref(o->value));
        bytes = 12;
        break;
    case CE_OBJ_PALETTE:
        ce_put16(out, (uint16_t)o->bytes);
        bytes = 2;
        break;
    default:
        return 0;
    }
    if (!a2)
        return bytes;
    unsigned n = MIN(a1, bytes);
    memcpy(PTR(void, a2), out, n);
    return n;
}
CE_API(CreateCompatibleBitmap)
{
    struct ce_bitmap *b = bitmap_new((int)a1, (int)a2, 16, true, 2);
    if (b)
        b->ddb = true;
    return bitmap_object(b);
}
CE_API(CreateBitmap)
{
    if (a2 != 1 || (a3 != 1 && a3 != 16)) {
        ce_error = 87;
        return 0;
    }
    struct ce_bitmap *b = bitmap_new((int)a0, (int)a1, (int)a3, true, 2);
    if (!b)
        return 0;
    b->ddb = true;
    if (a4)
        memcpy(b->block, PTR(void, a4), (size_t)b->stride * (size_t)b->h);
    return bitmap_object(b);
}
CE_API(CreateDIBSection)
{
    if (a3)
        *PTR(CW, a3) = 0;
    if (a4 || a5) { /* a section handle and offset: file mappings */
        ce_error = 50;
        return 0;
    }
    if (!a1) {
        ce_error = 87;
        return 0;
    }
    const uint8_t *info = PTR(uint8_t, a1);
    uint32_t palette[256], *colors = NULL;
    if (a2 == 1) { /* DIB_PAL_COLORS: indices into the DC's palette */
        struct dc *d = dc_get(a0);
        struct ce_object *p = ce_object(d ? d->palette : 0, CE_OBJ_PALETTE);
        unsigned bpp = ce_u16(info + 14), n = bpp <= 8 ? 1u << bpp : 0;
        for (unsigned i = 0; i < n; i++) {
            unsigned index = ce_u16(info + 40 + i * 2);
            palette[i] = p && index < p->bytes ? ((uint32_t *)p->data)[index] : (index & 1) * 0xffffff;
        }
        colors = palette;
    }
    struct ce_bitmap *b = bitmap_from_dib(info, 40 + 256 * 4, NULL, 0, colors, true);
    if (!b) {
        ce_error = 87;
        return 0;
    }
    if (a3)
        *PTR(CW, a3) = WORD(b->stride < 0 ? b->bits + b->stride * (b->h - 1) : b->bits);
    return bitmap_object(b);
}
CE_API(LoadBitmapW)
{
    return resource_bitmap(a0, a1);
}
CE_API(LoadImageW)
{
    if (a1 > 65535 || a2 > 2) {
        ce_error = 50;
        return 0;
    }
    if (a2 == 0) {
        CW handle = resource_bitmap(a0, a1);
        if (!handle)
            ce_error = 1813;
        return handle;
    }
    if (a2 == 1) {
        CW handle = resource_icon(a0, a1);
        if (!handle)
            ce_error = 1813;
        return handle;
    }
    uint32_t size; /* cursors are accepted but never drawn */
    if (!ce_resource(a0, 12, (uint16_t)a1, &size)) {
        ce_error = 1813;
        return 0;
    }
    return ce_new_object(CE_OBJ_ICON, a2, NULL, 0);
}
CE_API(DestroyIcon)
{
    return ce_object(a0, CE_OBJ_ICON) ? ce_DeleteObject(a0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) : 0;
}

/* ---- regions and palettes ---------------------------------------------- */

static CW region(int l, int t, int r, int b)
{
    struct ce_rect *rect = malloc(sizeof(*rect));
    if (!rect)
        return 0;
    *rect = (struct ce_rect){l, t, r, b};
    CW handle = ce_new_object(CE_OBJ_REGION, 0, rect, sizeof(*rect));
    if (!handle)
        free(rect);
    return handle;
}
CE_API(CreateRectRgn)
{
    int32_t x1 = (int32_t)a0, y1 = (int32_t)a1, x2 = (int32_t)a2, y2 = (int32_t)a3;
    if (x1 < -32768 || y1 < -32768 || x2 < -32768 || y2 < -32768 || x1 > 32767 || y1 > 32767 ||
        x2 > 32767 || y2 > 32767) {
        ce_error = 87;
        return 0;
    }
    return region(MIN(x1, x2), MIN(y1, y2), MAX(x1, x2), MAX(y1, y2));
}
CE_API(CreateRectRgnIndirect)
{
    const struct ce_rect *r = PTR(struct ce_rect, a0);
    return r ? region(r->left, r->top, r->right, r->bottom) : 0;
}
CE_API(SetRectRgn)
{
    struct ce_object *o = ce_object(a0, CE_OBJ_REGION);
    if (!o)
        return 0;
    *(struct ce_rect *)o->data = (struct ce_rect){(int)a1, (int)a2, (int)a3, (int)a4};
    return 1;
}
CE_API(SelectClipRgn)
{
    struct dc *d = dc_get(a0);
    if (!d)
        return 0;
    struct ce_object *o = a1 ? ce_object(a1, CE_OBJ_REGION) : NULL;
    if (a1 && !o) {
        ce_error = 6;
        return 0;
    }
    d->clipped = o != NULL;
    if (!o)
        return 2; /* SIMPLEREGION: the whole surface */
    d->clip = *(const struct ce_rect *)o->data;
    return d->clip.right <= d->clip.left || d->clip.bottom <= d->clip.top ? 1 : 2; /* NULLREGION */
}
CE_API(CreatePalette)
{
    const uint8_t *p = PTR(uint8_t, a0);
    unsigned count = p ? ce_u16(p + 2) : 0;
    if (!count || count > 256)
        return 0;
    uint32_t *entries = malloc(count * 4);
    if (!entries)
        return 0;
    for (unsigned i = 0; i < count; i++)
        entries[i] = (uint32_t)p[4 + i * 4] << 16 | p[5 + i * 4] << 8 | p[6 + i * 4];
    CW handle = ce_new_object(CE_OBJ_PALETTE, 0, entries, count);
    if (!handle)
        free(entries);
    return handle;
}
CE_API(SelectPalette)
{
    struct dc *d = dc_get(a0);
    if (!d || !ce_object(a1, CE_OBJ_PALETTE))
        return 0;
    CW old = d->palette;
    d->palette = a1;
    return old;
}
CE_API(RealizePalette)
{
    return 0;
}
CE_API(GetPaletteEntries)
{
    struct ce_object *o = ce_object(a0, CE_OBJ_PALETTE);
    if (!o || a1 >= o->bytes)
        return 0;
    unsigned n = MIN(a2, o->bytes - a1);
    for (unsigned i = 0; a3 && i < n; i++) {
        uint32_t rgb = ((uint32_t *)o->data)[a1 + i];
        uint8_t *e = PTR(uint8_t, a3 + i * 4);
        e[0] = (uint8_t)(rgb >> 16);
        e[1] = (uint8_t)(rgb >> 8);
        e[2] = (uint8_t)rgb;
        e[3] = 0;
    }
    return n;
}
/* The display is RGB565, so the nearest colour is its 565 rounding. */
CE_API(GetNearestColor)
{
    return colorref(rgb_of565(RGB565(ce_color(a1))));
}

/* ---- drawing ------------------------------------------------------------ */

CE_API(FillRect)
{
    struct target t;
    if (!a1 || !begin(a0, &t))
        return 0;
    struct ce_object *brush = ce_object(a2, CE_OBJ_BRUSH);
    /* A brush may also be COLOR_* + 1. */
    CW rgb = brush ? brush->value
             : a2 && a2 <= 32 ? ce_color(ce_GetSysColor(a2 - 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
                              : 0xc0c0c0;
    const struct ce_rect *r = PTR(struct ce_rect, a1);
    if (rgb != 0xffffffffu)
        fill(&t, r->left, r->top, r->right - r->left, r->bottom - r->top, rgb);
    end(&t);
    return 1;
}
CE_API(InvertRect)
{
    struct target t;
    if (!a1 || !begin(a0, &t))
        return 0;
    const struct ce_rect *r = PTR(struct ce_rect, a1);
    for (int y = r->top; y < r->bottom; y++)
        for (int x = r->left; x < r->right; x++)
            plot(&t, t.x + x, t.y + y, 0, 6);
    end(&t);
    return 1;
}
CE_API(DrawFocusRect)
{
    struct target t;
    if (!a1 || !begin(a0, &t))
        return 0;
    const struct ce_rect *r = PTR(struct ce_rect, a1);
    for (int x = r->left; x < r->right; x += 2) {
        plot(&t, t.x + x, t.y + r->top, 0xffffff, 7);
        plot(&t, t.x + x, t.y + r->bottom - 1, 0xffffff, 7);
    }
    for (int y = r->top + 2; y < r->bottom - 1; y += 2) {
        plot(&t, t.x + r->left, t.y + y, 0xffffff, 7);
        plot(&t, t.x + r->right - 1, t.y + y, 0xffffff, 7);
    }
    end(&t);
    return 1;
}
static void outline(struct target *t, const struct ce_object *pen, int l, int top, int r, int b)
{
    if (!pen || pen->height == 5) /* PS_NULL */
        return;
    for (int s = 0; s < pen->width; s++) {
        line(t, l + s, top + s, r - 1 - s, top + s, pen->value, t->dc->rop2, true);
        line(t, l + s, b - 1 - s, r - 1 - s, b - 1 - s, pen->value, t->dc->rop2, true);
        line(t, l + s, top + s, l + s, b - 1 - s, pen->value, t->dc->rop2, true);
        line(t, r - 1 - s, top + s, r - 1 - s, b - 1 - s, pen->value, t->dc->rop2, true);
    }
}
CE_API(Rectangle)
{
    struct target t;
    if (!begin(a0, &t))
        return 0;
    int l = (int)a1, top = (int)a2, r = (int)a3, b = (int)a4;
    uint32_t rgb = brush_color(t.dc);
    if (rgb != 0xffffffffu)
        fill(&t, l, top, r - l, b - top, rgb);
    outline(&t, pen_of(t.dc), l, top, r, b);
    end(&t);
    return 1;
}
CE_API(Ellipse)
{
    struct target t;
    if (!begin(a0, &t))
        return 0;
    int l = (int)a1, top = (int)a2, r = (int)a3, b = (int)a4;
    uint32_t rgb = brush_color(t.dc);
    const struct ce_object *pen = pen_of(t.dc);
    int64_t rx = (r - l) / 2, ry = (b - top) / 2;
    int cx = l + (int)rx, cy = top + (int)ry;
    for (int y = -(int)ry; rx > 0 && y <= (int)ry; y++) {
        /* The row's half width from x^2/rx^2 + y^2/ry^2 <= 1. */
        int64_t limit = rx * rx * (ry * ry - (int64_t)y * y);
        int half = 0;
        while ((int64_t)(half + 1) * (half + 1) * ry * ry <= limit)
            half++;
        if (rgb != 0xffffffffu)
            fill(&t, cx - half, cy + y, 2 * half + 1, 1, rgb);
        if (pen && pen->height != 5) {
            plot(&t, t.x + cx - half, t.y + cy + y, pen->value, t.dc->rop2);
            plot(&t, t.x + cx + half, t.y + cy + y, pen->value, t.dc->rop2);
        }
    }
    end(&t);
    return 1;
}
CE_API(Polyline)
{
    struct target t;
    if (a2 > 4096 || !begin(a0, &t))
        return 0;
    const struct ce_object *pen = pen_of(t.dc);
    int32_t *p = PTR(int32_t, a1);
    for (unsigned i = 1; pen && pen->height != 5 && i < a2; i++) {
        bool horizontal = abs(p[i * 2] - p[(i - 1) * 2]) >= abs(p[i * 2 + 1] - p[(i - 1) * 2 + 1]);
        for (int stroke = 0; stroke < pen->width; stroke++) {
            int offset = stroke - pen->width / 2;
            int dx = horizontal ? 0 : offset, dy = horizontal ? offset : 0;
            line(&t, p[(i - 1) * 2] + dx, p[(i - 1) * 2 + 1] + dy, p[i * 2] + dx, p[i * 2 + 1] + dy, pen->value,
                 t.dc->rop2, false);
        }
    }
    end(&t);
    return 1;
}
CE_API(DrawFrameControl)
{
    struct target t;
    if (!a1 || !begin(a0, &t))
        return 0;
    const struct ce_rect *r = PTR(struct ce_rect, a1);
    uint32_t top = (a3 & 0x200) ? 0x404040 : 0xffffff, bottom = (a3 & 0x200) ? 0xffffff : 0x404040;
    fill(&t, r->left, r->top, r->right - r->left, r->bottom - r->top, 0xc0c0c0);
    line(&t, r->left, r->top, r->right - 1, r->top, top, 13, true);
    line(&t, r->left, r->top, r->left, r->bottom - 1, top, 13, true);
    line(&t, r->left, r->bottom - 1, r->right - 1, r->bottom - 1, bottom, 13, true);
    line(&t, r->right - 1, r->top, r->right - 1, r->bottom - 1, bottom, 13, true);
    end(&t);
    return 1;
}
CE_API(GetPixel)
{
    struct dc *d = dc_get(a0);
    if (!d)
        return 0xffffffffu;
    int x = (int)a1, y = (int)a2;
    if (is_memory_dc(d)) {
        struct ce_bitmap *b = bitmap(d->bitmap);
        if (!b || x < 0 || y < 0 || x >= b->w || y >= b->h)
            return 0xffffffffu;
        return colorref(read_pixel(b, x, y));
    }
    gfx_rect r = window_bounds(d->window);
    x += r.x;
    y += r.y;
    if (x < 0 || y < 0 || x >= ui.surf.w || y >= ui.surf.h)
        return 0xffffffffu;
    return colorref(rgb_of565(ui.surf.px[y * ui.surf.stride + x]));
}
CE_API(SetPixel)
{
    struct target t;
    if (!begin(a0, &t))
        return 0xffffffffu;
    plot(&t, t.x + (int)a1, t.y + (int)a2, ce_color(a3), 13);
    end(&t);
    return a3;
}
CE_API(SetTextColor)
{
    struct dc *d = dc_get(a0);
    if (!d)
        return 0xffffffffu;
    CW old = d->text;
    d->text = ce_color(a1);
    return colorref(old);
}
CE_API(SetBkColor)
{
    struct dc *d = dc_get(a0);
    if (!d)
        return 0xffffffffu;
    CW old = d->back;
    d->back = ce_color(a1);
    return colorref(old);
}
CE_API(SetBkMode)
{
    struct dc *d = dc_get(a0);
    if (!d)
        return 0;
    int old = d->mode;
    d->mode = (int)a1;
    return (CW)old;
}
CE_API(SetROP2)
{
    struct dc *d = dc_get(a0);
    if (!d)
        return 0;
    int old = d->rop2;
    d->rop2 = (int)a1;
    return (CW)old;
}

/* ---- text --------------------------------------------------------------- */

/* Glyphs are Latin-1; other characters draw as '?'. */
static char glyph(WCHAR c)
{
    return (char)(c < 256 ? c : '?');
}
static int text_width(const gfx_font *f, const WCHAR *text, unsigned n)
{
    int width = 0;
    for (unsigned i = 0; i < n; i++) {
        char s[2] = {glyph(text[i]), 0};
        width += gfx_text_width(f, s);
    }
    return width;
}
/* n characters at DC coordinates, at the advances in `dx` when given, over
 * the background colour when the DC is OPAQUE. Returns the width. */
static int text_out(struct target *t, int x, int y, const WCHAR *text, unsigned n, const int32_t *dx)
{
    const gfx_font *f = dc_font(t->dc);
    int width = 0;
    for (unsigned i = 0; i < n; i++)
        width += dx ? dx[i] : text_width(f, text + i, 1);
    if (t->dc->mode == 2)
        fill(t, x, y, width, f->line_height, t->dc->back);
    int pen = x;
    for (unsigned i = 0; i < n; i++) {
        char s[2] = {glyph(text[i]), 0};
        int advance = gfx_text(t->s, t->x + pen, t->y + y, f, s, t->dc->text, 255);
        pen += dx ? dx[i] : advance;
    }
    return width;
}
CE_API(TextOutW)
{
    struct target t;
    if (a4 > 65535 || !begin(a0, &t))
        return 0;
    text_out(&t, (int)a1, (int)a2, PTR(WCHAR, a3), a4, NULL);
    end(&t);
    return 1;
}
static void clip_to(struct target *t, const struct ce_rect *r)
{
    gfx_rect c;
    if (!gfx_rect_intersect(t->s->clip, gfx_r(t->x + r->left, t->y + r->top, r->right - r->left, r->bottom - r->top),
                            &c))
        c = gfx_r(0, 0, 0, 0);
    t->s->clip = c;
}
CE_API(ExtTextOutW)
{
    struct target t;
    if (a6 > 65535 || !begin(a0, &t))
        return 0;
    const struct ce_rect *r = PTR(struct ce_rect, a4);
    if (r && (a3 & 2)) /* ETO_OPAQUE */
        fill(&t, r->left, r->top, r->right - r->left, r->bottom - r->top, t.dc->back);
    if (r && (a3 & 4)) /* ETO_CLIPPED */
        clip_to(&t, r);
    int mode = t.dc->mode;
    if (r && (a3 & 2))
        t.dc->mode = 1; /* the rectangle already holds the background */
    if (a5)
        text_out(&t, (int)a1, (int)a2, PTR(WCHAR, a5), a6, a7 ? PTR(int32_t, a7) : NULL);
    t.dc->mode = mode;
    end(&t);
    return 1;
}
CW ce_draw_text(CW a0, CW a1, CW a2, CW a3, CW a4)
{
    if (!a1 || !a3)
        return 0;
    unsigned length = a2 == 0xffffffffu ? ce_wlen(PTR(WCHAR, a1)) : a2;
    if (!length || length > 65535)
        return 0;
    struct target t;
    if (!begin(a0, &t))
        return 0;
    WCHAR *source = PTR(WCHAR, a1);
    struct ce_rect *rect = PTR(struct ce_rect, a3);
    const gfx_font *font = dc_font(t.dc);
    int line_height = font->line_height;
    int width = MAX(1, rect->right - rect->left), y = rect->top, widest = 0, lines = 0;
    if ((a4 & 0x24) == 0x24) /* DT_SINGLELINE | DT_VCENTER */
        y += (rect->bottom - rect->top - line_height) / 2;
    else if ((a4 & 0x28) == 0x28) /* DT_SINGLELINE | DT_BOTTOM */
        y = rect->bottom - line_height;
    if (!(a4 & (0x100 | 0x400))) /* DT_NOCLIP, DT_CALCRECT */
        clip_to(&t, rect);
    for (unsigned pos = 0; pos < length && source[pos];) {
        WCHAR line_text[512];
        unsigned count = 0, word = 0, word_pos = pos;
        int advance = 0, word_width = 0;
        while (pos < length && source[pos] && count + 1 < ARRAY_SIZE(line_text)) {
            WCHAR ch = source[pos];
            if (!(a4 & 0x20) && (ch == '\r' || ch == '\n')) {
                pos++;
                if (ch == '\r' && pos < length && source[pos] == '\n')
                    pos++;
                break;
            }
            if (ch == '\r' || ch == '\n' || ch == '\t')
                ch = ' ';
            int next = text_width(font, &ch, 1);
            if ((a4 & 0x10) && !(a4 & 0x20) && advance + next > width && count) { /* DT_WORDBREAK */
                if (word) {
                    count = word;
                    pos = word_pos;
                    advance = word_width;
                }
                break;
            }
            if (ch == '&' && !(a4 & 0x800)) { /* DT_NOPREFIX */
                pos++;
                if (pos >= length || source[pos] != '&')
                    continue;
            }
            line_text[count++] = ch;
            advance += next;
            pos++;
            if (ch == ' ') {
                word = count;
                word_pos = pos;
                word_width = advance;
            }
        }
        widest = MAX(widest, advance);
        if (!(a4 & 0x400)) {
            int x = rect->left;
            if (a4 & 1)
                x += (width - advance) / 2;
            else if (a4 & 2)
                x += width - advance;
            text_out(&t, x, y, line_text, count, NULL);
        }
        y += line_height;
        lines++;
    }
    if (a4 & 0x400) {
        rect->right = rect->left + widest;
        rect->bottom = rect->top + lines * line_height;
    }
    end(&t);
    return (CW)(lines * line_height);
}
CE_API(DrawTextW)
{
    return ce_draw_text(a0, a1, a2, a3, a4);
}
CE_API(GetTextExtentExPointW)
{
    if (!a1 || !a6 || a2 > 65535)
        return 0;
    const gfx_font *f = dc_font(dc_get(a0));
    int width = 0;
    unsigned fit = 0;
    const WCHAR *source = PTR(WCHAR, a1);
    for (unsigned i = 0; i < a2; i++) {
        width += text_width(f, source + i, 1);
        if (a5)
            PTR(int32_t, a5)[i] = width;
        if (width <= (int32_t)a3)
            fit = i + 1;
    }
    if (a4)
        *PTR(CW, a4) = fit;
    PTR(int32_t, a6)[0] = width;
    PTR(int32_t, a6)[1] = f->line_height;
    return 1;
}
CE_API(GetTextExtentPoint32W)
{
    if (!a1 || !a3 || a2 > 65535)
        return 0;
    const gfx_font *f = dc_font(dc_get(a0));
    PTR(int32_t, a3)[0] = text_width(f, PTR(WCHAR, a1), a2);
    PTR(int32_t, a3)[1] = f->line_height;
    return 1;
}

/* ---- blits -------------------------------------------------------------- */

/* A blit source: a bitmap of any depth, or the screen. */
struct source {
    const struct ce_bitmap *bitmap;
    const gfx_surface *screen;
    int x, y;         /* the screen origin of a window DC */
    uint32_t mono[2]; /* colours of a mono DDB's 0 and 1 bits */
    bool mono_ddb;
};
static bool source_of(CW handle, struct source *s, const struct dc *dst)
{
    memset(s, 0, sizeof(*s));
    const struct ce_bitmap *b = bitmap(handle); /* TransparentImage takes a bitmap too */
    struct dc *d = b ? NULL : dc_get(handle);
    if (d && is_memory_dc(d))
        b = bitmap(d->bitmap);
    if (b) {
        s->bitmap = b;
        /* A mono DDB takes the destination's text (0) and background (1). */
        s->mono_ddb = b->ddb && b->bpp == 1;
        s->mono[0] = dst->text;
        s->mono[1] = dst->back;
        return true;
    }
    if (!d)
        return false;
    gfx_rect r = window_bounds(d->window);
    s->screen = &ui.surf;
    s->x = r.x;
    s->y = r.y;
    return true;
}
static bool source_inside(const struct source *s, int x, int y)
{
    if (s->screen)
        return s->x + x >= 0 && s->y + y >= 0 && s->x + x < s->screen->w && s->y + y < s->screen->h;
    return x >= 0 && y >= 0 && x < s->bitmap->w && y < s->bitmap->h;
}
static uint16_t source_pixel(const struct source *s, int x, int y)
{
    if (s->screen)
        return s->screen->px[(s->y + y) * s->screen->stride + s->x + x];
    const struct ce_bitmap *b = s->bitmap;
    if (b->rgb565)
        return ce_u16(b->bits + y * b->stride + x * 2);
    if (s->mono_ddb)
        return RGB565(s->mono[(b->bits[y * b->stride + x / 8] >> (7 - x % 8)) & 1]);
    return RGB565(read_pixel(b, x, y));
}
/* Ternary raster operation `index` (its bits indexed by P, S, D) on
 * 16-bit pixels. */
static uint16_t raster(unsigned index, uint16_t p, uint16_t s, uint16_t d)
{
    switch (index) {
    case 0xcc:
        return s;
    case 0xf0:
        return p;
    case 0x00:
        return 0;
    case 0xff:
        return 0xffff;
    }
    uint16_t r = 0;
    for (unsigned bit = 0; bit < 8; bit++)
        if (index >> bit & 1)
            r |= (uint16_t)((bit & 4 ? p : ~p) & (bit & 2 ? s : ~s) & (bit & 1 ? d : ~d));
    return r;
}
static bool rop_uses_source(unsigned index)
{
    return ((index >> 2) & 0x33) != (index & 0x33);
}
/* Copies, scales and combines a source rectangle into a destination. A
 * `transparent` 565 colour is skipped; `mask` chooses between the fore
 * (set bits) and back raster operations, as MaskBlt does. */
static CW blit(CW dst_dc, int x, int y, int w, int h, CW src_dc, int sx, int sy, int sw, int sh, unsigned fore,
               unsigned back, int transparent, const struct ce_bitmap *mask, int mx, int my)
{
    if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0)
        return 1;
    struct dc *dst = dc_get(dst_dc);
    if (!dst)
        return 0;
    struct ce_bitmap *bm = is_memory_dc(dst) ? bitmap(dst->bitmap) : NULL;
    bool slow = bm && !bm->rgb565; /* mono or palette destinations */
    struct target t;
    if (!slow && !begin(dst_dc, &t))
        return 0;
    struct source s;
    bool need_source = rop_uses_source(fore) || rop_uses_source(back) || transparent >= 0;
    if (need_source && !source_of(src_dc, &s, dst)) {
        if (!slow)
            end(&t);
        ce_error = 6;
        return 0;
    }
    bool mono_source = need_source && s.bitmap && s.bitmap->bpp == 1;
    uint16_t pattern = RGB565(brush_color(dst));
    /* Copies within one surface (scrolling) read a snapshot. */
    uint16_t *copy = NULL;
    if (need_source && ((s.screen && !bm) || (s.bitmap && s.bitmap == bm))) {
        copy = malloc((size_t)sw * (size_t)sh * 2);
        if (!copy) {
            if (!slow)
                end(&t);
            return 0;
        }
        for (int yy = 0; yy < sh; yy++)
            for (int xx = 0; xx < sw; xx++)
                copy[yy * sw + xx] = source_inside(&s, sx + xx, sy + yy) ? source_pixel(&s, sx + xx, sy + yy) : 0;
    }
    for (int yy = 0; yy < h; yy++) {
        int src_y = sy + (int)((int64_t)yy * sh / h);
        for (int xx = 0; xx < w; xx++) {
            int src_x = sx + (int)((int64_t)xx * sw / w);
            uint16_t sp = 0;
            if (need_source) {
                if (!source_inside(&s, src_x, src_y))
                    continue;
                sp = copy ? copy[(src_y - sy) * sw + (src_x - sx)] : source_pixel(&s, src_x, src_y);
                if (transparent >= 0 && sp == transparent)
                    continue;
            }
            unsigned rop = fore;
            if (mask && (mx + xx < 0 || my + yy < 0 || mx + xx >= mask->w || my + yy >= mask->h ||
                         !(read_pixel(mask, mx + xx, my + yy) & 0xffffff)))
                rop = back;
            if (slow) {
                if (x + xx < 0 || y + yy < 0 || x + xx >= bm->w || y + yy >= bm->h)
                    continue;
                /* Colour to mono: pixels of the background colour are white. */
                if (bm->bpp == 1 && need_source && !mono_source)
                    sp = sp == RGB565(dst->back) ? 0xffff : 0;
                uint16_t d = RGB565(read_pixel(bm, x + xx, y + yy));
                write_pixel(bm, x + xx, y + yy, rgb_of565(raster(rop, pattern, sp, d)));
                continue;
            }
            int px = t.x + x + xx, py = t.y + y + yy;
            if (clipped_out(&t.s->clip, px, py))
                continue;
            uint16_t *d = &t.s->px[py * t.s->stride + px];
            *d = raster(rop, pattern, sp, *d);
        }
    }
    free(copy);
    if (!slow)
        end(&t);
    return 1;
}
CE_API(BitBlt)
{
    unsigned rop = (a8 >> 16) & 255;
    return blit(a0, (int)a1, (int)a2, (int)a3, (int)a4, a5, (int)a6, (int)a7, (int)a3, (int)a4, rop, rop, -1,
                NULL, 0, 0);
}
CE_API(PatBlt)
{
    unsigned rop = (a5 >> 16) & 255;
    if (rop_uses_source(rop)) {
        ce_error = 50; /* PatBlt has no source */
        return 0;
    }
    struct dc *d = dc_get(a0);
    if (d && rop == 0xf0 && brush_color(d) == 0xffffffffu) /* NULL_BRUSH */
        return 1;
    return blit(a0, (int)a1, (int)a2, (int)a3, (int)a4, 0, 0, 0, 1, 1, rop, rop, -1, NULL, 0, 0);
}
CE_API(StretchBlt)
{
    unsigned rop = (a10 >> 16) & 255;
    return blit(a0, (int)a1, (int)a2, (int)a3, (int)a4, a5, (int)a6, (int)a7, (int)a8, (int)a9, rop, rop, -1,
                NULL, 0, 0);
}
CE_API(TransparentImage)
{
    return blit(a0, (int)a1, (int)a2, (int)a3, (int)a4, a5, (int)a6, (int)a7, (int)a8, (int)a9, 0xcc, 0xcc,
                RGB565(ce_color(a10)), NULL, 0, 0);
}
CE_API(MaskBlt)
{
    /* MAKEROP4(fore, back): fore in bits 16-23, back in bits 24-31. */
    const struct ce_bitmap *mask = bitmap(a8);
    unsigned fore = (a11 >> 16) & 255, back = (a11 >> 24) & 255;
    return blit(a0, (int)a1, (int)a2, (int)a3, (int)a4, a5, (int)a6, (int)a7, (int)a3, (int)a4, fore,
                mask ? back : fore, -1, mask, (int)a9, (int)a10);
}
/* Moves window pixels; the window repaints the exposed strip. */
CE_API(ScrollWindowEx)
{
    struct ce_window *w = ce_window(a0);
    if (!w)
        return 0;
    gfx_rect r = ce_client_rect(a0);
    const struct ce_rect *area = PTR(struct ce_rect, a3);
    gfx_rect s = area ? gfx_r(r.x + area->left, r.y + area->top, area->right - area->left, area->bottom - area->top)
                      : r;
    if (!gfx_rect_intersect(s, r, &s))
        return 1;
    int dx = (int)a1, dy = (int)a2;
    gfx_rect moved = gfx_r(s.x + dx, s.y + dy, s.w, s.h), keep;
    if (gfx_rect_intersect(moved, s, &keep)) {
        int from = dy > 0 ? keep.h - 1 : 0, step = dy > 0 ? -1 : 1;
        for (int n = 0; n < keep.h; n++, from += step)
            memmove(ui.surf.px + (keep.y + from) * ui.surf.stride + keep.x,
                    ui.surf.px + (keep.y + from - dy) * ui.surf.stride + keep.x - dx, (size_t)keep.w * 2);
    }
    if (a6) { /* prcUpdate: the exposed strip, in client coordinates */
        struct ce_rect *update = PTR(struct ce_rect, a6);
        *update = (struct ce_rect){s.x - r.x, s.y - r.y, s.x - r.x + s.w, s.y - r.y + s.h};
        if (dy > 0)
            update->bottom = update->top + dy;
        else if (dy < 0)
            update->top = update->bottom + dy;
        if (dx > 0)
            update->right = update->left + dx;
        else if (dx < 0)
            update->left = update->right + dx;
    }
    if (a7 & 2) /* SW_INVALIDATE */
        w->dirty = true;
    ce_screen_dirty = true;
    return 2; /* SIMPLEREGION */
}

/* ---- resources and image lists ----------------------------------------- */

CE_API(LoadStringW)
{
    uint32_t size;
    const uint8_t *p = ce_resource(a0, 6, (uint16_t)((a1 >> 4) + 1), &size);
    if (!p)
        return 0;
    uint32_t off = 0;
    for (unsigned i = 0; i < (a1 & 15); i++) {
        if (!ce_range(off, 2, size))
            return 0;
        uint32_t n = ce_u16(p + off);
        off += 2 + n * 2;
    }
    if (!ce_range(off, 2, size))
        return 0;
    uint32_t n = ce_u16(p + off);
    off += 2;
    if (!ce_range(off, n * 2, size) || !a3)
        return 0;
    n = MIN(n, a3 - 1);
    memcpy(PTR(void, a2), p + off, n * 2);
    PTR(WCHAR, a2)[n] = 0;
    return n;
}
CE_API(FindResourceW)
{
    uint32_t size;
    return WORD(ce_resource(a0, (uint16_t)a2, (uint16_t)a1, &size));
}
CE_API(LoadResource)
{
    return a1;
}
CE_API(LockResource)
{
    return a0;
}
CE_API(LoadCursorW)
{
    return a1 ? a1 : 32512;
}
CE_API(SetCursor)
{
    static CW cursor;
    CW old = cursor;
    cursor = a0;
    return old;
}
CE_API(LoadIconW)
{
    return ce_LoadImageW(a0, a1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
static struct image_list *image_list(CW handle)
{
    struct ce_object *o = ce_object(handle, CE_OBJ_IMAGE_LIST);
    return o ? o->data : NULL;
}
static CW image_list_new(int cx, int cy)
{
    if (cx <= 0 || cy <= 0 || cx > 256 || cy > 256)
        return 0;
    struct image_list *l = calloc(1, sizeof(*l));
    if (!l)
        return 0;
    l->cx = cx;
    l->cy = cy;
    CW handle = ce_new_object(CE_OBJ_IMAGE_LIST, 0, l, sizeof(*l));
    if (!handle)
        free(l);
    return handle;
}
/* Puts each cx-wide image of a bitmap at `index`, appending at `count`.
 * Pixels of the `key` colour (or none, -1), set bits of an icon's AND mask
 * and white pixels of a `mask` bitmap are transparent. Pixels are copied,
 * since the caller may delete the bitmap. */
static int image_list_put(struct image_list *l, unsigned index, const struct ce_bitmap *b, int64_t key,
                          const struct ce_bitmap *mask)
{
    unsigned added = (unsigned)MAX(1, b->w / l->cx);
    if (index > l->count || index + added > 4096)
        return -1;
    if (index + added > l->capacity) {
        unsigned capacity = MAX(index + added, l->capacity * 2);
        size_t pixels = (size_t)capacity * (size_t)l->cx * (size_t)l->cy;
        uint16_t *px = calloc(pixels, 2);
        uint8_t *opaque = calloc(pixels, 1);
        if (!px || !opaque) {
            free(px);
            free(opaque);
            return -1;
        }
        for (int y = 0; l->px && y < l->cy; y++) {
            memcpy(px + (size_t)y * capacity * l->cx, l->px + (size_t)y * l->capacity * l->cx,
                   (size_t)l->count * l->cx * 2);
            memcpy(opaque + (size_t)y * capacity * l->cx, l->opaque + (size_t)y * l->capacity * l->cx,
                   (size_t)l->count * l->cx);
        }
        free(l->px);
        free(l->opaque);
        l->px = px;
        l->opaque = opaque;
        l->capacity = capacity;
    }
    size_t stride = (size_t)l->capacity * (size_t)l->cx;
    for (int y = 0; y < l->cy; y++)
        for (int x = 0; x < (int)added * l->cx; x++) {
            size_t at = (size_t)y * stride + (size_t)index * l->cx + (size_t)x;
            if (x >= b->w || y >= b->h) {
                l->opaque[at] = 0;
                continue;
            }
            uint16_t px = RGB565(read_pixel(b, x, y));
            l->px[at] = px;
            l->opaque[at] = (key < 0 || px != RGB565((uint32_t)key)) && !icon_transparent(b, x, y) &&
                            !(mask && x < mask->w && y < mask->h && (read_pixel(mask, x, y) & 0xffffff));
        }
    l->count = MAX(l->count, index + added);
    return (int)index;
}
CE_API(ImageList_Create)
{
    return image_list_new((int)a0, (int)a1);
}
CE_API(ImageList_Add)
{
    struct image_list *l = image_list(a0);
    const struct ce_bitmap *b = bitmap(a1);
    if (!l || !b)
        return 0xffffffffu;
    return (CW)image_list_put(l, l->count, b, -1, a2 ? bitmap(a2) : NULL);
}
CE_API(ImageList_AddMasked)
{
    struct image_list *l = image_list(a0);
    const struct ce_bitmap *b = bitmap(a1);
    if (!l || !b)
        return 0xffffffffu;
    return (CW)image_list_put(l, l->count, b, ce_color(a2), NULL);
}
/* Index -1 appends. */
CE_API(ImageList_ReplaceIcon)
{
    struct image_list *l = image_list(a0);
    struct ce_object *icon = ce_object(a2, CE_OBJ_ICON);
    if (!l || !icon || !icon->data || (a1 != 0xffffffffu && a1 >= l->count))
        return 0xffffffffu;
    return (CW)image_list_put(l, a1 == 0xffffffffu ? l->count : a1, icon->data, -1, NULL);
}
CE_API(ImageList_GetImageCount)
{
    struct image_list *l = image_list(a0);
    return l ? l->count : 0;
}
/* uType (a5) IMAGE_ICON loads an icon; a cx (a2) of 0 takes its width. */
CE_API(ImageList_LoadImage)
{
    CW bm = a5 == 1 ? resource_icon(a0, a1) : resource_bitmap(a0, a1);
    struct ce_object *o = ce_object(bm, 0);
    const struct ce_bitmap *b = o ? o->data : NULL;
    if (!b) {
        if (o)
            ce_DeleteObject(bm, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        return 0;
    }
    CW handle = image_list_new(a2 ? (int)a2 : b->w, b->h);
    struct image_list *l = image_list(handle);
    if (l && image_list_put(l, 0, b, a4 == 0xffffffffu ? -1 : (int64_t)ce_color(a4), NULL) < 0) {
        ce_DeleteObject(handle, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        handle = 0;
    }
    ce_DeleteObject(bm, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    return handle;
}
CE_API(ImageList_Draw)
{
    struct image_list *l = image_list(a0);
    struct target t;
    if (!l || a1 >= l->count || !begin(a2, &t))
        return 0;
    size_t stride = (size_t)l->capacity * (size_t)l->cx;
    for (int y = 0; y < l->cy; y++)
        for (int x = 0; x < l->cx; x++) {
            size_t at = (size_t)y * stride + (size_t)a1 * l->cx + (size_t)x;
            int px = t.x + (int)a3 + x, py = t.y + (int)a4 + y;
            if ((!l->opaque[at] && (a5 & 1)) || clipped_out(&t.s->clip, px, py)) /* ILD_TRANSPARENT */
                continue;
            t.s->px[py * t.s->stride + px] = l->px[at];
        }
    end(&t);
    return 1;
}
CE_API(ImageList_Destroy)
{
    return image_list(a0) ? ce_DeleteObject(a0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) : 0;
}

static const struct ce_export exports[] = {
    CE_EXPORT(BeginPaint),
    CE_EXPORT(EndPaint),
    CE_EXPORT(GetDC),
    {"GetWindowDC", (void *)ce_GetDC},
    {"GetDCEx", (void *)ce_GetDC},
    CE_EXPORT(ReleaseDC),
    CE_EXPORT(CreateCompatibleDC),
    CE_EXPORT(DeleteDC),
    CE_EXPORT(GetDeviceCaps),
    CE_EXPORT(GetSysColor),
    CE_EXPORT(GetSysColorBrush),
    CE_EXPORT(GetStockObject),
    CE_EXPORT(CreateSolidBrush),
    CE_EXPORT(CreatePen),
    CE_EXPORT(CreatePenIndirect),
    CE_EXPORT(CreateFontIndirectW),
    CE_EXPORT(GetTextMetricsW),
    CE_EXPORT(EnumFontFamiliesW),
    CE_EXPORT(DeleteObject),
    CE_EXPORT(SelectObject),
    CE_EXPORT(GetCurrentObject),
    CE_EXPORT(GetObjectW),
    CE_EXPORT(CreateCompatibleBitmap),
    CE_EXPORT(CreateBitmap),
    CE_EXPORT(CreateDIBSection),
    CE_EXPORT(LoadBitmapW),
    CE_EXPORT(LoadImageW),
    CE_EXPORT(CreateRectRgn),
    CE_EXPORT(CreateRectRgnIndirect),
    CE_EXPORT(SetRectRgn),
    CE_EXPORT(SelectClipRgn),
    CE_EXPORT(CreatePalette),
    CE_EXPORT(SelectPalette),
    CE_EXPORT(RealizePalette),
    CE_EXPORT(GetPaletteEntries),
    CE_EXPORT(GetNearestColor),
    CE_EXPORT(FillRect),
    CE_EXPORT(InvertRect),
    CE_EXPORT(DrawFocusRect),
    CE_EXPORT(Rectangle),
    CE_EXPORT(Ellipse),
    CE_EXPORT(Polyline),
    CE_EXPORT(DrawFrameControl),
    CE_EXPORT(GetPixel),
    CE_EXPORT(SetPixel),
    CE_EXPORT(SetTextColor),
    CE_EXPORT(SetBkColor),
    CE_EXPORT(SetBkMode),
    CE_EXPORT(SetROP2),
    CE_EXPORT(TextOutW),
    CE_EXPORT(ExtTextOutW),
    CE_EXPORT(DrawTextW),
    CE_EXPORT(GetTextExtentExPointW),
    CE_EXPORT(GetTextExtentPoint32W),
    CE_EXPORT(BitBlt),
    CE_EXPORT(PatBlt),
    CE_EXPORT(StretchBlt),
    CE_EXPORT(TransparentImage),
    CE_EXPORT(MaskBlt),
    CE_EXPORT(ScrollWindowEx),
    CE_EXPORT(LoadStringW),
    CE_EXPORT(FindResourceW),
    CE_EXPORT(LoadResource),
    CE_EXPORT(LockResource),
    CE_EXPORT(LoadCursorW),
    CE_EXPORT(SetCursor),
    CE_EXPORT(LoadIconW),
    CE_EXPORT(DestroyIcon),
    CE_EXPORT(ImageList_Create),
    CE_EXPORT(ImageList_Add),
    CE_EXPORT(ImageList_AddMasked),
    CE_EXPORT(ImageList_ReplaceIcon),
    CE_EXPORT(ImageList_GetImageCount),
    CE_EXPORT(ImageList_LoadImage),
    CE_EXPORT(ImageList_Draw),
    CE_EXPORT(ImageList_Destroy),
};
void *ce_resolve_gdi(const char *name)
{
    stock_init();
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return NULL;
}
