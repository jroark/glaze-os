/* Render SpringBoard screens on the host for review and golden tests. */
#include "hostres.h"
#include "../servers/springboard/sb_internal.h"

#include <stdio.h>

int main(void)
{
    static uint16_t px[240 * 320];
    gfx_surface s;
    struct gz_datetime dt = { 2009, 1, 1, 9, 41, 0, 4 };
    struct sb_view v = { .pressed = -1, .shimmer_x = 120 };

    gfx_surface_init(&s, px, 240, 320, 240);
    sb_draw_init();
    sb_clock_from(&v.clk, &dt);

    sb_draw_home(&s, &v);
    sb_draw_lock(&s, &v, 0);
    host_write_bmp("lock.bmp", &s);

    v.knob_x = 60;
    sb_draw_lock(&s, &v, 0);
    host_write_bmp("lock_drag.bmp", &s);

    v.knob_x = 0;
    sb_draw_home(&s, &v);
    sb_draw_lock(&s, &v, 40);
    host_write_bmp("unlock_anim.bmp", &s);

    sb_draw_home(&s, &v);
    host_write_bmp("home.bmp", &s);

    v.pressed = 2;
    v.wallpaper = 1;
    sb_draw_home(&s, &v);
    host_write_bmp("home_dusk_pressed.bmp", &s);
    return 0;
}
