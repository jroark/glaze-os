#ifndef CE_DIALOG_TEMPLATE_H
#define CE_DIALOG_TEMPLATE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct ce_dialog_item {
    uint32_t style;
    uint16_t id, klass[64], text[256], text_resource;
    int16_t x, y, width, height;
};
struct ce_dialog_template {
    uint32_t style;
    int16_t width, height;
    uint16_t title[256];
    unsigned count;
    struct ce_dialog_item items[32];
};
bool ce_dialog_decode(struct ce_dialog_template *, const void *, size_t);
#endif
