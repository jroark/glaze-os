/* Push/pop navigation transitions: content slides, the nav bar cross-fades. */

#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdlib.h>

#define TRANSITION_MS 300

static gfx_surface old_s, new_s;

static bool ensure_buffers(void)
{
    if (!old_s.px) {
        gfx_surface_init(&old_s, malloc(UI_W * UI_H * 2), UI_W, UI_H, UI_W);
        gfx_surface_init(&new_s, malloc(UI_W * UI_H * 2), UI_W, UI_H, UI_W);
    }
    return old_s.px && new_s.px;
}

void ui_transition(void (*draw_new)(gfx_surface *s), bool forward)
{
    gfx_rect all = gfx_r(0, 0, UI_W, UI_H);

    if (!ensure_buffers()) {
        draw_new(&ui.surf);
        ui_invalidate_all();
        ui_present();
        return;
    }
    gfx_blit(&old_s, 0, 0, &ui.surf, all);
    gfx_reset_clip(&new_s);
    draw_new(&new_s);

    uint32_t t0 = gz_time_ms();
    for (;;) {
        int p = ui_ease_out(gz_time_ms() - t0, TRANSITION_MS);
        int shift = UI_W * p / 1024;
        gfx_rect body = gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H);
        gfx_set_clip(&ui.surf, body);
        if (forward) {
            gfx_blit(&ui.surf, -shift, 0, &old_s, all);
            gfx_blit(&ui.surf, UI_W - shift, 0, &new_s, all);
        } else {
            gfx_blit(&ui.surf, shift, 0, &old_s, all);
            gfx_blit(&ui.surf, shift - UI_W, 0, &new_s, all);
        }
        gfx_set_clip(&ui.surf, gfx_r(0, 0, UI_W, UI_NAV_H));
        gfx_blit(&ui.surf, 0, 0, &old_s, gfx_r(0, 0, UI_W, UI_NAV_H));
        gfx_blit_alpha(&ui.surf, 0, 0, &new_s, gfx_r(0, 0, UI_W, UI_NAV_H), p * 255 / 1024);
        gfx_reset_clip(&ui.surf);
        ui_invalidate_all();
        ui_present();
        if (p >= 1024)
            break;
        gz_sleep_ms(15);
    }
}
