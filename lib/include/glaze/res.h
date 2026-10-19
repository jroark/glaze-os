/*
 * Resource access: images and fonts from romfs "res/" on the device, or
 * from assets/gen on the host preview build.
 */
#ifndef GLAZE_RES_H
#define GLAZE_RES_H

#include <glaze/gfx.h>

const void *res_map(const char *name, uint32_t *size);
bool res_image(gfx_image *img, const char *name);
bool res_font(gfx_font *f, const char *name);

/* The standard UI fonts, loaded on first use. */
enum {
    FONT_R9, FONT_R10, FONT_R11, FONT_R12, FONT_R13, FONT_R14, FONT_R17, FONT_R24, FONT_R44,
    FONT_B8, FONT_B9, FONT_B10, FONT_B11, FONT_B12, FONT_B13, FONT_B14, FONT_B17, FONT_B20,
    FONT_COUNT
};
const gfx_font *ui_font(int which);

#endif
