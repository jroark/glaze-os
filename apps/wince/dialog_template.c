/* Standard Win32/CE DLGTEMPLATE and DLGITEMTEMPLATE, including DWORD item
 * alignment and the optional DS_SETFONT header. Extended templates reject. */
#include "dialog_template.h"
#include "pe.h"
#include <string.h>
struct cursor {
    const uint8_t *p;
    size_t size, pos;
};
static bool take(struct cursor *c, size_t n, const uint8_t **p)
{
    if (c->pos > c->size || n > c->size - c->pos)
        return false;
    *p = c->p + c->pos;
    c->pos += n;
    return true;
}
static bool text(struct cursor *c, uint16_t *out, unsigned cap, uint16_t *ordinal)
{
    const uint8_t *p;
    unsigned n = 0;
    if (ordinal)
        *ordinal = 0;
    for (;;) {
        if (!take(c, 2, &p))
            return false;
        uint16_t ch = ce_u16(p);
        if (!n && ch == 0xffff) {
            if (!take(c, 2, &p) || !ordinal)
                return false;
            *ordinal = ce_u16(p);
            if (out)
                out[0] = 0;
            return true;
        }
        if (out) {
            if (n >= cap)
                return false;
            out[n] = ch;
        }
        if (!ch)
            return true;
        n++;
    }
}
bool ce_dialog_decode(struct ce_dialog_template *d, const void *data, size_t size)
{
    memset(d, 0, sizeof(*d));
    struct cursor c = {data, size, 0};
    const uint8_t *p;
    uint16_t ordinal;
    if (!take(&c, 18, &p) || ce_u16(p + 2) == 0xffff)
        return false;
    d->style = ce_u32(p);
    d->count = ce_u16(p + 8);
    d->x = (int16_t)ce_u16(p + 10);
    d->y = (int16_t)ce_u16(p + 12);
    d->width = (int16_t)ce_u16(p + 14);
    d->height = (int16_t)ce_u16(p + 16);
    if (d->count > 32 || d->width <= 0 || d->height <= 0)
        return false;
    if (!text(&c, NULL, 0, &ordinal) || !text(&c, NULL, 0, &ordinal) || !text(&c, d->title, 256, NULL))
        return false;
    if ((d->style & 0x40) && (!take(&c, 2, &p) || !text(&c, NULL, 0, NULL)))
        return false;
    static const char *const classes[] = {"Button", "Edit", "Static", "ListBox", "ScrollBar", "ComboBox"};
    for (unsigned i = 0; i < d->count; i++) {
        c.pos = (c.pos + 3) & ~(size_t)3;
        if (!take(&c, 18, &p))
            return false;
        struct ce_dialog_item *item = &d->items[i];
        item->style = ce_u32(p);
        item->id = ce_u16(p + 16);
        item->x = (int16_t)ce_u16(p + 8);
        item->y = (int16_t)ce_u16(p + 10);
        item->width = (int16_t)ce_u16(p + 12);
        item->height = (int16_t)ce_u16(p + 14);
        if (!text(&c, item->klass, 64, &ordinal))
            return false;
        if (ordinal) {
            if (ordinal < 0x80 || ordinal > 0x85)
                return false;
            const char *s = classes[ordinal - 0x80];
            for (unsigned j = 0;; j++) {
                item->klass[j] = s[j];
                if (!s[j])
                    break;
            }
        }
        if (!text(&c, item->text, 256, &ordinal) || !take(&c, 2, &p))
            return false;
        item->text_resource = ordinal;
        unsigned extra = ce_u16(p);
        if (extra && (extra < 2 || !take(&c, extra - 2, &p)))
            return false;
    }
    return true;
}
