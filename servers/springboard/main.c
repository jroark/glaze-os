/*
 * SpringBoard: the first user process. Owns the framebuffer and input,
 * draws the lock and home screens, launches apps (one at a time, like
 * iPhone OS 1-3), composites their surfaces, and animates transitions.
 */

#include "sb_internal.h"

#include <glaze/audio.h>
#include <glaze/fs.h>
#include <glaze/notify.h>
#include <glaze/passcode.h>
#include <glaze/prefs.h>
#include <glaze/res.h>
#include <glaze/sys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum state {
    ST_LOCK,
    ST_PASSCODE,        /* keypad after the slide, when a passcode is set */
    ST_UNLOCK,          /* lock bars sliding apart */
    ST_HOME,
    ST_LAUNCH_WAIT,     /* app spawned, waiting for its first frame */
    ST_ZOOM_IN,
    ST_APP,
    ST_ZOOM_OUT,
};

#define FRAME_MS        33
#define UNLOCK_MS       320
#define ZOOM_MS         280
#define PAGE_MS         260
#define SPRING_MS       200
#define FLING_MS        150     /* how far a released knob coasts */
#define LAUNCH_TIMEOUT  3000
#define BANNER_MS       4000
#define PIN_FREE_TRIES  5       /* wrong passcodes before entry is disabled */
#define PIN_WAIT_MS     60000   /* for this long, doubling per further miss */
#define PIN_WAIT_MAX    (15 * 60000)
#define MAX_CLIENTS     4
#define HOLD_MS         1000    /* press on an icon to edit the home screen */
#define JIGGLE_MS       100
#define CARD_POLL_MS    2000    /* look for apps on a card coming or going */
#define SURFACE_BYTES   (SB_APP_W * SB_APP_H * 2)
#define SHM_HEADER      4096

_Static_assert(sizeof(struct sb_shared) <= SHM_HEADER, "sb_shared must fit before the surface");

static gfx_surface fb;          /* the hardware framebuffer */
static gfx_surface bb;          /* back buffer, flushed to fb by rect */
static gfx_surface snap;        /* cached frame underneath animations */
static struct sb_view view;
static enum state state;
static int sb_port;

/* The single app slot and its shared surface (reused for every app). */
static int shm_id;
static uint8_t *shm_base;
static struct sb_shared *shared;
static gfx_surface app_surf;
static int app_pid, app_index = -1, app_port;
static bool app_ready, lock_over_app;

struct anim {
    bool active;
    uint32_t t0, dur;
    int from, to;
};
static struct anim anim;
static uint32_t next_frame;
static uint32_t launch_t0;

/* Touch tracking. */
static bool touching, dragging_knob, swiping;
static int down_x, down_y, grab_dx, last_x;
static uint32_t last_move_ms;
static int velocity;            /* px per 100 ms, smoothed */
static int minute_shown = -1;
static uint32_t shimmer_next;

/* Notification banner over the top of the screen, and the app to open
 * once the current one has zoomed out (a tapped banner's). */
static struct sb_note banner;
static bool banner_queued, banner_visible, banner_touch;
static uint32_t banner_until;
static int launch_after_home = -1;

/* An alert over the home screen: confirm deleting an app, or a message. */
static struct {
    bool open;
    int app;                    /* deleting this app, or -1 for a message */
    char title[48], msg[96];
    gfx_rect r1, r2;
    int pressed;
} alert = { .app = -1, .pressed = -1 };
static uint32_t down_ms, jiggle_next, card_poll_next;

/* Passcode entry. Wrong tries are counted until the right one, in RAM. */
static char pin[UI_PASSCODE_LEN + 1];
static int pin_fails;
static bool pin_wait;
static uint32_t pin_until;
static char pin_msg[40];

/* Request pages of processes that notify for other apps (SB_CLIENT_OPEN). */
static struct {
    int pid, shm;
    struct sb_note_req *req;
} clients[MAX_CLIENTS];

static void load_settings(void);

/* ---- helpers ------------------------------------------------------------ */

static uint32_t now_ms(void)
{
    return gz_time_ms();
}

static void flush(gfx_rect r)
{
    gfx_blit(&fb, r.x, r.y, &bb, r);
}

static void flush_all(void)
{
    flush(gfx_r(0, 0, SCREEN_W, SCREEN_H));
}

static void overlay_banner(void);

static void anim_start(int from, int to, uint32_t dur)
{
    anim.active = true;
    anim.t0 = now_ms();
    anim.dur = dur;
    anim.from = from;
    anim.to = to;
    next_frame = anim.t0;
}

/* Eased progress 0..1024 (ease-out cubic). */
static int anim_progress(bool *done)
{
    uint32_t t = now_ms() - anim.t0;
    if (t >= anim.dur) {
        *done = true;
        return 1024;
    }
    *done = false;
    int x = (int)(t * 1024 / anim.dur);
    int inv = 1024 - x;
    return 1024 - inv * inv / 1024 * inv / 1024;
}

static int lerp(int a, int b, int p)
{
    return a + (b - a) * p / 1024;
}

static gfx_rect lerp_rect(gfx_rect a, gfx_rect b, int p)
{
    return gfx_r(lerp(a.x, b.x, p), lerp(a.y, b.y, p), lerp(a.w, b.w, p), lerp(a.h, b.h, p));
}

static void update_clock(bool force)
{
    struct gz_datetime dt;

    if (gz_rtc_get(&dt))
        return;
    if (!force && dt.minute == minute_shown)
        return;
    minute_shown = dt.minute;
    sb_clock_from(&view.clk, &dt);
}

static uint32_t now_secs(void)
{
    struct gz_datetime dt;

    return gz_rtc_get(&dt) ? 0 : gz_dt_to_secs(&dt);
}

/* ms until the next minute boundary, so the clocks stay current. */
static int ms_to_next_minute(void)
{
    struct gz_datetime dt;

    if (gz_rtc_get(&dt))
        return 1000;
    return (60 - dt.second) * 1000;
}

/* ---- rendering per state ------------------------------------------------ */

static void render_lock(void)
{
    view.num_lock_notes = sb_num_delivered;
    sb_draw_wallpaper(&bb, &view, gfx_r(0, 0, SCREEN_W, SCREEN_H));
    sb_draw_lock(&bb, &view, 0);
    flush_all();
}

static void render_passcode(void)
{
    view.pin.digits = (int)strlen(pin);
    sb_draw_passcode(&bb, &view);
    flush_all();
}

static void render_track(void)
{
    gfx_rect r = gfx_r(TRACK_X - 2, TRACK_Y - 2, TRACK_W + 4, TRACK_H + 4);

    gfx_set_clip(&bb, r);
    sb_draw_lock(&bb, &view, 0);
    gfx_reset_clip(&bb);
    flush(r);
}

static void render_home(void)
{
    sb_draw_home(&bb, &view);
    if (alert.open) {
        bool del = alert.app >= 0;
        ui_draw_alert(&bb, alert.title, alert.msg, del ? "Cancel" : "OK", del ? "Delete" : NULL,
                      &alert.r1, &alert.r2, alert.pressed);
    }
    flush_all();
    overlay_banner();
}

static void show_alert(int app, const char *title, const char *msg)
{
    alert.open = true;
    alert.app = app;
    alert.pressed = -1;
    strlcpy(alert.title, title, sizeof(alert.title));
    strlcpy(alert.msg, msg, sizeof(alert.msg));
    view.pressed = -1;
    render_home();
}

static void render_grid(void)
{
    gfx_rect r = gfx_r(0, SB_STATUS_H, SCREEN_W, DOCK_Y - SB_STATUS_H);

    if (alert.open || view.jiggle) {        /* those cover the dock too */
        render_home();
        return;
    }
    gfx_set_clip(&bb, r);
    sb_draw_home(&bb, &view);
    gfx_reset_clip(&bb);
    flush(r);
    overlay_banner();
}

static void render_app_status(void)
{
    if (app_index < 0)
        return;
    sb_draw_status(&bb, &view.clk, sb_apps[app_index].status_style, false);
    flush(gfx_r(0, 0, SCREEN_W, SB_STATUS_H));
}

static void present_app_rect(gfx_rect r)
{
    gfx_rect c;

    if (!gfx_rect_intersect(r, gfx_r(0, 0, SB_APP_W, SB_APP_H), &c))
        return;
    gfx_blit(&fb, c.x, c.y + SB_STATUS_H, &app_surf, c);
    if (c.y < BANNER_H)
        overlay_banner();
}

/* ---- notification banner ------------------------------------------------ */

static gfx_rect banner_rect(void)
{
    return gfx_r(0, SB_STATUS_H, SCREEN_W, BANNER_H);
}

/* Draws the banner over whatever is under it: the app or the home screen. */
static void overlay_banner(void)
{
    gfx_rect r = banner_rect();

    if (!banner_visible)
        return;
    if (state == ST_APP) {
        gfx_blit(&bb, 0, SB_STATUS_H, &app_surf, gfx_r(0, 0, SB_APP_W, BANNER_H));
    } else {
        gfx_set_clip(&bb, r);
        sb_draw_home(&bb, &view);
        gfx_reset_clip(&bb);
    }
    sb_draw_banner(&bb, &banner);
    flush(r);
}

static void hide_banner(void)
{
    if (!banner_visible)
        return;
    banner_visible = false;
    banner_touch = false;
    if (state == ST_APP)
        present_app_rect(gfx_r(0, 0, SB_APP_W, BANNER_H));
    else if (state == ST_HOME)
        render_home();
}

/* A queued banner waits for the home screen or an app to be up. */
static void show_queued_banner(void)
{
    if (!banner_queued || (state != ST_HOME && state != ST_APP))
        return;
    banner_queued = false;
    banner_visible = true;
    banner_until = now_ms() + BANNER_MS;
    overlay_banner();
}

static void render_app(void)
{
    render_app_status();
    present_app_rect(gfx_r(0, 0, SB_APP_W, SB_APP_H));
}

/* ---- app management ----------------------------------------------------- */

static void app_cleanup(void)
{
    if (app_pid > 0 && gz_proc_alive(app_pid))
        gz_kill(app_pid);
    app_pid = 0;
    app_index = -1;
    app_port = 0;
    app_ready = false;
}

static void launch(int index)
{
    char msg[64];

    app_cleanup();
    shared->head = shared->tail = 0;
    memset(app_surf.px, 0xFF, SURFACE_BYTES);
    if (sb_apps[index].installed) {
        strlcpy(shared->bundle, sb_apps[index].path, sizeof(shared->bundle));
        app_pid = apps_launch(index);
    } else {
        shared->bundle[0] = 0;
        app_pid = gz_spawn(sb_apps[index].path, 0);
    }
    if (app_pid < 0) {
        snprintf(msg, sizeof(msg), "springboard: cannot launch %s (%d)\n", sb_apps[index].path, app_pid);
        gz_log(msg, (uint32_t)strlen(msg));
        app_pid = 0;
        if (sb_apps[index].installed) {
            snprintf(msg, sizeof(msg), "\"%s\" could not be opened.", sb_apps[index].label);
            show_alert(-1, "Cannot Open App", msg);
        }
        return;
    }
    app_index = index;
    state = ST_LAUNCH_WAIT;
    launch_t0 = now_ms();
}

static void begin_zoom_in(void)
{
    banner_visible = banner_touch = false;
    view.pressed = -1;
    sb_draw_home(&snap, &view);
    state = ST_ZOOM_IN;
    anim_start(0, 1024, ZOOM_MS);
}

static void begin_zoom_out(void)
{
    banner_visible = banner_touch = false;
    load_settings();
    view.pressed = -1;
    sb_draw_home(&snap, &view);
    state = ST_ZOOM_OUT;
    anim_start(0, 1024, ZOOM_MS);
}

static void send_event(const struct gz_input_event *ev)
{
    if (!app_ready || shared->head - shared->tail >= SB_RING)
        return;
    struct gz_input_event e = *ev;
    if (e.type == GZ_EV_TOUCH_DOWN || e.type == GZ_EV_TOUCH_MOVE || e.type == GZ_EV_TOUCH_UP)
        e.y -= SB_STATUS_H;
    shared->ev[shared->head % SB_RING] = e;
    shared->head++;
    gz_notify(app_port, SB_NOTIFY_EVENTS);
}

/* ---- lock and power ----------------------------------------------------- */

static void lock_device(void)
{
    lock_over_app = (state == ST_APP);
    banner_visible = banner_touch = false;
    view.jiggle = alert.open = false;
    state = ST_LOCK;
    anim.active = false;
    view.knob_x = 0;
    dragging_knob = false;
    touching = false;
    audio_sound(AUDIO_SND_LOCK);
    update_clock(true);
    render_lock();
}

static void begin_unlock(void)
{
    audio_sound_stop();
    audio_sound(AUDIO_SND_UNLOCK);
    notify_clear_delivered();
    view.num_lock_notes = 0;
    if (lock_over_app && app_index >= 0) {
        gfx_blit(&snap, 0, SB_STATUS_H, &app_surf, gfx_r(0, 0, SB_APP_W, SB_APP_H));
        sb_draw_status(&snap, &view.clk, sb_apps[app_index].status_style, false);
    } else {
        view.scroll_x = 0;
        view.pressed = -1;
        sb_draw_home(&snap, &view);
    }
    state = ST_UNLOCK;
    anim_start(0, SCREEN_H / 2, UNLOCK_MS);
}

/* ---- passcode ----------------------------------------------------------- */

/* "Try again in N minutes" while entry is disabled; true if it changed. */
static bool pin_message(void)
{
    char buf[sizeof(pin_msg)];

    if (!pin_wait)
        return false;
    if ((int)(now_ms() - pin_until) >= 0) {
        pin_wait = false;
        view.pin.msg = NULL;
        return true;
    }
    unsigned min = (pin_until - now_ms() + 59999) / 60000;
    snprintf(buf, sizeof(buf), "Try again in %u minute%s", min, min == 1 ? "" : "s");
    if (view.pin.msg == pin_msg && !strcmp(buf, pin_msg))
        return false;
    strcpy(pin_msg, buf);
    view.pin.msg = pin_msg;
    return true;
}

static void begin_passcode(void)
{
    state = ST_PASSCODE;
    anim.active = false;
    view.knob_x = 0;
    memset(pin, 0, sizeof(pin));
    view.pin = (struct ui_passcode){ .title = "Enter Passcode", .cancel = "Cancel", .pressed = -1 };
    pin_message();
    render_passcode();
}

/* Back to slide to unlock, keeping whatever was in front. */
static void cancel_passcode(void)
{
    memset(pin, 0, sizeof(pin));
    state = ST_LOCK;
    view.knob_x = 0;
    dragging_knob = false;
    touching = false;
    render_lock();
}

static void wrong_passcode(void)
{
    view.pin.msg = "Wrong Passcode";
    if (++pin_fails < PIN_FREE_TRIES)
        return;
    int extra = pin_fails - PIN_FREE_TRIES;
    uint32_t wait = PIN_WAIT_MS << (extra > 4 ? 4 : extra);
    pin_wait = true;
    pin_until = now_ms() + (wait > PIN_WAIT_MAX ? PIN_WAIT_MAX : wait);
    pin_message();
}

static void passcode_key(int k)
{
    size_t n = strlen(pin);

    if (k == UI_PASSCODE_CANCEL) {
        cancel_passcode();
        return;
    }
    if (pin_wait)
        return;
    if (k == '\b' && n > 0) {
        pin[n - 1] = 0;
    } else if (k >= '0' && k <= '9' && n < UI_PASSCODE_LEN) {
        pin[n++] = (char)k;
        if (n == UI_PASSCODE_LEN) {
            render_passcode();              /* show the last dot */
            bool ok = passcode_check(pin);
            memset(pin, 0, sizeof(pin));
            if (ok) {
                pin_fails = 0;
                begin_unlock();
                return;
            }
            wrong_passcode();
        }
    } else {
        return;
    }
    render_passcode();
}

static void passcode_touch(const struct gz_input_event *e)
{
    int k = ui_passcode_hit(sb_passcode_area(), &view.pin, e->x, e->y);

    if (pin_wait && k != UI_PASSCODE_CANCEL)
        k = -1;
    switch (e->type) {
    case GZ_EV_TOUCH_DOWN:
        view.pin.pressed = k;
        if (k >= 0) {
            audio_sound(AUDIO_SND_KEY);
            render_passcode();
        }
        break;
    case GZ_EV_TOUCH_MOVE:
        if (view.pin.pressed >= 0 && k != view.pin.pressed) {
            view.pin.pressed = -1;
            render_passcode();
        }
        break;
    case GZ_EV_TOUCH_UP: {
        int p = view.pin.pressed;
        view.pin.pressed = -1;
        if (p >= 0 && p == k)
            passcode_key(p);
        else if (p >= 0)
            render_passcode();
        break;
    }
    }
}

/* The knob reached the end: unlock, or ask for the passcode first. */
static void slide_done(void)
{
    if (passcode_enabled())
        begin_passcode();
    else
        begin_unlock();
}

/* ---- input -------------------------------------------------------------- */

/* Horizontal speed in px per 100 ms, smoothed. Uses the event timestamps:
 * events can arrive in bursts, so the drain time says nothing about speed. */
static void track_move(const struct gz_input_event *e, bool start)
{
    if (start) {
        velocity = 0;
    } else {
        uint32_t dt = e->time_ms - last_move_ms;
        if (dt > 0)
            velocity = (velocity + (e->x - last_x) * 100 / (int)dt) / 2;
    }
    last_x = e->x;
    last_move_ms = e->time_ms;
}

static void lock_touch(const struct gz_input_event *e)
{
    int knob_left = TRACK_X + 2 + view.knob_x;

    switch (e->type) {
    case GZ_EV_TOUCH_DOWN:
        if (e->x >= knob_left - 6 && e->x < knob_left + KNOB_W + 6 &&
            e->y >= TRACK_Y - 8 && e->y < TRACK_Y + TRACK_H + 8) {
            anim.active = false;
            dragging_knob = true;
            grab_dx = e->x - knob_left;
            track_move(e, true);
        }
        break;
    case GZ_EV_TOUCH_MOVE:
        if (dragging_knob) {
            int k = e->x - grab_dx - (TRACK_X + 2);
            track_move(e, false);
            view.knob_x = k < 0 ? 0 : k > KNOB_TRAVEL ? KNOB_TRAVEL : k;
            render_track();
        }
        break;
    case GZ_EV_TOUCH_UP:
        if (!dragging_knob)
            break;
        dragging_knob = false;
        /* The panel is sampled, so a quick slide can lift off before the
         * last sample reaches the end; carry it on by its speed. */
        if (view.knob_x + velocity * FLING_MS / 100 >= KNOB_TRAVEL - 6)
            slide_done();
        else
            anim_start(view.knob_x, 0, SPRING_MS);
        break;
    }
}

static int page_count(void)
{
    return sb_num_pages();
}

/* After the app list changed: keep the scroll position on a real page. */
static bool rescan(bool card_only)
{
    apps_scan(card_only);
    if (!apps_changed())
        return false;
    int max = (page_count() - 1) * SCREEN_W;
    if (view.scroll_x > max)
        view.scroll_x = max;
    if (app_index >= 0 && !sb_app_shown(app_index))
        app_index = -1;
    return true;
}

static void end_jiggle(void)
{
    if (!view.jiggle)
        return;
    view.jiggle = false;
    render_home();
}

/* The icon goes at once; removing the files takes a few seconds of flash
 * writes, and a failure brings it back with the rescan. */
static void delete_app(int app)
{
    char msg[64];

    sb_apps[app].present = false;
    render_home();
    int rv = apps_uninstall(app);
    rescan(false);
    if (rv) {
        snprintf(msg, sizeof(msg), "%s", fs_strerror(rv));
        show_alert(-1, "Cannot Delete App", msg);
    } else {
        render_home();
    }
}

static bool alert_touch(const struct gz_input_event *e)
{
    if (!alert.open)
        return false;
    int hit = gfx_rect_contains(alert.r1, e->x, e->y) ? 0
            : alert.app >= 0 && gfx_rect_contains(alert.r2, e->x, e->y) ? 1 : -1;
    if (e->type != GZ_EV_TOUCH_UP) {
        if (hit != alert.pressed) {
            alert.pressed = hit;
            render_home();
        }
        return true;
    }
    alert.pressed = -1;
    if (hit < 0) {
        render_home();
        return true;
    }
    alert.open = false;
    if (hit == 1)
        delete_app(alert.app);
    else
        render_home();
    return true;
}

static int delete_hit(int x, int y)
{
    for (int i = 0; i < sb_num_apps; i++) {
        if (sb_app_shown(i) && sb_apps[i].installed && gfx_rect_contains(sb_delete_rect(i, view.scroll_x), x, y))
            return i;
    }
    return -1;
}

static int delete_pressed = -1;

static void home_touch(const struct gz_input_event *e)
{
    int page = (view.scroll_x + SCREEN_W / 2) / SCREEN_W;

    if (alert_touch(e))
        return;
    switch (e->type) {
    case GZ_EV_TOUCH_DOWN:
        anim.active = false;
        touching = true;
        swiping = false;
        down_x = e->x;
        down_y = e->y;
        down_ms = now_ms();
        track_move(e, true);
        if (view.jiggle) {
            delete_pressed = delete_hit(e->x, e->y);
            break;                      /* icons don't open while editing */
        }
        view.pressed = sb_hit_icon(e->x, e->y, view.scroll_x);
        if (view.pressed >= 0)
            render_home();
        break;
    case GZ_EV_TOUCH_MOVE: {
        if (!touching)
            break;
        track_move(e, false);
        if (!swiping && abs(e->x - down_x) > 8 && down_y < DOCK_Y) {
            swiping = true;
            if (view.pressed >= 0) {
                view.pressed = -1;
                render_home();
            }
        }
        if (swiping) {
            int base = page * SCREEN_W, max = (page_count() - 1) * SCREEN_W;
            int sx = base - (e->x - down_x);
            if (sx < 0)
                sx /= 2;                        /* rubber band */
            else if (sx > max)
                sx = max + (sx - max) / 2;
            view.scroll_x = sx;
            render_grid();
        } else if (view.pressed >= 0 && sb_hit_icon(e->x, e->y, view.scroll_x) != view.pressed) {
            view.pressed = -1;
            render_home();
        }
        break;
    }
    case GZ_EV_TOUCH_UP:
        if (!touching)
            break;
        touching = false;
        audio_sound(swiping ? AUDIO_SND_SWIPE : AUDIO_SND_TAP);
        if (view.jiggle && !swiping) {
            int app = delete_pressed;
            delete_pressed = -1;
            if (app >= 0 && delete_hit(e->x, e->y) == app) {
                char msg[96];
                snprintf(msg, sizeof(msg), "Deleting this app will also\ndelete all of its data.");
                char title[48];
                snprintf(title, sizeof(title), "Delete \"%s\"?", sb_apps[app].label);
                show_alert(app, title, msg);
            } else if (sb_hit_icon(e->x, e->y, view.scroll_x) < 0 && e->y < DOCK_Y) {
                end_jiggle();           /* a tap on empty space ends editing */
            }
            break;
        }
        if (swiping) {
            int target = (view.scroll_x + SCREEN_W / 2) / SCREEN_W;
            if (velocity < -40)
                target = (view.scroll_x + SCREEN_W - 1) / SCREEN_W;
            else if (velocity > 40)
                target = view.scroll_x / SCREEN_W;
            if (target < 0)
                target = 0;
            if (target > page_count() - 1)
                target = page_count() - 1;
            anim_start(view.scroll_x, target * SCREEN_W, PAGE_MS);
        } else if (view.pressed >= 0) {
            int app = view.pressed;
            if (sb_hit_icon(e->x, e->y, view.scroll_x) == app)
                launch(app);
            else {
                view.pressed = -1;
                render_home();
            }
        }
        break;
    }
}

static void handle_key(const struct gz_input_event *e)
{
    if (e->type != GZ_EV_KEY_DOWN) {
        if (state == ST_APP)
            send_event(e);
        return;
    }
    if (state == ST_PASSCODE) {
        if (e->code == GZ_KEY_POWER || e->code == GZ_KEY_ESC)
            cancel_passcode();
        else if (e->code == GZ_KEY_BACKSPACE)
            passcode_key('\b');
        return;
    }
    switch (e->code) {
    case GZ_KEY_POWER:
        if (state == ST_HOME || state == ST_APP)
            lock_device();
        break;
    case GZ_KEY_HOME:
        audio_sound_stop();
        if (state == ST_APP)
            begin_zoom_out();
        else if (state == ST_HOME && (view.jiggle || alert.open)) {
            alert.open = false;
            view.jiggle = false;
            render_home();
        } else if (state == ST_HOME && view.scroll_x != 0)
            anim_start(view.scroll_x, 0, PAGE_MS);
        break;
    default:
        if (state == ST_APP)
            send_event(e);
        break;
    }
}

/* A tap on the banner opens its app; the banner swallows that touch. */
static bool banner_touch_event(const struct gz_input_event *e)
{
    if (e->type == GZ_EV_TOUCH_DOWN && banner_visible && gfx_rect_contains(banner_rect(), e->x, e->y)) {
        banner_touch = true;
        return true;
    }
    if (!banner_touch)
        return false;
    if (e->type == GZ_EV_TOUCH_UP) {
        banner_touch = false;
        if (banner_visible && gfx_rect_contains(banner_rect(), e->x, e->y)) {
            int target = banner.app;
            audio_sound_stop();
            audio_sound(AUDIO_SND_TAP);
            if (state == ST_HOME) {
                banner_visible = false;
                launch(target);
            } else if (state == ST_APP && app_index != target) {
                launch_after_home = target;
                begin_zoom_out();
            } else {
                hide_banner();
            }
        }
    }
    return true;
}

static void handle_input(void)
{
    struct gz_input_event evs[16];
    int n;

    while ((n = gz_input_read(evs, 16)) > 0) {
        for (int i = 0; i < n; i++) {
            const struct gz_input_event *e = &evs[i];
            if (e->type == GZ_EV_KEY_DOWN || e->type == GZ_EV_KEY_UP) {
                handle_key(e);
                continue;
            }
            if (e->type == GZ_EV_TEXT) {
                if (state == ST_APP)
                    send_event(e);
                else if (state == ST_PASSCODE && e->code >= '0' && e->code <= '9')
                    passcode_key(e->code);
                continue;
            }
            if (banner_touch_event(e))
                continue;
            switch (state) {
            case ST_LOCK:
                lock_touch(e);
                break;
            case ST_PASSCODE:
                passcode_touch(e);
                break;
            case ST_HOME:
                home_touch(e);
                break;
            case ST_APP:
                if (e->y >= SB_STATUS_H || e->type == GZ_EV_TOUCH_UP)
                    send_event(e);
                break;
            default:
                break;
            }
        }
    }
}

/* ---- notifications ------------------------------------------------------ */

static bool app_in_front(int app)
{
    return app_ready && app_index == app && (state == ST_APP || state == ST_ZOOM_IN);
}

/* Clock's notes are its timer, which rings until it is dismissed. */
static void note_sound(const struct sb_note *n)
{
    const char *path = n->app >= 0 && n->app < sb_num_apps ? sb_apps[n->app].path : "";

    if (!strcmp(path, "bin/clock"))
        audio_sound(AUDIO_SND_TIMER);
    else if (!strcmp(path, "bin/calendar"))
        audio_sound(AUDIO_SND_CALENDAR);
    else
        audio_sound(AUDIO_SND_NOTIFY);
}

static void fire(const struct sb_note *n)
{
    note_sound(n);
    if (app_in_front(n->app)) {
        struct gz_input_event e = { .type = GZ_EV_APP_NOTIFY, .code = (uint16_t)n->id, .time_ms = now_ms() };
        send_event(&e);
        return;
    }
    notify_add_badge(n->app, n->badge);
    if (state == ST_LOCK || state == ST_PASSCODE) {
        notify_deliver_locked(n);
        if (state == ST_LOCK && !dragging_knob && !anim.active)
            render_lock();
        return;
    }
    banner = *n;
    banner_queued = true;
    banner_visible = false;
    if (state == ST_HOME)
        render_home();
    show_queued_banner();
}

static void fire_due(void)
{
    struct sb_note n;
    uint32_t now = now_secs();

    while (now && notify_pop_due(now, &n))
        fire(&n);
}

/* The request page of a client process, opening one if asked to. */
static struct sb_note_req *client_req(int pid, bool open, int *shm)
{
    int slot = -1;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].req && clients[i].pid == pid) {
            slot = i;
            break;
        }
    }
    if (slot < 0 && open) {
        /* Reuse the page of a client that has exited. */
        for (int i = 0; i < MAX_CLIENTS && slot < 0; i++) {
            if (!clients[i].req || !gz_proc_alive(clients[i].pid))
                slot = i;
        }
        if (slot < 0)
            return NULL;
        if (!clients[slot].req) {
            clients[slot].shm = gz_shm_create(sizeof(struct sb_note_req));
            clients[slot].req = clients[slot].shm < 0 ? NULL : gz_shm_map(clients[slot].shm);
            if (!clients[slot].req)
                return NULL;
        }
        clients[slot].pid = pid;
    }
    if (slot < 0)
        return NULL;
    if (shm)
        *shm = clients[slot].shm;
    return clients[slot].req;
}

/* Notification requests: the front app's for itself, or a client's for an
 * app named by path. */
static int handle_note_msg(const struct gz_msg *m, struct gz_msg *r)
{
    struct sb_note_req q;
    const struct sb_note_req *src;
    int pid = (int)m->w[3], app, shm = -1;
    uint32_t op = m->w[0];

    if (op == SB_CLIENT_OPEN) {
        if (!client_req(pid, true, &shm))
            return GZ_ENOSPC;
        r->w[1] = (uint32_t)shm;
        return 0;
    }
    if (op >= SB_BADGE_SET_FOR) {
        if (!(src = client_req(pid, false, NULL)))
            return GZ_EPERM;
        op -= SB_BADGE_SET_FOR - SB_BADGE_SET;
        memcpy(&q, src, sizeof(q));
        q.path[sizeof(q.path) - 1] = 0;
        if ((app = notify_app_for_path(q.path)) < 0)
            return GZ_ENOENT;
    } else {
        if (app_pid <= 0 || pid != app_pid || app_index < 0)
            return GZ_EPERM;
        app = app_index;
        memcpy(&q, &shared->req, sizeof(q));
    }
    q.title[sizeof(q.title) - 1] = 0;
    q.body[sizeof(q.body) - 1] = 0;

    switch (op) {
    case SB_BADGE_SET:
        notify_set_badge(app, m->w[1]);
        if (state == ST_HOME)
            render_home();
        return 0;
    case SB_NOTE_POST: {
        uint32_t when = m->w[1] ? m->w[1] : now_secs();
        return notify_post(app, m->w[2], when, m->w[4], q.title, q.body);
    }
    case SB_NOTE_CANCEL:
        return notify_cancel(app, m->w[1]);
    case SB_NOTE_PENDING:
        r->w[1] = notify_pending(app, m->w[1]);
        return 0;
    }
    return GZ_EINVAL;
}

/* ---- app messages ------------------------------------------------------- */

static void handle_msg(int token, struct gz_msg *m)
{
    struct gz_msg r;

    memset(&r, 0, sizeof(r));
    switch (m->w[0]) {
    case SB_HELLO:
        if ((int)m->w[3] != app_pid || app_pid <= 0) {
            r.w[0] = (uint32_t)GZ_EPERM;
            break;
        }
        app_port = (int)m->w[1];
        app_ready = true;
        r.w[1] = (uint32_t)shm_id;
        r.w[2] = SB_APP_W;
        r.w[3] = SB_APP_H;
        r.w[4] = (uint32_t)gz_input_status();
        break;
    case SB_PRESENT: {
        gfx_rect d = gfx_r((int)m->w[1], (int)m->w[2], (int)m->w[3], (int)m->w[4]);
        if (state == ST_APP)
            present_app_rect(d);
        else if (state == ST_LAUNCH_WAIT)
            begin_zoom_in();
        break;
    }
    case SB_SET_STATUS:
        break;
    case SB_BADGE_SET:
    case SB_NOTE_POST:
    case SB_NOTE_CANCEL:
    case SB_NOTE_PENDING:
    case SB_CLIENT_OPEN:
    case SB_BADGE_SET_FOR:
    case SB_NOTE_POST_FOR:
    case SB_NOTE_CANCEL_FOR:
        r.w[0] = (uint32_t)handle_note_msg(m, &r);
        break;
    default:
        r.w[0] = (uint32_t)GZ_EINVAL;
        break;
    }
    gz_reply(token, &r);
}

/* ---- animation frames --------------------------------------------------- */

static void frame(void)
{
    bool done;
    int p = anim_progress(&done);
    int v = lerp(anim.from, anim.to, p);

    switch (state) {
    case ST_LOCK:                       /* knob springing back */
        view.knob_x = v;
        render_track();
        break;
    case ST_UNLOCK:
        gfx_blit(&bb, 0, 0, &snap, gfx_r(0, 0, SCREEN_W, SCREEN_H));
        sb_draw_lock(&bb, &view, v);
        flush_all();
        if (done) {
            view.knob_x = 0;
            if (lock_over_app && app_index >= 0) {
                state = ST_APP;
                render_app();
            } else {
                state = ST_HOME;
                render_home();
            }
        }
        break;
    case ST_HOME:                       /* page snap */
        view.scroll_x = v;
        render_grid();
        break;
    case ST_ZOOM_IN:
    case ST_ZOOM_OUT: {
        gfx_rect icon = sb_icon_rect(app_index, view.scroll_x);
        gfx_rect full = gfx_r(0, SB_STATUS_H, SB_APP_W, SB_APP_H);
        int q = state == ST_ZOOM_IN ? p : 1024 - p;
        gfx_rect r = lerp_rect(icon, full, q);
        gfx_blit(&bb, 0, 0, &snap, gfx_r(0, 0, SCREEN_W, SCREEN_H));
        int alpha = q > 512 ? 255 : 80 + q * 175 / 512;
        gfx_blit_scaled(&bb, r, &app_surf, gfx_r(0, 0, SB_APP_W, SB_APP_H), alpha);
        flush_all();
        if (done) {
            if (state == ST_ZOOM_IN) {
                state = ST_APP;
                render_app();
            } else {
                app_cleanup();
                rescan(false);          /* the app may have been Files installing one */
                state = ST_HOME;
                render_home();
                if (launch_after_home >= 0)
                    launch(launch_after_home);
                launch_after_home = -1;
            }
        }
        break;
    }
    default:
        break;
    }
    if (done)
        anim.active = false;
}

/* ---- main --------------------------------------------------------------- */

static void init_surfaces(void)
{
    uint16_t *fbp = gz_fb_map();
    if (!fbp) {
        puts("springboard: no framebuffer");
        gz_exit(1);
    }
    gfx_surface_init(&fb, fbp, GZ_FB_WIDTH, GZ_FB_HEIGHT, GZ_FB_STRIDE);
    gfx_surface_init(&bb, malloc(SCREEN_W * SCREEN_H * 2), SCREEN_W, SCREEN_H, SCREEN_W);
    gfx_surface_init(&snap, malloc(SCREEN_W * SCREEN_H * 2), SCREEN_W, SCREEN_H, SCREEN_W);

    shm_id = gz_shm_create(SHM_HEADER + SURFACE_BYTES);
    shm_base = gz_shm_map(shm_id);
    if (!bb.px || !snap.px || !shm_base) {
        puts("springboard: out of memory");
        gz_exit(1);
    }
    shared = (struct sb_shared *)shm_base;
    memset(shared, 0, sizeof(*shared));
    shared->surface_offset = SHM_HEADER;
    shared->stride = SB_APP_W;
    gfx_surface_init(&app_surf, (uint16_t *)(shm_base + SHM_HEADER), SB_APP_W, SB_APP_H, SB_APP_W);
}

/* Settings may change while an app runs; re-read them on the way home. */
static void load_settings(void)
{
    int32_t wall = 0, h24 = 0;

    if (pref_get("sb.wallpaper", &wall, sizeof(wall)) == 4 && wall >= 0 && wall < sb_num_wallpapers)
        view.wallpaper = wall;
    if (pref_get("sb.24h", &h24, sizeof(h24)) == 4)
        sb_24h = h24 != 0;
    update_clock(true);
}

int main(void)
{
    init_surfaces();
    sb_draw_init();
    load_settings();
    apps_scan(false);                   /* before notify_load: it keys on bundles */
    apps_changed();
    notify_load();
    view.pressed = -1;
    view.badges = sb_badges;
    view.lock_notes = sb_delivered;

    sb_port = gz_port_create();
    gz_port_register(sb_port, SB_PORT_NAME);
    gz_input_bind(sb_port);

    update_clock(true);
    state = ST_LOCK;
    render_lock();
    puts("springboard: ready");

    for (;;) {
        uint32_t t = now_ms();
        int timeout = ms_to_next_minute();

        if (anim.active) {
            int wait = (int)(next_frame - t);
            timeout = wait < 0 ? 0 : wait;
        } else if (state == ST_LOCK && !dragging_knob && view.knob_x == 0) {
            int wait = (int)(shimmer_next - t);
            timeout = MIN(timeout, wait < 0 ? 0 : wait);
        } else if (state == ST_LAUNCH_WAIT || app_index >= 0) {
            timeout = MIN(timeout, 250);    /* watch the app process */
        }
        if (pin_wait)
            timeout = MIN(timeout, 1000);   /* count down "Try again" */
        if (state == ST_HOME && !anim.active) {
            if (view.jiggle)
                timeout = MIN(timeout, MAX((int)(jiggle_next - t), 0));
            else if (touching && !swiping && view.pressed >= 0)
                timeout = MIN(timeout, MAX((int)(down_ms + HOLD_MS - t), 0));
            if (!touching)
                timeout = MIN(timeout, MAX((int)(card_poll_next - t), 0));
        }
        int due = notify_next_ms(now_secs());
        if (due >= 0)
            timeout = MIN(timeout, due);
        if (banner_visible)
            timeout = MIN(timeout, (int)(banner_until - t) < 0 ? 0 : (int)(banner_until - t));

        struct gz_msg m;
        int tok = gz_recv(sb_port, &m, timeout);
        if (tok == GZ_RECV_NOTIFY) {
            if (m.w[0] & GZ_NOTIFY_INPUT)
                handle_input();
        } else if (tok > 0) {
            handle_msg(tok, &m);
        }

        t = now_ms();
        if (anim.active && (int)(t - next_frame) >= 0) {
            frame();
            next_frame += FRAME_MS;
            if ((int)(t - next_frame) > 0)
                next_frame = t + FRAME_MS;      /* we fell behind; don't burst */
        }

        if (state == ST_LOCK && !dragging_knob && view.knob_x == 0 && !anim.active &&
            (int)(t - shimmer_next) >= 0) {
            view.shimmer_x += 8;
            if (view.shimmer_x > TRACK_X + TRACK_W + 30)
                view.shimmer_x = TRACK_X + KNOB_W - 20;
            render_track();
            shimmer_next = t + 60;
        }

        if (state == ST_HOME && !anim.active) {
            if (!view.jiggle && touching && !swiping && view.pressed >= 0 && t - down_ms >= HOLD_MS) {
                view.jiggle = true;     /* held: edit the home screen */
                view.pressed = -1;
                jiggle_next = t;
            }
            if (view.jiggle && (int)(t - jiggle_next) >= 0) {
                view.jiggle_phase++;
                render_home();
                jiggle_next = t + JIGGLE_MS;
            }
            if (!touching && !alert.open && (int)(t - card_poll_next) >= 0) {
                if (rescan(true))
                    render_home();
                card_poll_next = now_ms() + CARD_POLL_MS;
            }
        }

        if (state == ST_LAUNCH_WAIT && t - launch_t0 > LAUNCH_TIMEOUT)
            begin_zoom_in();                    /* show whatever it has */

        if (app_pid > 0 && !gz_proc_alive(app_pid)) {
            bool was_front = (state == ST_APP || state == ST_ZOOM_IN || state == ST_LAUNCH_WAIT);
            app_pid = 0;
            app_cleanup();
            if (was_front) {
                load_settings();
                rescan(false);
                anim.active = false;
                state = ST_HOME;
                render_home();
            }
        }

        if (pin_message() && state == ST_PASSCODE)
            render_passcode();

        fire_due();
        show_queued_banner();
        if (banner_visible && (int)(now_ms() - banner_until) >= 0)
            hide_banner();

        int old_minute = minute_shown;
        update_clock(false);
        if (minute_shown != old_minute) {
            if (state == ST_LOCK)
                render_lock();
            else if (state == ST_HOME)
                render_home();
            else if (state == ST_APP)
                render_app_status();
        }
    }
}
