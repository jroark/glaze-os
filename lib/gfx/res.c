/* Resource helpers shared by the device and host builds (see res.h). */

#include <glaze/res.h>

bool res_image(gfx_image *img, const char *name)
{
    uint32_t size;
    const void *data = res_map(name, &size);

    return data && gfx_image_parse(img, data, size);
}

bool res_font(gfx_font *f, const char *name)
{
    uint32_t size;
    const void *data = res_map(name, &size);

    return data && gfx_font_parse(f, data, size);
}

static const char *const font_files[FONT_COUNT] = {
    "sans_r9.gzf", "sans_r10.gzf", "sans_r11.gzf", "sans_r12.gzf", "sans_r13.gzf",
    "sans_r14.gzf", "sans_r17.gzf", "sans_r24.gzf", "sans_r44.gzf",
    "sans_b8.gzf", "sans_b9.gzf", "sans_b10.gzf", "sans_b11.gzf", "sans_b12.gzf",
    "sans_b13.gzf", "sans_b14.gzf", "sans_b17.gzf", "sans_b20.gzf",
};

const gfx_font *ui_font(int which)
{
    static gfx_font fonts[FONT_COUNT];
    static bool loaded[FONT_COUNT];

    if (which < 0 || which >= FONT_COUNT)
        which = FONT_R12;
    if (!loaded[which]) {
        loaded[which] = res_font(&fonts[which], font_files[which]);
        if (!loaded[which])
            return which == FONT_R12 ? &fonts[which] : ui_font(FONT_R12);
    }
    return &fonts[which];
}
