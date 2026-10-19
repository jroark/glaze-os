/* Contact sheet of fonts, icons and wallpapers rendered with libgfx. */
#include "hostres.h"

#include <stdio.h>
#include <stdlib.h>

static gfx_image img(const char *name)
{
    char path[256];
    size_t n;
    gfx_image i;
    snprintf(path, sizeof(path), "../assets/gen/%s", name);
    if (!gfx_image_parse(&i, host_load(path, &n), n)) {
        fprintf(stderr, "bad image %s\n", name);
        exit(1);
    }
    return i;
}

static gfx_font font(const char *name)
{
    char path[256];
    size_t n;
    gfx_font f;
    snprintf(path, sizeof(path), "../assets/gen/%s", name);
    if (!gfx_font_parse(&f, host_load(path, &n), n)) {
        fprintf(stderr, "bad font %s\n", name);
        exit(1);
    }
    return f;
}

int main(void)
{
    static uint16_t px[720 * 400];
    gfx_surface s;
    gfx_surface_init(&s, px, 720, 400, 720);
    gfx_fill(&s, gfx_r(0, 0, 720, 400), 0xFFFFFF);

    gfx_image wall = img("wall_aqua.gzi");
    gfx_draw_image(&s, 0, 0, &wall, 255);
    const char *icons[] = { "calendar", "clock", "calculator", "settings", "notes", "photos" };
    const char *labels[] = { "Calendar", "Clock", "Calculator", "Settings", "Notes", "Photos" };
    gfx_font lab = font("sans_b9.gzf");
    for (int i = 0; i < 6; i++) {
        char n[64];
        snprintf(n, sizeof(n), "icon_%s.gzi", icons[i]);
        gfx_image ic = img(n);
        int x = 8 + (i % 4) * 58, y = 30 + (i / 4) * 74;
        gfx_draw_image(&s, x, y, &ic, 255);
        gfx_text_shadow(&s, x + 22 - gfx_text_width(&lab, labels[i]) / 2, y + 46, &lab, labels[i],
                        0xFFFFFF, 0x000000, -1, 255);
    }
    gfx_round_rect_vgradient(&s, gfx_r(10, 200, 220, 40), 8, 0x5A5A5A, 0x111111, 200);
    gfx_image knob = img("ui_knob.gzi");
    gfx_draw_image(&s, 12, 202, &knob, 255);
    gfx_font big = font("sans_r44.gzf");
    gfx_text_center(&s, 0, 100, 240, &big, "9:41", 0xFFFFFF, 255);

    gfx_image dusk = img("wall_dusk.gzi");
    gfx_draw_image(&s, 240, 0, &dusk, 255);
    gfx_image meadow = img("wall_meadow.gzi");
    gfx_draw_image_scaled(&s, gfx_r(480, 0, 120, 160), &meadow, 255);
    gfx_image ph = img("../../userdata/Pictures/Sunset.gzi");
    gfx_draw_image_scaled(&s, gfx_r(600, 0, 120, 160), &ph, 255);

    const char *fonts[] = { "sans_r9.gzf", "sans_r10.gzf", "sans_r12.gzf", "sans_r14.gzf", "sans_r17.gzf",
                            "sans_b9.gzf", "sans_b11.gzf", "sans_b13.gzf", "sans_b17.gzf", "sans_r24.gzf" };
    int y = 165;
    for (int i = 0; i < 10; i++) {
        gfx_font f = font(fonts[i]);
        gfx_text(&s, 484, y, &f, "Glaze 1.0 " GFX_CH_TIMES GFX_CH_DIVIDE GFX_CH_DEGREE " ok", 0x000000, 255);
        y += f.line_height;
    }
    gfx_round_rect(&s, gfx_r(250, 330, 100, 40), 10, 0xE94560, 255);
    gfx_circle(&s, 400, 350, 18, 0x2A86C9, 255);
    gfx_line_aa(&s, 430 * 16, 340 * 16, 470 * 16, 380 * 16, 32, 0x000000, 255);
    host_write_bmp("sheet.bmp", &s);
    return 0;
}
