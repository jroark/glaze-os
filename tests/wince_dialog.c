#include "../apps/wince/dialog_template.h"
#include "../apps/wince/pe.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void word(unsigned char *p, unsigned n)
{
    p[0] = n;
    p[1] = n >> 8;
}
int main(void)
{
    unsigned char data[128] = {0};
    ce_put32(data, 0x40);
    word(data + 8, 1);
    word(data + 10, 7);
    word(data + 12, 9);
    word(data + 14, 134);
    word(data + 16, 56);
    /* menu, class, title "D", point size, face "F", padding. */
    word(data + 22, 'D');
    word(data + 26, 8);
    word(data + 28, 'F');
    unsigned item = 32;
    ce_put32(data + item, 0x50810080);
    word(data + item + 8, 2);
    word(data + item + 10, 14);
    word(data + item + 12, 130);
    word(data + item + 14, 12);
    word(data + item + 16, 1020);
    word(data + item + 18, 0xffff);
    word(data + item + 20, 0x81);
    /* Empty caption and zero creation-data size. */
    unsigned size = item + 26;
    struct ce_dialog_template *d = malloc(sizeof(*d));
    assert(d && ce_dialog_decode(d, data, size));
    assert(d->count == 1 && d->width == 134 && d->title[0] == 'D');
    assert(d->x == 7 && d->y == 9);
    assert(d->items[0].id == 1020 && d->items[0].klass[0] == 'E' && d->items[0].y == 14);
    for (unsigned n = 0; n < size; n++)
        assert(!ce_dialog_decode(d, data, n));
    word(data + 8, CE_DIALOG_ITEMS + 1);
    assert(!ce_dialog_decode(d, data, size));
    word(data + 8, 1);
    word(data + item + 20, 0x86);
    assert(!ce_dialog_decode(d, data, size));
    word(data + item + 20, 0x81);
    word(data + item + 24, 1);
    assert(!ce_dialog_decode(d, data, size));
    word(data + item + 24, 0);
    /* Static resource icons encode their title as an ordinal, not UTF-16. */
    word(data + item + 20, 0x82);
    word(data + item + 22, 0xffff);
    word(data + item + 24, 101);
    word(data + item + 26, 0);
    assert(ce_dialog_decode(d, data, size + 2));
    assert(d->items[0].text_resource == 101 && !d->items[0].text[0]);
    assert(!ce_dialog_decode(d, data, size + 1));
    word(data + 2, 0xffff);
    assert(!ce_dialog_decode(d, data, size));
    unsigned char many[24 + 33 * 28] = {0};
    word(many + 8, 33);
    word(many + 14, 130);
    word(many + 16, 160);
    for (unsigned i = 0; i < 33; i++) {
        unsigned char *p = many + 24 + i * 28;
        word(p + 12, 50); word(p + 14, 10); word(p + 16, 100 + i);
        word(p + 18, 0xffff); word(p + 20, 0x82);
    }
    assert(ce_dialog_decode(d, many, sizeof(many)) && d->count == 33);
    assert(d->items[32].id == 132);
    unsigned char menu_dialog[26] = {0};
    word(menu_dialog + 14, 115); word(menu_dialog + 16, 114);
    word(menu_dialog + 18, 0xffff); word(menu_dialog + 20, 128);
    assert(ce_dialog_decode(d, menu_dialog, sizeof(menu_dialog)) && d->menu == 128 && !d->count);
    free(d);
    puts("WinCE dialog alignment, class ordinals and truncation checks passed");
}
