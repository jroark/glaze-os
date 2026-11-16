/* Push/pop navigation transitions: content slides, the nav bar cross-fades. */

#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdlib.h>
#include "transition.h"

#define TRANSITION_MS 300

static gfx_surface old_s, new_s;
static int capacity;            /* pixels in each buffer */

/* Shaped like the app's surface, portrait or landscape. */
static bool ensure_buffers(void)
{
    int w = ui_w(), h = ui_h();

    if (w * h > capacity)
        uitr_release();
    if (!old_s.px)
        old_s.px = malloc((size_t)w * (size_t)h * 2);
    if (!new_s.px)
        new_s.px = malloc((size_t)w * (size_t)h * 2);
    if (!old_s.px || !new_s.px)
        return false;
    capacity = MAX(capacity, w * h);
    gfx_surface_init(&old_s, old_s.px, w, h, w);
    gfx_surface_init(&new_s, new_s.px, w, h, w);
    return true;
}

/* 2 x 146 KB: each its own mapping (malloc.c), so the pages go back to the
 * kernel while the app sits in the background. */
void uitr_release(void)
{
    free(old_s.px);
    free(new_s.px);
    old_s.px = new_s.px = NULL;
    capacity = 0;
}

#define FRAME_MS    20u     /* two scheduler ticks */

void ui_transition(void (*draw_new)(gfx_surface *s), bool forward)
{
    int w = ui_w(), h = ui_h();
    gfx_rect all = gfx_r(0, 0, w, h);

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
        uint32_t frame = gz_time_ms();
        int p = ui_ease_out(frame - t0, TRANSITION_MS);
        int shift = w * p / 1024;
        gfx_rect body = gfx_r(0, UI_NAV_H, w, h - UI_NAV_H);
        gfx_set_clip(&ui.surf, body);
        if (forward) {
            gfx_blit(&ui.surf, -shift, 0, &old_s, all);
            gfx_blit(&ui.surf, w - shift, 0, &new_s, all);
        } else {
            gfx_blit(&ui.surf, shift, 0, &old_s, all);
            gfx_blit(&ui.surf, shift - w, 0, &new_s, all);
        }
        gfx_set_clip(&ui.surf, gfx_r(0, 0, w, UI_NAV_H));
        gfx_blit(&ui.surf, 0, 0, &old_s, gfx_r(0, 0, w, UI_NAV_H));
        gfx_blit_alpha(&ui.surf, 0, 0, &new_s, gfx_r(0, 0, w, UI_NAV_H), p * 255 / 1024);
        gfx_reset_clip(&ui.surf);
        ui_invalidate_all();
        ui_present();
        if (p >= 1024)
            break;
        /* Pace frames by when they start, not by a fixed sleep after the
         * drawing: a 15 ms sleep on top of a slow frame (two body blits, a
         * blended bar and the present) halved the frame rate on the device.
         * A frame that overruns the period goes straight to the next. */
        uint32_t spent = gz_time_ms() - frame;
        if (spent < FRAME_MS)
            gz_sleep_ms(FRAME_MS - spent);
    }
}
