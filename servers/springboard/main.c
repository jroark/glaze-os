/*
 * SpringBoard: the first user process. Owns the framebuffer and input,
 * draws the lock and home screens, launches apps (one at a time, like
 * iPhone OS 1-3), composites their surfaces, and animates transitions.
 */

#include "sb_internal.h"

#include <glaze/res.h>
#include <glaze/sys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum state {
    ST_LOCK,
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
#define LAUNCH_TIMEOUT  3000
#define SURFACE_BYTES   (SB_APP_W * SB_APP_H * 2)
#define SHM_HEADER      4096

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
    sb_draw_wallpaper(&bb, &view, gfx_r(0, 0, SCREEN_W, SCREEN_H));
    sb_draw_lock(&bb, &view, 0);
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
    flush_all();
}

static void render_grid(void)
{
    gfx_rect r = gfx_r(0, SB_STATUS_H, SCREEN_W, DOCK_Y - SB_STATUS_H);

    gfx_set_clip(&bb, r);
    sb_draw_home(&bb, &view);
    gfx_reset_clip(&bb);
    flush(r);
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
    app_pid = gz_spawn(sb_apps[index].path, 0);
    if (app_pid < 0) {
        snprintf(msg, sizeof(msg), "springboard: cannot launch %s\n", sb_apps[index].path);
        gz_log(msg, (uint32_t)strlen(msg));
        app_pid = 0;
        return;
    }
    app_index = index;
    state = ST_LAUNCH_WAIT;
    launch_t0 = now_ms();
}

static void begin_zoom_in(void)
{
    view.pressed = -1;
    sb_draw_home(&snap, &view);
    state = ST_ZOOM_IN;
    anim_start(0, 1024, ZOOM_MS);
}

static void begin_zoom_out(void)
{
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
    state = ST_LOCK;
    anim.active = false;
    view.knob_x = 0;
    dragging_knob = false;
    touching = false;
    update_clock(true);
    render_lock();
}

static void begin_unlock(void)
{
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

/* ---- input -------------------------------------------------------------- */

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
        }
        break;
    case GZ_EV_TOUCH_MOVE:
        if (dragging_knob) {
            int k = e->x - grab_dx - (TRACK_X + 2);
            view.knob_x = k < 0 ? 0 : k > KNOB_TRAVEL ? KNOB_TRAVEL : k;
            render_track();
        }
        break;
    case GZ_EV_TOUCH_UP:
        if (!dragging_knob)
            break;
        dragging_knob = false;
        if (view.knob_x >= KNOB_TRAVEL - 6)
            begin_unlock();
        else
            anim_start(view.knob_x, 0, SPRING_MS);
        break;
    }
}

static int page_count(void)
{
    return sb_num_pages();
}

static void home_touch(const struct gz_input_event *e)
{
    int page = (view.scroll_x + SCREEN_W / 2) / SCREEN_W;
    uint32_t t = now_ms();

    switch (e->type) {
    case GZ_EV_TOUCH_DOWN:
        anim.active = false;
        touching = true;
        swiping = false;
        down_x = last_x = e->x;
        down_y = e->y;
        last_move_ms = t;
        velocity = 0;
        view.pressed = sb_hit_icon(e->x, e->y, view.scroll_x);
        if (view.pressed >= 0)
            render_home();
        break;
    case GZ_EV_TOUCH_MOVE: {
        if (!touching)
            break;
        uint32_t dt = t - last_move_ms;
        if (dt > 0)
            velocity = (velocity + (e->x - last_x) * 100 / (int)dt) / 2;
        last_x = e->x;
        last_move_ms = t;
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
    switch (e->code) {
    case GZ_KEY_POWER:
        if (state == ST_HOME || state == ST_APP)
            lock_device();
        break;
    case GZ_KEY_HOME:
        if (state == ST_APP)
            begin_zoom_out();
        else if (state == ST_HOME && view.scroll_x != 0)
            anim_start(view.scroll_x, 0, PAGE_MS);
        break;
    default:
        if (state == ST_APP)
            send_event(e);
        break;
    }
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
            switch (state) {
            case ST_LOCK:
                lock_touch(e);
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
                state = ST_HOME;
                render_home();
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

static void load_settings(void)
{
    int32_t wall = 0;

    if (gz_kv_get("sb.wallpaper", &wall, sizeof(wall)) == 4 && wall >= 0 && wall < sb_num_wallpapers)
        view.wallpaper = wall;
}

int main(void)
{
    init_surfaces();
    sb_draw_init();
    load_settings();
    view.pressed = -1;

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

        if (state == ST_LAUNCH_WAIT && t - launch_t0 > LAUNCH_TIMEOUT)
            begin_zoom_in();                    /* show whatever it has */

        if (app_pid > 0 && !gz_proc_alive(app_pid)) {
            bool was_front = (state == ST_APP || state == ST_ZOOM_IN || state == ST_LAUNCH_WAIT);
            app_pid = 0;
            app_cleanup();
            if (was_front) {
                anim.active = false;
                state = ST_HOME;
                render_home();
            }
        }

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
