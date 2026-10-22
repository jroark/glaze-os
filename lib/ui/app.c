/* libui: connection to SpringBoard, event queue, presentation. */

#include "sound.h"
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ui_app ui;

static bool bg_running;         /* keep running in the background */
static bool touching;           /* a touch is down */
static int16_t touch_x, touch_y;
static bool deactivate_held;    /* DEACTIVATE waits behind a cancelled touch */
static bool suspend_next;       /* the next ui_next_event waits for ACTIVATE */

void ui_init(int status_style)
{
    struct gz_msg m, r;

    ui.port = gz_port_create();
    for (int tries = 0; (ui.sb_port = gz_port_lookup(SB_PORT_NAME)) < 0; tries++) {
        if (tries > 200)
            exit(1);
        gz_sleep_ms(5);
    }
    memset(&m, 0, sizeof(m));
    m.w[0] = SB_HELLO;
    m.w[1] = (uint32_t)ui.port;
    m.w[2] = (uint32_t)status_style;
    m.w[3] = (uint32_t)gz_getpid();
    if (gz_send(ui.sb_port, &m, &r) || r.w[0]) {
        puts("ui: SpringBoard refused HELLO");
        exit(1);
    }
    ui.hardware_keyboard = (r.w[4] & GZ_INPUT_STOWAWAY) != 0;
    uint8_t *base = gz_shm_map((int)r.w[1]);
    if (!base)
        exit(1);
    ui.shared = (struct sb_shared *)base;
    res_set_bundle(ui.shared->bundle);
    gfx_surface_init(&ui.surf, (uint16_t *)(base + ui.shared->surface_offset), (int)r.w[2], (int)r.w[3],
                     (int)ui.shared->stride);
    ui.dirty = gfx_r(0, 0, 0, 0);
}

bool ui_in_background(void)
{
    return ui.shared && ui.shared->background != 0;
}

void ui_set_background_running(bool on)
{
    bg_running = on;
    ui.shared->keep_running = on;
}

/* Suspends the whole process (every thread) until SpringBoard brings it
 * back, then drops what it was sent meanwhile up to the ACTIVATE. The
 * kernel refuses if it is already back in front; the wait covers that. */
static void wait_activate(struct gz_input_event *ev)
{
    struct sb_shared *sh = ui.shared;

    gz_proc_set_bg(gz_getpid(), GZ_PROC_SUSPENDED);
    for (;;) {
        while (sh->tail != sh->head) {
            *ev = sh->ev[sh->tail % SB_RING];
            sh->tail++;
            if (ev->type == GZ_EV_APP_ACTIVATE)
                return;
        }
        struct gz_msg m;
        gz_recv(ui.port, &m, -1);
    }
}

int ui_next_event(struct gz_input_event *ev, int timeout_ms)
{
    uint32_t start = gz_time_ms();

    if (deactivate_held) {
        deactivate_held = false;
        *ev = (struct gz_input_event){.type = GZ_EV_APP_DEACTIVATE, .time_ms = gz_time_ms()};
        return 1;
    }
    if (suspend_next) {
        suspend_next = false;
        wait_activate(ev);
        ui_invalidate_all();
        return 1;
    }
    for (;;) {
        struct sb_shared *sh = ui.shared;
        if (sh->tail != sh->head) {
            *ev = sh->ev[sh->tail % SB_RING];
            sh->tail++;
            /* A drag is only as smooth as the app can draw: skip to the
             * newest of consecutive moves rather than fall behind. */
            while (ev->type == GZ_EV_TOUCH_MOVE && sh->tail != sh->head &&
                   sh->ev[sh->tail % SB_RING].type == GZ_EV_TOUCH_MOVE) {
                *ev = sh->ev[sh->tail % SB_RING];
                sh->tail++;
            }
            if (ev->type == GZ_EV_TOUCH_DOWN || ev->type == GZ_EV_TOUCH_MOVE) {
                touching = true;
                touch_x = ev->x;
                touch_y = ev->y;
            } else if (ev->type == GZ_EV_TOUCH_UP)
                touching = false;
            else if (ev->type == GZ_EV_APP_ACTIVATE)
                ui_invalidate_all();
            else if (ev->type == GZ_EV_APP_DEACTIVATE) {
                suspend_next = !bg_running;
                if (touching) {
                    touching = false;
                    deactivate_held = true;
                    *ev = (struct gz_input_event){.type = GZ_EV_TOUCH_CANCEL, .x = touch_x, .y = touch_y,
                                                  .time_ms = gz_time_ms()};
                }
            }
            if (!ui_in_background() && ev->type < GZ_EV_APP_ACTIVATE)
                uisnd_event(ev);
            return 1;
        }
        int wait = timeout_ms;
        if (timeout_ms >= 0) {
            int elapsed = (int)(gz_time_ms() - start);
            if (elapsed >= timeout_ms)
                return 0;
            wait = timeout_ms - elapsed;
        }
        struct gz_msg m;
        if (gz_recv(ui.port, &m, wait) == GZ_ETIMEDOUT)
            return 0;
    }
}

void ui_invalidate(gfx_rect r)
{
    gfx_rect c;

    if (gfx_rect_intersect(r, gfx_r(0, 0, ui.surf.w, ui.surf.h), &c))
        ui.dirty = gfx_rect_union(ui.dirty, c);
}

void ui_invalidate_all(void)
{
    ui.dirty = gfx_r(0, 0, ui.surf.w, ui.surf.h);
}

void ui_present(void)
{
    struct gz_msg m, r;

    if (gfx_rect_empty(ui.dirty))
        return;
    memset(&m, 0, sizeof(m));
    m.w[0] = SB_PRESENT;
    m.w[1] = (uint32_t)ui.dirty.x;
    m.w[2] = (uint32_t)ui.dirty.y;
    m.w[3] = (uint32_t)ui.dirty.w;
    m.w[4] = (uint32_t)ui.dirty.h;
    m.w[5] = (uint32_t)gz_getpid();
    gz_send(ui.sb_port, &m, &r);
    ui.dirty = gfx_r(0, 0, 0, 0);
}

int ui_ease_out(uint32_t t, uint32_t dur)
{
    if (t >= dur)
        return 1024;
    int x = (int)(t * 1024 / dur), inv = 1024 - x;
    return 1024 - inv * inv / 1024 * inv / 1024;
}
