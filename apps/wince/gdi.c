/* RGB565 GDI adapter; bitmap payloads are the original PE resources. */
#include "runtime.h"

static struct ce_object objects[192];
static CW text_color, background_color = 0xffffff, selected_font, selected_brush, selected_pen;
static int background_mode = 2;
#define CE_IMAGES 256
struct image_list {
    unsigned count;
    struct ce_object image[CE_IMAGES];
};

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
            objects[i] = (struct ce_object){kind, value, data, bytes, 0, 0};
            return 0x76000000u + i;
        }
    ce_error = 8;
    return 0;
}
uint32_t ce_color(CW color)
{
    return (color & 255) << 16 | (color & 0xff00) | (color >> 16 & 255);
}

static gfx_rect rectangle(CW dc, const struct ce_rect *rect)
{
    gfx_rect r = ce_client_rect(dc);
    return gfx_r(r.x + rect->left, r.y + rect->top, rect->right - rect->left, rect->bottom - rect->top);
}

static void border(gfx_rect r, uint32_t top, uint32_t bottom)
{
    gfx_line(&ui.surf, r.x, r.y, r.x + r.w - 1, r.y, top, 255);
    gfx_line(&ui.surf, r.x, r.y, r.x, r.y + r.h - 1, top, 255);
    gfx_line(&ui.surf, r.x, r.y + r.h - 1, r.x + r.w - 1, r.y + r.h - 1, bottom, 255);
    gfx_line(&ui.surf, r.x + r.w - 1, r.y, r.x + r.w - 1, r.y + r.h - 1, bottom, 255);
}

void ce_draw_bitmap(const struct ce_object *object, int x, int y, int source_x, int width, CW mask)
{
    const uint8_t *b = object->data;
    if (!b || object->bytes < 40)
        return;
    uint32_t header = ce_u32(b), planes = ce_u16(b + 12), bits = ce_u16(b + 14), compression = ce_u32(b + 16);
    int32_t w = (int32_t)ce_u32(b + 4), h = (int32_t)ce_u32(b + 8);
    if (header < 40 || header > object->bytes || planes != 1 || w <= 0 || w > 8192 || h == 0 ||
        h == INT32_MIN || h > 8192 || h < -8192 || compression > 3)
        ce_missing("unsupported bitmap resource layout");
    uint32_t colors = bits <= 8 ? (ce_u32(b + 32) ? ce_u32(b + 32) : 1u << bits) : 0;
    if (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32)
        ce_missing("unsupported bitmap depth");
    if (colors > 256 || (compression && !(bits == 16 && compression == 3)))
        ce_missing("compressed bitmap resource");
    uint32_t offset = header + colors * 4 + (compression == 3 && header == 40 ? 12 : 0),
             stride = ALIGN_UP((uint32_t)w * bits, 32u) / 8u;
    uint32_t height = h < 0 ? (uint32_t)-h : (uint32_t)h;
    if (object->kind == CE_OBJ_ICON)
        height /= 2;
    if (!ce_range(offset, stride * height, object->bytes))
        ce_fatal("Bitmap pixels exceed resource");
    for (uint32_t row = 0; row < height; row++) {
        int dy = y + (int)row;
        if (dy < ui.surf.clip.y || dy >= ui.surf.clip.y + ui.surf.clip.h)
            continue;
        const uint8_t *pixels = b + offset + (h > 0 ? height - row - 1 : row) * stride;
        for (int col = 0; col < width; col++) {
            int sx = source_x + col, dx = x + col;
            if (sx < 0 || sx >= w || dx < ui.surf.clip.x || dx >= ui.surf.clip.x + ui.surf.clip.w)
                continue;
            uint32_t rgb;
            if (bits <= 8) {
                unsigned index = bits == 8   ? pixels[sx]
                                 : bits == 4 ? (pixels[sx / 2] >> (sx % 2 ? 0 : 4) & 15)
                                             : (pixels[sx / 8] >> (7 - sx % 8) & 1);
                if (index >= colors)
                    continue;
                const uint8_t *p = b + header + index * 4;
                rgb = (uint32_t)p[2] << 16 | p[1] << 8 | p[0];
            } else if (bits == 16) {
                uint16_t v = ce_u16(pixels + sx * 2);
                if (compression == 3) {
                    rgb =
                        ((v >> 11) * 255 / 31) << 16 | ((v >> 5 & 63) * 255 / 63) << 8 | (v & 31) * 255 / 31;
                } else
                    rgb = ((v >> 10 & 31) * 255 / 31) << 16 | ((v >> 5 & 31) * 255 / 31) << 8 |
                          (v & 31) * 255 / 31;
            } else {
                const uint8_t *p = pixels + sx * (bits / 8);
                rgb = (uint32_t)p[2] << 16 | p[1] << 8 | p[0];
            }
            if (mask != 0xffffffffu && rgb == ce_color(mask))
                continue;
            if (object->kind == CE_OBJ_ICON) {
                uint32_t mask_stride = ALIGN_UP((uint32_t)w, 32u) / 8;
                uint32_t mask_off = offset + stride * height;
                if (ce_range(mask_off, mask_stride * height, object->bytes)) {
                    const uint8_t *and = b + mask_off + (height - row - 1) * mask_stride;
                    if (and[sx / 8] & (0x80u >> (sx & 7)))
                        continue;
                }
            }
            ui.surf.px[dy * ui.surf.stride + dx] = RGB565(rgb);
        }
    }
}

struct memory_dc {
    CW bitmap, font, brush, pen;
    bool has_clip;
    struct ce_rect clip;
};
struct writable_dib {
    uint8_t *data, *pixels;
    unsigned width, height, stride, bits, colors;
    bool top_down, rgb565;
};
static bool writable_dib(CW bitmap, struct writable_dib *d)
{
    struct ce_object *b = ce_object(bitmap, CE_OBJ_BITMAP);
    if (!b || !(b->value & 0xc0000000u))
        return false;
    uint8_t *p = b->data;
    unsigned bits = ce_u16(p + 14), colors = bits <= 8 ? ce_u32(p + 32) : 0;
    if (bits <= 8 && !colors)
        colors = 1u << bits;
    unsigned offset = 40 + colors * 4 + (ce_u32(p + 16) == 3 ? 12 : 0);
    *d = (struct writable_dib){p, p + offset, b->width, b->height,
        ALIGN_UP((unsigned)b->width * bits, 32u) / 8, bits, colors,
        (int32_t)ce_u32(p + 8) < 0, ce_u32(p + 16) == 3};
    return true;
}
static uint8_t *dib_row(const struct writable_dib *d, unsigned y)
{
    return d->pixels + (d->top_down ? y : d->height - y - 1) * d->stride;
}
static uint32_t dib_rgb(const struct writable_dib *d, unsigned x, unsigned y)
{
    const uint8_t *p = dib_row(d, y);
    if (d->bits <= 8) {
        unsigned shift = 8 - d->bits - x * d->bits % 8;
        unsigned index = (p[x * d->bits / 8] >> shift) & ((1u << d->bits) - 1);
        if (index >= d->colors)
            return 0;
        p = d->data + 40 + index * 4;
    } else if (d->bits == 16) {
        unsigned v = ce_u16(p + x * 2);
        return d->rgb565 ? ((v >> 11) * 255 / 31) << 16 |
            ((v >> 5 & 63) * 255 / 63) << 8 | (v & 31) * 255 / 31 :
            ((v >> 10 & 31) * 255 / 31) << 16 | ((v >> 5 & 31) * 255 / 31) << 8 |
            (v & 31) * 255 / 31;
    } else
        p += x * (d->bits / 8);
    return (uint32_t)p[2] << 16 | p[1] << 8 | p[0];
}
static void dib_put(const struct writable_dib *d, unsigned x, unsigned y, uint32_t rgb)
{
    uint8_t *p = dib_row(d, y);
    if (d->bits <= 8) {
        unsigned index = 0, distance = UINT32_MAX;
        for (unsigned i = 0; i < d->colors; i++) {
            const uint8_t *c = d->data + 40 + i * 4;
            int r = (int)(rgb >> 16 & 255) - c[2], g = (int)(rgb >> 8 & 255) - c[1],
                b = (int)(rgb & 255) - c[0];
            unsigned delta = r * r + g * g + b * b;
            if (delta < distance) { distance = delta; index = i; }
            if (!delta) break;
        }
        unsigned shift = 8 - d->bits - x * d->bits % 8, mask = ((1u << d->bits) - 1) << shift;
        p += x * d->bits / 8;
        *p = (*p & ~mask) | index << shift;
    } else if (d->bits == 16) {
        unsigned v = d->rgb565 ? RGB565(rgb) :
            ((rgb >> 19 & 31) << 10) | ((rgb >> 11 & 31) << 5) | (rgb >> 3 & 31);
        ce_put16(p + x * 2, v);
    } else {
        p += x * (d->bits / 8);
        p[0] = rgb; p[1] = rgb >> 8; p[2] = rgb >> 16;
    }
}
static struct memory_dc *memory_dc(CW handle)
{
    struct ce_object *o = ce_object(handle, CE_OBJ_DC);
    return o ? o->data : NULL;
}
CE_API(CreateCompatibleDC)
{
    struct memory_dc *dc = calloc(1, sizeof(*dc));
    if (!dc)
        return 0;
    CW handle = ce_new_object(CE_OBJ_DC, 0, dc, sizeof(*dc));
    if (!handle)
        free(dc);
    return handle;
}
CE_API(DeleteDC)
{
    struct ce_object *o = ce_object(a0, CE_OBJ_DC);
    if (!o) {
        ce_error = 6;
        return 0;
    }
    free(o->data);
    memset(o, 0, sizeof(*o));
    return 1;
}
CE_API(CreateCompatibleBitmap)
{
    if (!a1 || !a2 || a1 > 1024 || a2 > 1024) {
        ce_error = 87;
        return 0;
    }
    unsigned stride = ALIGN_UP(a1 * 2, 4u), bytes = 52 + stride * a2;
    uint8_t *b = calloc(1, bytes);
    if (!b)
        return 0;
    ce_put32(b, 40);
    ce_put32(b + 4, a1);
    ce_put32(b + 8, a2);
    ce_put16(b + 12, 1);
    ce_put16(b + 14, 16);
    ce_put32(b + 16, 3);
    ce_put32(b + 40, 0xf800);
    ce_put32(b + 44, 0x7e0);
    ce_put32(b + 48, 31);
    CW handle = ce_new_object(CE_OBJ_BITMAP, 0x80000000u, b, bytes);
    struct ce_object *o = ce_object(handle, CE_OBJ_BITMAP);
    if (o) {
        o->width = a1;
        o->height = a2;
    } else
        free(b);
    return handle;
}
CE_API(CreateDIBSection)
{
    if (a3)
        *PTR(CW, a3) = 0;
    if (!a1 || !a3 || a2 || a4 || a5) {
        ce_error = a2 || a4 || a5 ? 50 : 87;
        return 0;
    }
    const uint8_t *info = PTR(uint8_t, a1);
    uint32_t header = ce_u32(info), bits = ce_u16(info + 14), compression = ce_u32(info + 16);
    int32_t width = (int32_t)ce_u32(info + 4), height = (int32_t)ce_u32(info + 8);
    if (header != 40 || ce_u16(info + 12) != 1 || width <= 0 || width > 1024 || !height ||
        height < -1024 || height > 1024 ||
        (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
        (compression && !(compression == 3 && bits == 16))) {
        ce_error = 87;
        return 0;
    }
    uint32_t colors = bits <= 8 ? ce_u32(info + 32) : 0;
    if (bits <= 8 && !colors)
        colors = 1u << bits;
    if (colors > (bits <= 8 ? 1u << bits : 0)) {
        ce_error = 87;
        return 0;
    }
    /* The renderer uses RGB565 for BI_BITFIELDS. Other masks must not be
     * silently interpreted as that format. BI_RGB sixteen-bit DIBs are RGB555. */
    if (compression == 3 && (ce_u32(info + 40) != 0xf800 || ce_u32(info + 44) != 0x7e0 ||
                             ce_u32(info + 48) != 31)) {
        ce_error = 50;
        return 0;
    }
    unsigned offset = header + colors * 4 + (compression == 3 ? 12 : 0);
    unsigned stride = ALIGN_UP((unsigned)width * bits, 32u) / 8;
    unsigned rows = height < 0 ? -height : height, bytes = offset + stride * rows;
    uint8_t *data = calloc(1, bytes);
    if (!data) {
        ce_error = 8;
        return 0;
    }
    memcpy(data, info, offset);
    ce_put32(data + 20, stride * rows);
    CW handle = ce_new_object(CE_OBJ_BITMAP, 0x40000000u, data, bytes);
    struct ce_object *o = ce_object(handle, CE_OBJ_BITMAP);
    if (!o) {
        free(data);
        return 0;
    }
    o->width = width;
    o->height = rows;
    *PTR(CW, a3) = WORD(data + offset);
    return handle;
}
CE_API(BitBlt)
{
    struct memory_dc *source = memory_dc(a5), *dest = memory_dc(a0);
    struct ce_object *bitmap = source ? ce_object(source->bitmap, CE_OBJ_BITMAP) : NULL;
    if (a8 != 0x00cc0020 || !bitmap || a3 > 1024 || a4 > 1024) {
        ce_error = 50;
        return 0;
    }
    if (dest) {
        struct writable_dib source_dib, dest_dib;
        if (!writable_dib(source->bitmap, &source_dib) || !writable_dib(dest->bitmap, &dest_dib)) {
            ce_error = 50;
            return 0;
        }
        /* Stage the source so overlapping copies behave like memmove. */
        uint32_t *pixels = malloc(MAX(1u, a3 * a4) * sizeof(*pixels));
        if (!pixels) { ce_error = 8; return 0; }
        for (unsigned y = 0; y < a4; y++)
            for (unsigned x = 0; x < a3; x++) {
                int sx = (int32_t)a6 + (int)x, sy = (int32_t)a7 + (int)y;
                pixels[y * a3 + x] = sx >= 0 && sy >= 0 && (unsigned)sx < source_dib.width &&
                    (unsigned)sy < source_dib.height ? dib_rgb(&source_dib, sx, sy) : UINT32_MAX;
            }
        for (unsigned y = 0; y < a4; y++)
            for (unsigned x = 0; x < a3; x++) {
                int dx = (int32_t)a1 + (int)x, dy = (int32_t)a2 + (int)y;
                if (dx < 0 || dy < 0 || (unsigned)dx >= dest_dib.width || (unsigned)dy >= dest_dib.height ||
                    pixels[y * a3 + x] == UINT32_MAX || (dest->has_clip &&
                    (dx < dest->clip.left || dx >= dest->clip.right || dy < dest->clip.top || dy >= dest->clip.bottom)))
                    continue;
                dib_put(&dest_dib, dx, dy, pixels[y * a3 + x]);
            }
        free(pixels);
        return 1;
    }
    gfx_surface saved = ui.surf;
    gfx_rect r = ce_client_rect(a0);
    gfx_set_clip(&ui.surf, gfx_r(r.x + (int)a1, r.y + (int)a2, a3, a4));
    ce_draw_bitmap(bitmap, r.x + (int)a1, r.y + (int)a2 - (int)a7, a6, a3, 0xffffffffu);
    ui.surf = saved;
    return 1;
}
CE_API(DrawFocusRect)
{
    if (!a1)
        return 0;
    gfx_rect r = rectangle(a0, PTR(struct ce_rect, a1));
    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++) {
            int dx = r.x + x, dy = r.y + y;
            if ((x == 0 || y == 0 || x == r.w - 1 || y == r.h - 1) && !((dx + dy) & 1) &&
                gfx_rect_contains(ui.surf.clip, dx, dy))
                ui.surf.px[dy * ui.surf.stride + dx] ^= 0xffff;
        }
    return 1;
}
CE_API(GetObjectW)
{
    struct ce_object *o = ce_object(a0, 0);
    if (!o)
        return 0;
    if (o->kind == CE_OBJ_BITMAP) {
        if (!a2)
            return 24;
        if (a1 < 24 || o->bytes < 40)
            return 0;
        const uint8_t *b = o->data;
        CW out[6] = {0,
                     ce_u32(b + 4),
                     (CW)abs((int)ce_u32(b + 8)),
                     ALIGN_UP(ce_u32(b + 4) * ce_u16(b + 14), 32u) / 8,
                     (CW)ce_u16(b + 12) | ((CW)ce_u16(b + 14) << 16),
                     (o->value & 0xc0000000u) ? WORD(b + ce_u32(b) +
                         (ce_u16(b + 14) <= 8 ? (ce_u32(b + 32) ? ce_u32(b + 32) :
                          1u << ce_u16(b + 14)) * 4 : 0) +
                         (ce_u32(b + 16) == 3 && ce_u32(b) == 40 ? 12 : 0)) : 0};
        memcpy(PTR(void, a2), out, 24);
        return 24;
    }
    ce_error = 50;
    return 0;
}
CE_API(BeginPaint)
{
    struct ce_window *w = ce_window(a0);
    if (!w || !a1)
        return 0;
    memset(PTR(void, a1), 0, 64);
    CW *p = PTR(CW, a1);
    p[0] = a0;
    p[1] = 1;
    *PTR(struct ce_rect, a1 + 8) =
        (struct ce_rect){0, 0, w->rect.right - w->rect.left, w->rect.bottom - w->rect.top};
    gfx_set_clip(&ui.surf, ce_client_rect(a0));
    w->dirty = false;
    return a0;
}
CE_API(EndPaint)
{
    gfx_reset_clip(&ui.surf);
    return ce_window(a0) != NULL;
}
CE_API(GetDC)
{
    gfx_reset_clip(&ui.surf);
    return a0 ? a0 : 0x74000000u;
}
CE_API(ReleaseDC)
{
    gfx_reset_clip(&ui.surf);
    return 1;
}
CE_API(GetDeviceCaps)
{
    switch (a1) {
    case 8:
        return 240;
    case 10:
        return 320;
    case 12:
        return 16;
    case 14:
        return 1;
    case 88:
    case 90:
        return 96;
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
CE_API(GetSysColorBrush)
{
    static CW brushes[32];
    if (a0 >= ARRAY_SIZE(brushes))
        return 0;
    if (!ce_object(brushes[a0], CE_OBJ_BRUSH))
        brushes[a0] = ce_new_object(CE_OBJ_BRUSH,
            ce_color(ce_GetSysColor(a0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)), NULL, 0);
    return brushes[a0];
}
CE_API(GetStockObject)
{
    if (a0 <= 5) {
        static CW brushes[6];
        static const CW colors[] = {0xffffff, 0xc0c0c0, 0x808080, 0x404040, 0, 0xffffffffu};
        if (!ce_object(brushes[a0], CE_OBJ_BRUSH))
            brushes[a0] = ce_new_object(CE_OBJ_BRUSH, colors[a0], NULL, 0);
        return brushes[a0];
    }
    if (a0 <= 8) {
        static CW pens[3];
        unsigned index = a0 - 6;
        if (!ce_object(pens[index], CE_OBJ_PEN)) {
            pens[index] = ce_new_object(CE_OBJ_PEN, index ? 0 : 0xffffff, NULL, 0);
            struct ce_object *pen = ce_object(pens[index], CE_OBJ_PEN);
            if (pen) { pen->width = 1; pen->height = index == 2 ? 5 : 0; }
        }
        return pens[index];
    }
    static CW font;
    if (!font)
        font = ce_new_object(CE_OBJ_FONT, 12, NULL, 0);
    return font;
}
CE_API(CreateSolidBrush)
{
    return ce_new_object(CE_OBJ_BRUSH, ce_color(a0), NULL, 0);
}
CE_API(CreatePen)
{
    if ((a0 != 0 && a0 != 5) || a1 > 16) {
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
    const uint8_t *p = PTR(uint8_t, a0);
    return ce_CreatePen(ce_u32(p), ce_u32(p + 4), ce_u32(p + 12), 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
CE_API(CreateFontIndirectW)
{
    int32_t height = (int32_t)ce_u32(PTR(void, a0));
    return ce_new_object(CE_OBJ_FONT, (CW)abs(height), NULL, 0);
}
CE_API(DeleteObject)
{
    struct ce_object *o = ce_object(a0, 0);
    if (!o)
        return 0;
    if (o->kind == CE_OBJ_IMAGE_LIST) {
        struct image_list *l = o->data;
        for(unsigned i=0;i<l->count;i++)free(l->image[i].data);
    }
    if (o->kind == CE_OBJ_IMAGE_LIST || o->kind == CE_OBJ_DC || o->kind == CE_OBJ_REGION ||
        (o->kind == CE_OBJ_BITMAP && (o->value & 0xc0000000u)))
        free(o->data);
    memset(o, 0, sizeof(*o));
    return 1;
}
CE_API(SelectObject)
{
    struct ce_object *o = ce_object(a1, 0);
    if (!o)
        return 0;
    struct memory_dc *dc = memory_dc(a0);
    if (dc) {
        CW *slot = o->kind == CE_OBJ_BITMAP ? &dc->bitmap : o->kind == CE_OBJ_FONT ? &dc->font :
                   o->kind == CE_OBJ_BRUSH ? &dc->brush : o->kind == CE_OBJ_PEN ? &dc->pen : NULL;
        if (!slot) return 0;
        CW old = *slot; *slot = a1; return old;
    }
    CW old = 0;
    if (o->kind == CE_OBJ_FONT) {
        old = selected_font;
        selected_font = a1;
    }
    if (o->kind == CE_OBJ_BRUSH) {
        old = selected_brush;
        selected_brush = a1;
    }
    if (o->kind == CE_OBJ_PEN) {
        old = selected_pen;
        selected_pen = a1;
    }
    return old;
}
CE_API(FillRect)
{
    struct ce_object *brush = ce_object(a2, CE_OBJ_BRUSH);
    CW rgb = brush ? brush->value : 0xc0c0c0;
    if (rgb != 0xffffffffu)
        gfx_fill(&ui.surf, rectangle(a0, PTR(struct ce_rect, a1)), rgb);
    return 1;
}
static bool drawing_surface(CW handle, gfx_surface *surface, gfx_rect *origin)
{
    struct memory_dc *dc = memory_dc(handle);
    if (!dc) {
        *surface = ui.surf;
        *origin = ce_client_rect(handle);
        return true;
    }
    struct ce_object *b = ce_object(dc->bitmap, CE_OBJ_BITMAP);
    if (!b || b->value != 0x80000000u) {
        ce_error = 50;
        return false;
    }
    gfx_surface_init(surface, (uint16_t *)((uint8_t *)b->data + 52), b->width, b->height,
                     ALIGN_UP(b->width, 2));
    if (dc->has_clip)
        gfx_set_clip(surface, gfx_r(dc->clip.left, dc->clip.top, dc->clip.right - dc->clip.left,
                                   dc->clip.bottom - dc->clip.top));
    ce_put32((uint8_t *)b->data + 8, (CW)-b->height);
    *origin = gfx_r(0, 0, b->width, b->height);
    return true;
}
CE_API(CreateRectRgn)
{
    int32_t x1 = (int32_t)a0, y1 = (int32_t)a1, x2 = (int32_t)a2, y2 = (int32_t)a3;
    if (x1 < -32768 || y1 < -32768 || x2 < -32768 || y2 < -32768 ||
        x1 > 32767 || y1 > 32767 || x2 > 32767 || y2 > 32767) {
        ce_error = 87;
        return 0;
    }
    struct ce_rect *r = malloc(sizeof(*r));
    if (!r)
        return 0;
    *r = (struct ce_rect){MIN(x1, x2), MIN(y1, y2), MAX(x1, x2), MAX(y1, y2)};
    CW handle = ce_new_object(CE_OBJ_REGION, 0, r, sizeof(*r));
    if (!handle)
        free(r);
    return handle;
}
CE_API(SelectClipRgn)
{
    struct ce_object *region = a1 ? ce_object(a1, CE_OBJ_REGION) : NULL;
    if (a1 && !region) {
        ce_error = 6;
        return 0;
    }
    struct memory_dc *dc = memory_dc(a0);
    gfx_rect origin = ce_client_rect(a0);
    struct ce_rect r = region ? *(struct ce_rect *)region->data :
        (struct ce_rect){0, 0, dc ? 0 : origin.w, dc ? 0 : origin.h};
    if (dc) {
        dc->has_clip = region != NULL;
        dc->clip = r;
    } else
        gfx_set_clip(&ui.surf, gfx_r(origin.x + r.left, origin.y + r.top,
                                    r.right - r.left, r.bottom - r.top));
    return region && (r.right == r.left || r.bottom == r.top) ? 1 : 2;
}
CE_API(PatBlt)
{
    unsigned rop = (a5 >> 16) & 255;
    if (((rop >> 2) & 0x33) != (rop & 0x33)) {
        ce_error = 50; /* PatBlt has no source bitmap. */
        return 0;
    }
    if (a3 > 4096 || a4 > 4096 || (int32_t)a1 < -32768 || (int32_t)a1 > 32767 ||
        (int32_t)a2 < -32768 || (int32_t)a2 > 32767) {
        ce_error = 87;
        return 0;
    }
    struct memory_dc *dc = memory_dc(a0);
    if (dc) {
        struct writable_dib d;
        if (!writable_dib(dc->bitmap, &d)) { ce_error = 50; return 0; }
        struct ce_object *brush = ce_object(dc->brush, CE_OBJ_BRUSH);
        if (brush && brush->value == UINT32_MAX && rop == 0xf0)
            return 1;
        uint32_t pattern = brush ? brush->value : 0xffffff;
        int left = MAX(0, (int32_t)a1), top = MAX(0, (int32_t)a2),
            right = MIN((int)d.width, (int32_t)a1 + (int)a3), bottom = MIN((int)d.height, (int32_t)a2 + (int)a4);
        if (dc->has_clip) {
            left = MAX(left, dc->clip.left); top = MAX(top, dc->clip.top);
            right = MIN(right, dc->clip.right); bottom = MIN(bottom, dc->clip.bottom);
        }
        for (int y = top; y < bottom; y++)
            for (int x = left; x < right; x++) {
                uint32_t dest = dib_rgb(&d, x, y);
                uint32_t rgb = ((rop & 1) ? ~pattern & ~dest : 0) | ((rop & 2) ? ~pattern & dest : 0) |
                    ((rop & 16) ? pattern & ~dest : 0) | ((rop & 32) ? pattern & dest : 0);
                dib_put(&d, x, y, rgb & 0xffffff);
            }
        return 1;
    }
    gfx_surface surface;
    gfx_rect origin, bounds;
    if (!drawing_surface(a0, &surface, &origin))
        return 0;
    struct ce_object *brush = ce_object(dc ? dc->brush : selected_brush, CE_OBJ_BRUSH);
    if (brush && brush->value == 0xffffffffu && rop == 0xf0)
        return 1;
    uint16_t pattern = RGB565(brush ? brush->value : 0xffffff);
    gfx_rect area = gfx_r(origin.x + (int32_t)a1, origin.y + (int32_t)a2, a3, a4);
    if (!gfx_rect_intersect(area, surface.clip, &bounds))
        return 1;
    for (int y = bounds.y; y < bounds.y + bounds.h; y++)
        for (int x = bounds.x; x < bounds.x + bounds.w; x++) {
            uint16_t *pixel = surface.px + y * surface.stride + x, dest = *pixel;
            *pixel = ((rop & 1) ? ~pattern & ~dest : 0) |
                ((rop & 2) ? ~pattern & dest : 0) |
                ((rop & 16) ? pattern & ~dest : 0) | ((rop & 32) ? pattern & dest : 0);
        }
    return 1;
}
CE_API(Rectangle)
{
    int32_t left = (int32_t)a1, top = (int32_t)a2, right = (int32_t)a3, bottom = (int32_t)a4;
    if (left < -32768 || top < -32768 || right > 32767 || bottom > 32767 || right < left ||
        bottom < top || right - left > 4096 || bottom - top > 4096) {
        ce_error = 87;
        return 0;
    }
    gfx_surface surface;
    gfx_rect origin;
    if (!drawing_surface(a0, &surface, &origin))
        return 0;
    struct memory_dc *dc = memory_dc(a0);
    struct ce_object *brush = ce_object(dc ? dc->brush : selected_brush, CE_OBJ_BRUSH);
    struct ce_object *pen = ce_object(dc ? dc->pen : selected_pen, CE_OBJ_PEN);
    gfx_rect r = gfx_r(origin.x + left, origin.y + top, right - left, bottom - top);
    if (!brush || brush->value != 0xffffffffu)
        gfx_fill(&surface, r, brush ? brush->value : 0xffffff);
    if (!pen || pen->height != 5) {
        unsigned width = pen ? MAX(1, pen->width) : 1;
        uint32_t color = pen ? pen->value : 0;
        for (unsigned i = 0; i < width && r.w > 0 && r.h > 0; i++) {
            gfx_line(&surface, r.x, r.y, r.x + r.w - 1, r.y, color, 255);
            gfx_line(&surface, r.x, r.y, r.x, r.y + r.h - 1, color, 255);
            gfx_line(&surface, r.x, r.y + r.h - 1, r.x + r.w - 1, r.y + r.h - 1, color, 255);
            gfx_line(&surface, r.x + r.w - 1, r.y, r.x + r.w - 1, r.y + r.h - 1, color, 255);
            r.x++; r.y++; r.w -= 2; r.h -= 2;
        }
    }
    return 1;
}
CE_API(GetCurrentObject)
{
    struct memory_dc *dc = memory_dc(a0);
    CW object = a1 == 1 ? (dc ? dc->pen : selected_pen) :
        a1 == 2 ? (dc ? dc->brush : selected_brush) :
        a1 == 6 ? (dc ? dc->font : selected_font) : a1 == 7 && dc ? dc->bitmap : 0;
    if (!object && (a1 == 1 || a1 == 2 || a1 == 6))
        object = ce_GetStockObject(a1 == 1 ? 7 : a1 == 2 ? 0 : 13, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    return object;
}
CE_API(Polyline)
{
    if (a2 > 4096)
        return 0;
    gfx_rect r = ce_client_rect(a0);
    struct ce_object *pen = ce_object(selected_pen, CE_OBJ_PEN);
    if (pen && pen->height == 5)
        return 1;
    int32_t *p = PTR(int32_t, a1);
    for (unsigned i = 1; i < a2; i++) {
        int width = pen ? pen->width : 1;
        bool horizontal = abs(p[i * 2] - p[(i - 1) * 2]) >= abs(p[i * 2 + 1] - p[(i - 1) * 2 + 1]);
        for (int stroke = 0; stroke < width; stroke++) {
            int offset = stroke - width / 2;
            int dx = horizontal ? 0 : offset, dy = horizontal ? offset : 0;
            gfx_line(&ui.surf, r.x + p[(i - 1) * 2] + dx, r.y + p[(i - 1) * 2 + 1] + dy,
                r.x + p[i * 2] + dx, r.y + p[i * 2 + 1] + dy, pen ? pen->value : 0, 255);
        }
    }
    return 1;
}
CE_API(DrawFrameControl)
{
    gfx_rect r = rectangle(a0, PTR(struct ce_rect, a1));
    gfx_fill(&ui.surf, r, 0xc0c0c0);
    border(r, (a3 & 0x200) ? 0x404040 : 0xffffff, (a3 & 0x200) ? 0xffffff : 0x404040);
    return 1;
}
CE_API(SetTextColor)
{
    CW old = text_color;
    text_color = ce_color(a1);
    return ce_color(old);
}
CE_API(SetBkColor)
{
    CW old = background_color;
    background_color = ce_color(a1);
    return ce_color(old);
}
CE_API(SetBkMode)
{
    int old = background_mode;
    background_mode = (int)a1;
    return (CW)old;
}
CE_API(TextOutW)
{
    if (a4 > 65535)
        return 0;
    WCHAR *copy = malloc((a4 + 1) * 2);
    if (!copy)
        return 0;
    memcpy(copy, PTR(void, a3), a4 * 2);
    copy[a4] = 0;
    char *text = malloc(a4 * 3 + 1);
    if (!text) {
        free(copy);
        return 0;
    }
    ce_narrow(text, a4 * 3 + 1, copy);
    gfx_rect r = ce_client_rect(a0);
    gfx_text(&ui.surf, r.x + (int)a1, r.y + (int)a2, ui_font(FONT_R12), text, text_color, 255);
    free(text);
    free(copy);
    return 1;
}
CW ce_draw_text(CW a0, CW a1, CW a2, CW a3, CW a4)
{
    if (!a1 || !a3)
        return 0;
    unsigned length = a2 == 0xffffffffu ? ce_wlen(PTR(WCHAR, a1)) : a2;
    if (!length || length > 65535)
        return 0;
    WCHAR *source = PTR(WCHAR, a1);
    struct ce_rect *rect = PTR(struct ce_rect, a3);
    const gfx_font *font = ui_font(FONT_R12);
    int width = MAX(1, rect->right - rect->left), y = rect->top, widest = 0, lines = 0;
    if ((a4 & 0x24) == 0x24)
        y += (rect->bottom - rect->top - 14) / 2;
    gfx_rect origin = ce_client_rect(a0), old_clip = ui.surf.clip;
    if (!(a4 & (0x100 | 0x400)))
        gfx_set_clip(&ui.surf, rectangle(a0, rect));
    for (unsigned pos = 0; pos < length && source[pos];) {
        char line[512];
        unsigned count = 0, word = 0, word_pos = pos;
        int advance = 0, word_width = 0;
        while (pos < length && source[pos] && count + 1 < sizeof(line)) {
            WCHAR ch = source[pos];
            if (!(a4 & 0x20) && (ch == '\r' || ch == '\n')) {
                pos++;
                if (ch == '\r' && pos < length && source[pos] == '\n')
                    pos++;
                break;
            }
            if (ch == '\r' || ch == '\n' || ch == '\t')
                ch = ' ';
            char glyph[2] = {(char)(ch < 256 ? ch : '?'), 0};
            int next = gfx_text_width(font, glyph);
            if ((a4 & 0x10) && !(a4 & 0x20) && advance + next > width && count) {
                if (word) {
                    count = word;
                    pos = word_pos;
                    advance = word_width;
                }
                break;
            }
            if (ch == '&' && !(a4 & 0x800)) {
                pos++;
                if (pos >= length || source[pos] != '&')
                    continue;
            }
            line[count++] = glyph[0];
            advance += next;
            pos++;
            if (ch == ' ') {
                word = count;
                word_pos = pos;
                word_width = advance;
            }
        }
        line[count] = 0;
        widest = MAX(widest, advance);
        if (!(a4 & 0x400)) {
            int x = rect->left;
            if (a4 & 1)
                x += (width - advance) / 2;
            else if (a4 & 2)
                x += width - advance;
            gfx_text(&ui.surf, origin.x + x, origin.y + y, font, line, text_color, 255);
        }
        y += 14;
        lines++;
    }
    if (a4 & 0x400) {
        rect->right = rect->left + widest;
        rect->bottom = rect->top + lines * 14;
    }
    ui.surf.clip = old_clip;
    return (CW)(lines * 14);
}
CE_API(DrawTextW)
{
    return ce_draw_text(a0, a1, a2, a3, a4);
}
CE_API(GetTextExtentExPointW)
{
    if (!a1 || !a6 || a2 > 65535)
        return 0;
    int width = 0;
    unsigned fit = 0;
    const WCHAR *source = PTR(WCHAR, a1);
    for (unsigned i = 0; i < a2; i++) {
        char s[2] = {(char)(source[i] < 256 ? source[i] : '?'), 0};
        width += gfx_text_width(ui_font(FONT_R12), s);
        if (a5) PTR(int32_t, a5)[i] = width;
        if (width <= (int32_t)a3) fit = i + 1;
    }
    if (a4) *PTR(CW, a4) = fit;
    PTR(int32_t, a6)[0] = width;
    PTR(int32_t, a6)[1] = 14;
    return 1;
}
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
CE_API(LoadImageW);
CE_API(LoadIconW)
{
    return ce_LoadImageW(a0, a1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
CE_API(LoadImageW)
{
    if (a1 > 65535 || a2 > 2) {
        ce_error = 50;
        return 0;
    }
    uint32_t size;
    const void *data = ce_resource(a0, a2 == 0 ? 2 : a2 == 1 ? 14 : 12, (uint16_t)a1, &size);
    if (!data) {
        ce_error = 1813;
        return 0;
    }
    if (a2 == 1) {
        if (size < 20 || ce_u16((const uint8_t *)data + 2) != 1 || !ce_u16((const uint8_t *)data + 4))
            return 0;
        data = ce_resource(a0, 3, ce_u16((const uint8_t *)data + 18), &size);
        if (!data || size < 40)
            return 0;
    }
    CW handle = ce_new_object(a2 == 1 ? CE_OBJ_ICON : CE_OBJ_BITMAP, a2, (void *)data, size);
    struct ce_object *o = ce_object(handle, 0);
    if (o && size >= 40) {
        o->width = (int)ce_u32((const uint8_t *)data + 4);
        o->height = abs((int)ce_u32((const uint8_t *)data + 8)) / (a2 == 1 ? 2 : 1);
    }
    return handle;
}
CE_API(LoadBitmapW)
{
    return ce_LoadImageW(a0, a1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
CE_API(ImageList_Create)
{
    if (!a0 || !a1 || a0 > 256 || a1 > 256 || a3 > CE_IMAGES) {
        ce_error = 87;
        return 0;
    }
    struct image_list *l = calloc(1, sizeof(*l));
    if (!l)
        return 0;
    CW handle = ce_new_object(CE_OBJ_IMAGE_LIST, 0xffffffffu, l, sizeof(*l));
    struct ce_object *o = ce_object(handle, CE_OBJ_IMAGE_LIST);
    if (!o) {
        free(l);
        return 0;
    }
    o->width = a0;
    o->height = a1;
    return handle;
}
CE_API(ImageList_ReplaceIcon)
{
    struct ce_object *o = ce_object(a0, CE_OBJ_IMAGE_LIST), *icon = ce_object(a2, CE_OBJ_ICON);
    if (!o || !icon)
        return (CW)-1;
    struct image_list *l = o->data;
    unsigned index = a1 == 0xffffffffu ? l->count : a1;
    if (index >= CE_IMAGES || (a1 != 0xffffffffu && index >= l->count))
        return (CW)-1;
    void *copy = malloc(icon->bytes);
    if (!copy)
        return (CW)-1;
    memcpy(copy, icon->data, icon->bytes);
    free(l->image[index].data);
    l->image[index] = *icon;
    l->image[index].data = copy;
    if (index == l->count)
        l->count++;
    return index;
}
CE_API(ImageList_Add)
{
    struct ce_object *o = ce_object(a0, CE_OBJ_IMAGE_LIST), *bitmap = ce_object(a1, CE_OBJ_BITMAP);
    if (!o || !bitmap || !bitmap->width || a2) {
        ce_error = a2 ? 50 : 87;
        return (CW)-1;
    }
    struct image_list *l = o->data;
    unsigned count = bitmap->width / o->width, first = l->count;
    if (!count || count > CE_IMAGES - first)
        return (CW)-1;
    for (unsigned i = 0; i < count; i++) {
        void *copy = malloc(bitmap->bytes);
        if (!copy) {
            while (l->count > first) {
                free(l->image[--l->count].data);
                memset(&l->image[l->count], 0, sizeof(l->image[0]));
            }
            return (CW)-1;
        }
        memcpy(copy, bitmap->data, bitmap->bytes);
        l->image[l->count] = *bitmap;
        l->image[l->count].data = copy;
        l->image[l->count++].value = i * o->width;
    }
    return first;
}
CE_API(ImageList_GetImageCount)
{
    struct ce_object *o = ce_object(a0, CE_OBJ_IMAGE_LIST);
    return o ? ((struct image_list *)o->data)->count : 0;
}
CE_API(ImageList_LoadImage)
{
    CW bitmap = ce_LoadImageW(a0, a1, a5, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    struct ce_object *b = ce_object(bitmap, 0);
    if (!b)
        return 0;
    CW handle = ce_ImageList_Create(a2 ? a2 : (CW)b->width, b->height, 0, 0, a3, 0, 0, 0, 0, 0, 0, 0);
    struct ce_object *o = ce_object(handle, CE_OBJ_IMAGE_LIST);
    CW added = a5 == 1 ? ce_ImageList_ReplaceIcon(handle, (CW)-1, bitmap, 0, 0, 0, 0, 0, 0, 0, 0, 0)
                       : ce_ImageList_Add(handle, bitmap, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    ce_DeleteObject(bitmap, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (o)
        o->value = a4;
    if (added == (CW)-1) {
        ce_DeleteObject(handle, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        return 0;
    }
    return handle;
}
CE_API(ImageList_Draw)
{
    struct ce_object *o = ce_object(a0, CE_OBJ_IMAGE_LIST);
    if (!o || a1 >= ((struct image_list *)o->data)->count)
        return 0;
    struct ce_object *image = &((struct image_list *)o->data)->image[a1];
    gfx_rect r = ce_client_rect(a2);
    ce_draw_bitmap(image, r.x + (int)a3, r.y + (int)a4, image->kind == CE_OBJ_ICON ? 0 : image->value,
                   o->width, (a5 & 1) ? o->value : 0xffffffffu);
    return 1;
}
CE_API(ImageList_Destroy)
{
    if (!ce_object(a0, CE_OBJ_IMAGE_LIST))
        return 0;
    return ce_DeleteObject(a0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
CE_API(DestroyIcon)
{
    if (!ce_object(a0, CE_OBJ_ICON))
        return 0;
    return ce_DeleteObject(a0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

static const struct ce_export exports[] = {
    CE_EXPORT(CreateCompatibleDC), CE_EXPORT(DeleteDC), CE_EXPORT(CreateCompatibleBitmap),
    CE_EXPORT(CreateDIBSection),
    CE_EXPORT(BitBlt), CE_EXPORT(DrawFocusRect), CE_EXPORT(GetObjectW),
    {"GetWindowDC", (void *)ce_GetDC},
    CE_EXPORT(BeginPaint),
    CE_EXPORT(EndPaint),
    CE_EXPORT(GetDC),
    CE_EXPORT(ReleaseDC),
    CE_EXPORT(GetDeviceCaps),
    CE_EXPORT(GetSysColor),
    CE_EXPORT(GetSysColorBrush),
    CE_EXPORT(GetStockObject),
    CE_EXPORT(CreateSolidBrush),
    CE_EXPORT(CreatePen),
    CE_EXPORT(CreateFontIndirectW),
    CE_EXPORT(DeleteObject),
    CE_EXPORT(SelectObject),
    CE_EXPORT(FillRect),
    CE_EXPORT(PatBlt),
    CE_EXPORT(CreateRectRgn), CE_EXPORT(SelectClipRgn),
    CE_EXPORT(Rectangle), CE_EXPORT(CreatePenIndirect), CE_EXPORT(GetCurrentObject),
    CE_EXPORT(Polyline),
    CE_EXPORT(DrawFrameControl),
    CE_EXPORT(SetTextColor),
    CE_EXPORT(SetBkColor),
    CE_EXPORT(SetBkMode),
    CE_EXPORT(TextOutW),
    CE_EXPORT(DrawTextW),
    CE_EXPORT(GetTextExtentExPointW),
    CE_EXPORT(LoadStringW),
    CE_EXPORT(FindResourceW),
    CE_EXPORT(LoadResource),
    CE_EXPORT(LockResource),
    CE_EXPORT(LoadCursorW),
    CE_EXPORT(SetCursor),
    CE_EXPORT(LoadIconW), CE_EXPORT(LoadImageW),
    CE_EXPORT(LoadBitmapW),
    CE_EXPORT(ImageList_Create),
    CE_EXPORT(ImageList_Add),
    CE_EXPORT(ImageList_ReplaceIcon),
    CE_EXPORT(ImageList_GetImageCount),
    CE_EXPORT(DestroyIcon),
    CE_EXPORT(ImageList_LoadImage),
    CE_EXPORT(ImageList_Draw),
    CE_EXPORT(ImageList_Destroy),
};
void *ce_resolve_gdi(const char *name)
{
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return NULL;
}
