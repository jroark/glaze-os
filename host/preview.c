/* Render SpringBoard screens on the host for review and golden tests. */
#include "hostres.h"
#include "../servers/springboard/sb_internal.h"

#include <stdio.h>

int main(void)
{
    static uint16_t px[240 * 320];
    gfx_surface s;
    struct gz_datetime dt = { 2009, 1, 1, 9, 41, 0, 4 };
    struct sb_view v = { .pressed = -1, .switcher_pressed = -1, .switcher_minus_pressed = -1, .shimmer_x = 120 };

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

    static uint16_t badges[SB_MAX_APPS] = { [0] = 1, [2] = 12, [5] = 120 };
    v.badges = badges;
    v.pressed = -1;
    v.wallpaper = 0;
    sb_draw_home(&s, &v);
    host_write_bmp("home_badges.bmp", &s);

    struct sb_note b = { .app = 2, .title = "Timer Done", .body = "Clock" };
    sb_draw_banner(&s, &b);
    host_write_bmp("home_banner.bmp", &s);

    static const struct sb_note notes[] = {
        { .app = 2, .title = "Timer Done", .body = "Clock" },
        { .app = 0, .title = "Reminder", .body = "Thursday, January 1, with a body too long for one line" },
    };
    v.lock_notes = notes;
    v.num_lock_notes = 2;
    sb_draw_wallpaper(&s, &v, gfx_r(0, 0, 240, 320));
    sb_draw_lock(&s, &v, 0);
    host_write_bmp("lock_notes.bmp", &s);

    v.num_lock_notes = 0;
    v.pin = (struct ui_passcode){ .title = "Enter Passcode", .cancel = "Cancel", .digits = 2, .pressed = '5' };
    sb_draw_passcode(&s, &v);
    host_write_bmp("passcode.bmp", &s);

    v.pin.digits = 0;
    v.pin.pressed = -1;
    v.pin.msg = "Wrong Passcode";
    sb_draw_passcode(&s, &v);
    host_write_bmp("passcode_wrong.bmp", &s);

    v.switcher_open = true;
    v.num_switcher_apps = 4;
    v.switcher_apps[0] = 2;
    v.switcher_apps[1] = 3;
    v.switcher_apps[2] = 4;
    v.switcher_apps[3] = 0;
    sb_draw_home(&s, &v);
    host_write_bmp("home_switcher.bmp", &s);

    v.switcher_edit = true;
    v.jiggle_phase = 1;
    sb_draw_home(&s, &v);
    host_write_bmp("home_switcher_jiggle.bmp", &s);
    return 0;
}
