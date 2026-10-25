/*
 * SpringBoard: the first user process. Owns the framebuffer and input,
 * draws the lock and home screens, launches apps (one at a time, like
 * iPhone OS 1-3), composites their surfaces, and animates transitions.
 */

#include "sb_internal.h"
#include "calibration.h"
#include "display.h"
#include "idle.h"

#include <glaze/audio.h>
#include <glaze/fs.h>
#include <glaze/net.h>
#include <glaze/wifi.h>
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
#define DOUBLE_HOME_MS  400     /* double-press Home opens the multitasking bar */
#define CARD_POLL_MS    2000    /* look for apps on a card coming or going */
#define SURFACE_BYTES   (SB_APP_W * GZ_FB_HEIGHT * 2)
#define SHM_HEADER      4096
#define LOW_MEM_PAGES   512     /* keep >= 2 MiB free when backgrounding/spawning */
#define SUSPEND_MS      1000    /* a background app not suspended by now is frozen */

_Static_assert(sizeof(struct sb_shared) <= SHM_HEADER, "sb_shared must fit before the surface");

static gfx_surface fb;          /* the hardware framebuffer */
static gfx_surface bb;          /* back buffer, flushed to fb by rect */
static gfx_surface snap;        /* cached frame underneath animations */
static struct sb_view view;
static enum state state;
static int sb_port;
static struct sb_calibration calibration;
static struct sb_display display_controls;
static struct sb_idle idle;

/* Running app slots (one foreground + background apps, each with its own shm). */
struct sb_running {
    int pid;                    /* > 0 when occupied */
    int app_index;              /* index in sb_apps[], or -1 */
    int port;                   /* event port from SB_HELLO */
    int shm_id;
    uint8_t *shm_base;
    struct sb_shared *shared;
    gfx_surface surf;
    bool ready;
    bool fullscreen;
    bool suspend_due;           /* background: freeze at last_active_ms + SUSPEND_MS */
    uint32_t last_active_ms;
};
static struct sb_running running[SB_MAX_RUNNING];
static int front_slot = -1;
static bool lock_over_app;
static uint32_t last_home_ms;
static bool open_switcher_after_zoom;

static struct sb_running *front(void) { return front_slot >= 0 ? &running[front_slot] : NULL; }
static int front_app(void) { return front_slot >= 0 ? running[front_slot].app_index : -1; }
static bool front_fullscreen(void) { return front_slot >= 0 && running[front_slot].fullscreen; }
static int app_top(void) { return front_fullscreen() ? 0 : SB_STATUS_H; }
static int app_height(void) { return front_fullscreen() ? GZ_FB_HEIGHT : SB_APP_H; }

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
static uint32_t card_status_next;
static bool network_card, wireless_card;
static struct {
    bool open, touching, pressed;
    gfx_rect button;
    struct sb_card_details details;
} card_dialog;
static bool card_icon_touch;

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
    /* Keep the modal visible while clocks, notifications and apps update. */
    if (card_dialog.open || idle.dark)
        return;
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

bool sb_has_program(const char *path)
{
    uint32_t size;

    /* Only address space: romfs pages come in on first touch. */
    return gz_romfs_map(path, &size) != NULL;
}

static bool update_card_status(void)
{
    struct fs_stat st;
    struct net_status net;

    /* A network card can be inserted or pulled too (cf.c's pcmcia_poll);
     * net_status is answered at once, without this process's session. */
    network_card = net_status(&net) == 0 && net.state != NET_NO_CARD;
    wireless_card = network_card && net.kind == GZ_NET_XI825;
    bool mounted = network_card || fs_stat("/Card", &st) == 0;
    bool changed = mounted != view.card_mounted;

    view.card_mounted = mounted;
    card_status_next = now_ms() + CARD_POLL_MS;
    return changed;
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

    if (alert.open || view.jiggle || view.switcher_open) {  /* those cover the dock too */
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
    int app = front_app();

    if (app < 0 || front_fullscreen())
        return;
    sb_draw_status(&bb, &view, sb_apps[app].status_style, false);
    flush(gfx_r(0, 0, SCREEN_W, SB_STATUS_H));
}

static void present_app_rect(gfx_rect r)
{
    struct sb_running *s = front();
    gfx_rect c;

    if (card_dialog.open || idle.dark)
        return;
    if (!s || !gfx_rect_intersect(r, gfx_r(0, 0, SB_APP_W, app_height()), &c))
        return;
    gfx_blit(&fb, c.x, c.y + app_top(), &s->surf, c);
    if (c.y + app_top() < SB_STATUS_H + BANNER_H)
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
    if (state == ST_APP && front()) {
        gfx_blit(&bb, 0, SB_STATUS_H, &front()->surf,
                 gfx_r(0, SB_STATUS_H - app_top(), SB_APP_W, BANNER_H));
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
        present_app_rect(gfx_r(0, SB_STATUS_H - app_top(), SB_APP_W, BANNER_H));
    else if (state == ST_HOME)
        render_home();
}

/* A queued banner waits for the home screen or an app to be up. */
static void show_queued_banner(void)
{
    if (calibration.owner)
        return;
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
    present_app_rect(gfx_r(0, 0, SB_APP_W, app_height()));
}

/* ---- CF card details ---------------------------------------------------- */

static void card_size(char *out, size_t cap, const char *label, uint32_t bytes)
{
    uint32_t unit = bytes >= 1024 * 1024 ? 1024 * 1024 : 1024;

    snprintf(out, cap, "%s: %u.%u %s", label, bytes / unit,
             bytes % unit * 10 / unit, unit == 1024 ? "KB" : "MB");
}

static void render_card_dialog(void)
{
    gfx_blit(&bb, 0, 0, &snap, gfx_r(0, 0, SCREEN_W, SCREEN_H));
    card_dialog.button = sb_draw_card_details(&bb, &card_dialog.details, card_dialog.pressed);
    /* This is the only compositor write allowed while the dialog is open. */
    if (!idle.dark)
        gfx_blit(&fb, 0, 0, &bb, gfx_r(0, 0, SCREEN_W, SCREEN_H));
}

static void close_card_dialog(void)
{
    card_dialog.open = card_dialog.touching = card_dialog.pressed = false;
    if (state == ST_LOCK)
        render_lock();
    else if (state == ST_PASSCODE)
        render_passcode();
    else if (state == ST_HOME)
        render_home();
    else if (state == ST_APP)
        render_app();
}

static void show_card_details(void)
{
    struct sb_card_details *d = &card_dialog.details;
    char (*line)[48] = d->lines;

    memset(d, 0, sizeof(*d));
    if (network_card) {
        struct net_status st;
        strlcpy(d->title, "Network Card", sizeof(d->title));
        if (net_status(&st) < 0) {
            strlcpy(line[0], "Card details unavailable.", sizeof(line[0]));
            d->num_lines = 1;
        } else {
            const char *status = st.state == NET_UP ? "Connected (DHCP)"
                               : st.state == NET_UP_STATIC ? "Connected (static)"
                               : st.state == NET_CONFIGURING ? "Configuring address..." : "Not connected";
            strlcpy(line[0], wireless_card ? "XI-825 Wi-Fi" : "NE2000 Ethernet", sizeof(line[0]));
            strlcpy(line[1], status, sizeof(line[1]));
            snprintf(line[2], sizeof(line[2]), "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
            d->num_lines = 3;
            if (st.state == NET_UP || st.state == NET_UP_STATIC) {
                char addr[16];
                snprintf(line[3], sizeof(line[3]), "IP: %s", net_ntoa(st.addr, addr));
                snprintf(line[4], sizeof(line[4]), "Mask: %s", net_ntoa(st.mask, addr));
                snprintf(line[5], sizeof(line[5]), "Router: %s", net_ntoa(st.gateway, addr));
                snprintf(line[6], sizeof(line[6]), "DNS: %s", net_ntoa(st.dns, addr));
                d->num_lines = 7;
            }
        }
    } else {
        struct fs_info info;
        strlcpy(d->title, "Storage Card", sizeof(d->title));
        if (fs_statvfs("/Card", &info) < 0 || info.mounted) {
            strlcpy(line[0], "Card is no longer mounted.", sizeof(line[0]));
            d->num_lines = 1;
        } else {
            strlcpy(line[0], "CompactFlash storage", sizeof(line[0]));
            strlcpy(line[1], "Mounted at /Card", sizeof(line[1]));
            card_size(line[2], sizeof(line[2]), "Capacity", info.total);
            card_size(line[3], sizeof(line[3]), "Used", info.used);
            card_size(line[4], sizeof(line[4]), "Free", info.total > info.used ? info.total - info.used : 0);
            d->num_lines = 5;
        }
    }
    /* No transition runs while this modal uses the animation snapshot. */
    gfx_blit(&snap, 0, 0, &fb, gfx_r(0, 0, SCREEN_W, SCREEN_H));
    card_dialog.open = true;
    card_dialog.touching = card_dialog.pressed = false;
    render_card_dialog();
}

/* Swallow the entire icon gesture, so its release never reaches an app. */
static bool card_details_event(const struct gz_input_event *e)
{
    if (card_dialog.open) {
        if (e->type == GZ_EV_KEY_UP) {
            if (e->code == GZ_KEY_ESC || e->code == GZ_KEY_ENTER ||
                e->code == GZ_KEY_OK || e->code == GZ_KEY_HOME)
                close_card_dialog();
        } else if (e->type == GZ_EV_KEY_DOWN && e->code == GZ_KEY_POWER) {
            close_card_dialog();
            return false;
        } else if (e->type == GZ_EV_TOUCH_DOWN || e->type == GZ_EV_TOUCH_MOVE || e->type == GZ_EV_TOUCH_UP) {
            bool hit = gfx_rect_contains(card_dialog.button, e->x, e->y);
            if (e->type == GZ_EV_TOUCH_DOWN)
                card_dialog.touching = hit;
            bool pressed = card_dialog.touching && hit;
            if (e->type == GZ_EV_TOUCH_UP) {
                card_dialog.touching = false;
                if (pressed) {
                    audio_sound(AUDIO_SND_TAP);
                    close_card_dialog();
                    return true;
                }
                pressed = false;
            }
            if (pressed != card_dialog.pressed) {
                card_dialog.pressed = pressed;
                render_card_dialog();
            }
        }
        return true;
    }
    bool hit = gfx_rect_contains(sb_card_icon_rect(), e->x, e->y);
    bool stable = !anim.active && !touching && !dragging_knob && !alert.open &&
                  (state == ST_LOCK || state == ST_PASSCODE || state == ST_HOME ||
                   (state == ST_APP && !front_fullscreen()));
    if (e->type == GZ_EV_TOUCH_DOWN && stable && view.card_mounted && hit)
        card_icon_touch = true;
    if (!card_icon_touch)
        return false;
    if (e->type == GZ_EV_TOUCH_UP) {
        card_icon_touch = false;
        if (stable && view.card_mounted && hit) {
            audio_sound(AUDIO_SND_TAP);
            show_card_details();
        }
    }
    return e->type == GZ_EV_TOUCH_DOWN || e->type == GZ_EV_TOUCH_MOVE || e->type == GZ_EV_TOUCH_UP;
}

/* ---- app management ----------------------------------------------------- */

static bool slot_ensure_shm(struct sb_running *s)
{
    if (s->shm_base)
        return true;
    int id = gz_shm_create(SHM_HEADER + SURFACE_BYTES);
    if (id < 0)
        return false;
    uint8_t *base = gz_shm_map(id);
    if (!base)
        return false;
    s->shm_id = id;
    s->shm_base = base;
    s->shared = (struct sb_shared *)base;
    memset(s->shared, 0, sizeof(*s->shared));
    s->shared->surface_offset = SHM_HEADER;
    s->shared->stride = SB_APP_W;
    gfx_surface_init(&s->surf, (uint16_t *)(base + SHM_HEADER), SB_APP_W, SB_APP_H, SB_APP_W);
    return true;
}

/* Slot 0 keeps its surface, so the first launch needs none; the others give
 * theirs back with their app (freed once the app's mapping is gone too). */
static void slot_release_shm(int i)
{
    struct sb_running *s = &running[i];

    if (i == 0 || !s->shm_base)
        return;
    gz_shm_unmap(s->shm_base);
    s->shm_base = NULL;
    s->shared = NULL;
    s->shm_id = 0;
}

static void slot_reset(int i)
{
    struct sb_running *s = &running[i];

    if (calibration.owner == s->pid)
        sb_calibration_cancel(&calibration);
    if (display_controls.owner == s->pid)
        sb_display_cancel(&display_controls);
    s->pid = 0;
    s->app_index = -1;
    s->port = 0;
    s->ready = false;
    s->fullscreen = false;
    s->suspend_due = false;
    s->last_active_ms = 0;
    if (front_slot == i)
        front_slot = -1;
    slot_release_shm(i);
    if (s->shm_base) {
        s->surf.h = SB_APP_H;
        gfx_reset_clip(&s->surf);
    }
}

static void slot_kill(int i)
{
    if (i < 0 || i >= SB_MAX_RUNNING)
        return;
    if (running[i].pid > 0 && gz_proc_alive(running[i].pid))
        gz_kill(running[i].pid);
    slot_reset(i);
}

static int find_slot_by_pid(int pid)
{
    if (pid <= 0)
        return -1;
    for (int i = 0; i < SB_MAX_RUNNING; i++) {
        if (running[i].pid == pid)
            return i;
    }
    return -1;
}

static int find_slot_by_app(int app_index)
{
    if (app_index < 0)
        return -1;
    for (int i = 0; i < SB_MAX_RUNNING; i++) {
        if (running[i].pid > 0 && running[i].app_index == app_index)
            return i;
    }
    return -1;
}

static void send_slot_event(struct sb_running *s, const struct gz_input_event *ev)
{
    if (!s || !s->ready || !s->shared || s->shared->head - s->shared->tail >= SB_RING)
        return;
    struct gz_input_event e = *ev;
    if (e.type == GZ_EV_TOUCH_DOWN || e.type == GZ_EV_TOUCH_MOVE || e.type == GZ_EV_TOUCH_UP)
        e.y -= s->fullscreen ? 0 : SB_STATUS_H;
    s->shared->ev[s->shared->head % SB_RING] = e;
    s->shared->head++;
    gz_notify(s->port, SB_NOTIFY_EVENTS);
}

static void send_event(const struct gz_input_event *ev)
{
    send_slot_event(front(), ev);
}

static void calibration_cancel(void)
{
    int pid = calibration.owner;
    sb_calibration_cancel(&calibration);
    int slot = find_slot_by_pid(pid);
    if (slot >= 0) {
        struct gz_input_event e = { .type = GZ_EV_TOUCH_CAL_END, .time_ms = now_ms() };
        send_slot_event(&running[slot], &e);
    }
}

static void display_cancel(void)
{
    int slot = find_slot_by_pid(display_controls.owner);
    sb_display_cancel(&display_controls);
    if (slot >= 0) {
        struct gz_input_event e = {
            .type = GZ_EV_DISPLAY_REVERT, .time_ms = now_ms(),
            .x = (int16_t)display_controls.current.brightness,
            .y = (int16_t)display_controls.current.contrast,
        };
        send_slot_event(&running[slot], &e);
    }
}

static bool evict_lru_background(int exclude_slot)
{
    int victim = -1;

    for (int i = 0; i < SB_MAX_RUNNING; i++) {
        if (i == front_slot || i == exclude_slot || running[i].pid <= 0)
            continue;
        if (victim < 0 || (int32_t)(running[i].last_active_ms - running[victim].last_active_ms) < 0)
            victim = i;
    }
    if (victim < 0)
        return false;
    slot_kill(victim);
    return true;
}

/* Ends background apps while memory is low, but stops once ending one gives
 * nothing back: what is missing then is not theirs. */
static void reclaim_background_memory(int exclude_slot)
{
    struct gz_sysinfo si;

    if (gz_sysinfo(&si))
        return;
    while (si.free_pages < LOW_MEM_PAGES && evict_lru_background(exclude_slot)) {
        uint32_t before = si.free_pages;
        if (gz_sysinfo(&si) || si.free_pages <= before)
            break;
    }
}

static void sync_switcher(void)
{
    int slots[SB_MAX_RUNNING];
    int n = 0;

    for (int i = 0; i < SB_MAX_RUNNING; i++) {
        if (running[i].pid > 0 && gz_proc_alive(running[i].pid) &&
            running[i].app_index >= 0 && sb_app_shown(running[i].app_index))
            slots[n++] = i;
    }
    /* Sort most-recently-active first. */
    for (int i = 1; i < n; i++) {
        int v = slots[i], j = i;
        while (j > 0 && (int32_t)(running[v].last_active_ms - running[slots[j - 1]].last_active_ms) > 0) {
            slots[j] = slots[j - 1];
            j--;
        }
        slots[j] = v;
    }
    view.num_switcher_apps = n;
    for (int i = 0; i < n; i++)
        view.switcher_apps[i] = running[slots[i]].app_index;
    if (n == 0)
        view.switcher_edit = false;
}

static void open_switcher(void)
{
    alert.open = false;
    view.jiggle = false;
    view.pressed = -1;
    view.switcher_open = true;
    view.switcher_edit = false;
    view.switcher_pressed = -1;
    view.switcher_minus_pressed = -1;
    sync_switcher();
}

static void close_switcher(void)
{
    view.switcher_open = false;
    view.switcher_edit = false;
    view.switcher_pressed = -1;
    view.switcher_minus_pressed = -1;
}

/* Move the foreground app into the background pool instead of killing it. */
static void app_background(void)
{
    calibration_cancel();
    display_cancel();
    if (front_slot < 0)
        return;
    int slot = front_slot;
    struct sb_running *s = &running[slot];
    if (s->pid <= 0 || !gz_proc_alive(s->pid)) {
        slot_kill(slot);
        return;
    }
    s->shared->background = 1;
    s->last_active_ms = now_ms();
    struct gz_input_event e = { .type = GZ_EV_APP_DEACTIVATE, .time_ms = s->last_active_ms };
    send_slot_event(s, &e);
    gz_proc_set_bg(s->pid, GZ_PROC_BG);
    /* libui suspends the app once it has seen DEACTIVATE; one that never
     * gets back to its event loop (or has other threads) is frozen later,
     * unless it asked to keep running. */
    s->suspend_due = true;
    front_slot = -1;
    reclaim_background_memory(-1);
}

/* ms until the next background app is due to be frozen, or -1. */
static int suspend_next_ms(uint32_t t)
{
    int next = -1;

    for (int i = 0; i < SB_MAX_RUNNING; i++) {
        struct sb_running *s = &running[i];
        if (s->pid <= 0 || !s->suspend_due)
            continue;
        int wait = MAX((int)(s->last_active_ms + SUSPEND_MS - t), 0);
        next = next < 0 ? wait : MIN(next, wait);
    }
    return next;
}

static void suspend_due_apps(uint32_t t)
{
    for (int i = 0; i < SB_MAX_RUNNING; i++) {
        struct sb_running *s = &running[i];
        if (s->pid <= 0 || !s->suspend_due || (int)(t - s->last_active_ms) < SUSPEND_MS)
            continue;
        s->suspend_due = false;
        if (!s->shared->keep_running)
            gz_proc_set_bg(s->pid, GZ_PROC_SUSPENDED);
    }
}

/* Benchmark lines (tests/bench.sh): an event, its time in ms, and memory. */
static void bench_log(const char *what, const char *app, uint32_t ms)
{
    struct gz_sysinfo si;

    gz_sysinfo(&si);
    printf("sb: %s%s%s %u ms free %u KiB cache %u KiB\n", what, app ? " " : "", app ? app : "", ms,
           si.free_pages * 4, si.cache_pages * 4);
}

static void begin_zoom_in(void)
{
    banner_visible = banner_touch = false;
    close_switcher();
    view.pressed = -1;
    sb_draw_home(&snap, &view);
    state = ST_ZOOM_IN;
    anim_start(0, 1024, ZOOM_MS);
}

static void begin_zoom_out(void)
{
    calibration_cancel();
    display_cancel();
    banner_visible = banner_touch = false;
    close_switcher();
    load_settings();
    view.pressed = -1;
    sb_draw_home(&snap, &view);
    state = ST_ZOOM_OUT;
    anim_start(0, 1024, ZOOM_MS);
}

static void launch(int index)
{
    char msg[64];

    close_switcher();
    if (front_slot >= 0 && running[front_slot].app_index != index)
        app_background();

    /* Warm resume if the app is already alive in a background slot. */
    int existing = find_slot_by_app(index);
    if (existing >= 0) {
        if (gz_proc_alive(running[existing].pid)) {
            struct sb_running *s = &running[existing];
            front_slot = existing;
            s->shared->background = 0;
            s->suspend_due = false;
            s->last_active_ms = now_ms();
            gz_proc_set_bg(s->pid, GZ_PROC_FG);     /* also thaws it */
            struct gz_input_event e = { .type = GZ_EV_APP_ACTIVATE, .time_ms = s->last_active_ms };
            send_slot_event(s, &e);
            bench_log("launch", sb_apps[index].path, 0);
            begin_zoom_in();
            return;
        }
        slot_kill(existing);
    }

    reclaim_background_memory(-1);
    /* A free slot, preferring one with a surface already (slot 0's); when
     * none is free or no surface can be made, the least recently used app
     * gives up its slot and its surface. */
    int slot = -1;
    for (int i = 0; i < SB_MAX_RUNNING; i++) {
        if (running[i].pid <= 0 && (slot < 0 || (running[i].shm_base && !running[slot].shm_base)))
            slot = i;
    }
    while (slot < 0 || !slot_ensure_shm(&running[slot])) {
        int victim = -1;
        for (int i = 0; i < SB_MAX_RUNNING; i++) {
            if (i != front_slot && running[i].pid > 0 &&
                (victim < 0 || (int32_t)(running[i].last_active_ms - running[victim].last_active_ms) < 0))
                victim = i;
        }
        if (victim < 0) {
            show_alert(-1, "Out of Memory", "Close an app and try again.");
            return;
        }
        slot_kill(victim);
        slot = victim;
    }

    struct sb_running *s = &running[slot];

    s->shared->head = s->shared->tail = 0;
    s->shared->background = 0;
    s->ready = false;
    s->fullscreen = false;
    s->surf.h = SB_APP_H;
    gfx_reset_clip(&s->surf);
    memset(s->surf.px, 0xFF, SURFACE_BYTES);
    if (sb_apps[index].installed) {
        strlcpy(s->shared->bundle, sb_apps[index].path, sizeof(s->shared->bundle));
        s->pid = apps_launch(index);
    } else {
        s->shared->bundle[0] = 0;
        s->pid = gz_spawn(sb_apps[index].path, GZ_SPAWN_APP);
    }
    if (s->pid < 0) {
        snprintf(msg, sizeof(msg), "springboard: cannot launch %s (%d)\n", sb_apps[index].path, s->pid);
        gz_log(msg, (uint32_t)strlen(msg));
        slot_reset(slot);
        if (sb_apps[index].installed) {
            snprintf(msg, sizeof(msg), "\"%s\" could not be opened.", sb_apps[index].label);
            show_alert(-1, "Cannot Open App", msg);
        }
        return;
    }
    s->app_index = index;
    s->last_active_ms = now_ms();
    front_slot = slot;
    state = ST_LAUNCH_WAIT;
    launch_t0 = s->last_active_ms;
}

/* ---- lock and power ----------------------------------------------------- */

static void lock_device(void)
{
    calibration_cancel();
    display_cancel();
    if (state != ST_LOCK && state != ST_PASSCODE && state != ST_UNLOCK)
        lock_over_app = (state == ST_APP || state == ST_ZOOM_IN || state == ST_LAUNCH_WAIT);
    banner_visible = banner_touch = false;
    view.jiggle = alert.open = false;
    close_switcher();
    state = ST_LOCK;
    anim.active = false;
    view.knob_x = 0;
    dragging_knob = false;
    touching = false;
    audio_sound(AUDIO_SND_LOCK);
    update_clock(true);
    render_lock();
}

static void screen_sleep(void)
{
    sb_idle_sleep(&idle);       /* block every compositor output before locking */
    if (front() && idle.touch_down) {
        struct gz_input_event cancel = {
            .type = GZ_EV_TOUCH_CANCEL, .time_ms = now_ms(),
            .x = idle.touch_x, .y = (int16_t)(idle.touch_y - app_top()),
        };
        send_event(&cancel);
    }
    if (card_dialog.open)
        close_card_dialog();
    lock_device();
    memset(pin, 0, sizeof(pin));
    view.pin.pressed = -1;
    last_home_ms = 0;
    gfx_fill(&fb, gfx_r(0, 0, SCREEN_W, SCREEN_H), 0);
    gz_display_power(false);
    puts("springboard: screen off");
}

static void screen_wake(void)
{
    update_clock(true);
    render_lock();             /* paint before restoring illumination */
    shimmer_next = now_ms() + 60;
    gz_display_power(true);
    puts("springboard: screen on");
}

static void begin_unlock(void)
{
    audio_sound_stop();
    audio_sound(AUDIO_SND_UNLOCK);
    notify_clear_delivered();
    view.num_lock_notes = 0;
    if (lock_over_app && front()) {
        gfx_blit(&snap, 0, app_top(), &front()->surf, gfx_r(0, 0, SB_APP_W, app_height()));
        if (!front_fullscreen())
            sb_draw_status(&snap, &view, sb_apps[front_app()].status_style, false);
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
    uint32_t reinstalled = apps_reinstalled_mask();
    for (int i = 0; i < SB_MAX_RUNNING; i++) {
        int app = running[i].app_index;
        if (running[i].pid > 0 && app >= 0 && i != front_slot &&
            ((reinstalled & (1u << app)) || (!card_only && apps_replaced(app))))
            slot_kill(i);
    }
    if (!apps_changed())
        return false;
    int max = (page_count() - 1) * SCREEN_W;
    if (view.scroll_x > max)
        view.scroll_x = max;
    for (int i = 0; i < SB_MAX_RUNNING; i++) {
        if (running[i].pid > 0 && running[i].app_index >= 0 && !sb_app_shown(running[i].app_index))
            slot_kill(i);
    }
    if (view.switcher_open)
        sync_switcher();
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
    int slot = find_slot_by_app(app);

    if (slot >= 0)
        slot_kill(slot);
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

static void switcher_touch(const struct gz_input_event *e)
{
    int top = SCREEN_H - SWITCHER_H;

    switch (e->type) {
    case GZ_EV_TOUCH_DOWN:
        anim.active = false;
        touching = true;
        swiping = false;
        down_x = e->x;
        down_y = e->y;
        down_ms = now_ms();
        if (view.switcher_edit) {
            view.switcher_minus_pressed = sb_hit_switcher_minus(e->x, e->y, view.num_switcher_apps);
            view.switcher_pressed = -1;
        } else {
            view.switcher_pressed = sb_hit_switcher_icon(e->x, e->y, view.num_switcher_apps);
            view.switcher_minus_pressed = -1;
        }
        render_home();
        break;
    case GZ_EV_TOUCH_MOVE:
        if (!touching)
            break;
        if (view.switcher_minus_pressed >= 0 &&
            sb_hit_switcher_minus(e->x, e->y, view.num_switcher_apps) != view.switcher_minus_pressed) {
            view.switcher_minus_pressed = -1;
            render_home();
        }
        if (view.switcher_pressed >= 0 &&
            sb_hit_switcher_icon(e->x, e->y, view.num_switcher_apps) != view.switcher_pressed) {
            view.switcher_pressed = -1;
            render_home();
        }
        break;
    case GZ_EV_TOUCH_UP: {
        if (!touching)
            break;
        touching = false;
        audio_sound(AUDIO_SND_TAP);
        if (view.switcher_edit) {
            int m = view.switcher_minus_pressed;
            view.switcher_minus_pressed = -1;
            if (m >= 0 && sb_hit_switcher_minus(e->x, e->y, view.num_switcher_apps) == m) {
                int s = find_slot_by_app(view.switcher_apps[m]);
                if (s >= 0)
                    slot_kill(s);
                sync_switcher();
                render_home();
            } else if (e->y < top || sb_hit_switcher_icon(e->x, e->y, view.num_switcher_apps) < 0) {
                view.switcher_edit = false;
                render_home();
            } else {
                render_home();
            }
            break;
        }
        int p = view.switcher_pressed;
        view.switcher_pressed = -1;
        if (p >= 0 && sb_hit_switcher_icon(e->x, e->y, view.num_switcher_apps) == p) {
            int app = view.switcher_apps[p];
            close_switcher();
            launch(app);
        } else if (e->y < top) {
            close_switcher();
            render_home();
        } else {
            render_home();
        }
        break;
    }
    }
}

static void home_touch(const struct gz_input_event *e)
{
    int page = (view.scroll_x + SCREEN_W / 2) / SCREEN_W;

    if (alert_touch(e))
        return;
    if (view.switcher_open) {
        switcher_touch(e);
        return;
    }
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
    case GZ_KEY_HOME: {
        uint32_t t = now_ms();
        bool double_press = last_home_ms && (t - last_home_ms) <= DOUBLE_HOME_MS;
        audio_sound_stop();
        if (state == ST_APP) {
            last_home_ms = t;
            open_switcher_after_zoom = false;
            begin_zoom_out();
        } else if (state == ST_ZOOM_OUT) {
            if (double_press) {
                open_switcher_after_zoom = true;
                last_home_ms = 0;
            }
        } else if (state == ST_HOME) {
            if (view.switcher_open) {
                if (view.switcher_edit)
                    view.switcher_edit = false;
                else
                    close_switcher();
                last_home_ms = 0;
                render_home();
            } else if (double_press) {
                last_home_ms = 0;
                open_switcher();
                render_home();
            } else {
                last_home_ms = t;
                if (view.jiggle || alert.open) {
                    alert.open = false;
                    view.jiggle = false;
                    render_home();
                } else if (view.scroll_x != 0) {
                    anim_start(view.scroll_x, 0, PAGE_MS);
                }
            }
        }
        break;
    }
    case GZ_KEY_ESC:
        if (state == ST_HOME && view.switcher_open) {
            if (view.switcher_edit)
                view.switcher_edit = false;
            else
                close_switcher();
            render_home();
            break;
        }
        if (state == ST_APP)
            send_event(e);
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
            } else if (state == ST_APP && front_app() != target) {
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
            struct gz_input_event *e = &evs[i];
            enum sb_idle_input action = sb_idle_input(&idle, e, now_ms());
            if (action == SB_IDLE_WAKE)
                screen_wake();
            if (action != SB_IDLE_PASS)
                continue;
            if (calibration.owner && calibration.preview && (int32_t)(now_ms() - calibration.until) >= 0)
                calibration_cancel();
            if (sb_calibration_input(&calibration, e)) {
                /* Sampling must bypass status-bar, banner and other system
                 * hit tests, even when the old calibration is very wrong. */
                send_event(e);
                continue;
            }
            if (calibration.owner && calibration.preview &&
                (e->type == GZ_EV_TOUCH_DOWN || e->type == GZ_EV_TOUCH_MOVE || e->type == GZ_EV_TOUCH_UP)) {
                send_event(e);      /* keep test touches on the test page */
                continue;
            }
            if (card_details_event(e))
                continue;
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
                if (e->y >= app_top() || e->type == GZ_EV_TOUCH_UP)
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
    struct sb_running *s = front();

    return s && s->ready && s->app_index == app && (state == ST_APP || state == ST_ZOOM_IN);
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
        int slot = find_slot_by_pid(pid);
        if (slot < 0 || running[slot].app_index < 0 || !running[slot].shared)
            return GZ_EPERM;
        app = running[slot].app_index;
        memcpy(&q, &running[slot].shared->req, sizeof(q));
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

static bool settings_request_allowed(int pid)
{
    int app = front_app();
    return state == ST_APP && front() && front()->pid == pid && app >= 0 &&
           !sb_apps[app].installed && !strcmp(sb_apps[app].path, "bin/settings");
}

static void handle_msg(int token, struct gz_msg *m)
{
    struct gz_msg r;

    memset(&r, 0, sizeof(r));
    switch (m->w[0]) {
    case SB_DISPLAY_GET:
    case SB_DISPLAY_PREVIEW:
    case SB_DISPLAY_SAVE:
    case SB_DISPLAY_CANCEL:
    case SB_DISPLAY_RESET:
        r.w[0] = settings_request_allowed((int)m->w[3]) ?
            (uint32_t)sb_display_request(&display_controls, m->w[0], (int)m->w[3], (int)m->w[1], (int)m->w[2]) :
            (uint32_t)GZ_EPERM;
        r.w[1] = (uint32_t)display_controls.current.brightness;
        r.w[2] = (uint32_t)display_controls.current.contrast;
        break;
    case SB_TOUCH_BEGIN:
    case SB_TOUCH_PREVIEW:
    case SB_TOUCH_SAVE:
    case SB_TOUCH_CANCEL:
    case SB_TOUCH_RESET: {
        int pid = (int)m->w[3];
        if (!settings_request_allowed(pid)) {
            r.w[0] = (uint32_t)GZ_EPERM;
            break;
        }
        struct touch_calibration candidate;
        memcpy(&candidate, &m->w[4], sizeof(candidate));
        r.w[0] = (uint32_t)sb_calibration_request(&calibration, m->w[0], pid, &candidate, now_ms());
        if (!r.w[0]) {
            if (m->w[0] == SB_TOUCH_BEGIN)
                hide_banner();
            front()->shared->tail = front()->shared->head; /* discard touches in the previous coordinate space */
        }
        break;
    }
    case SB_HELLO: {
        int slot = find_slot_by_pid((int)m->w[3]);
        if (slot < 0) {
            r.w[0] = (uint32_t)GZ_EPERM;
            break;
        }
        struct sb_running *s = &running[slot];
        s->port = (int)m->w[1];
        s->ready = true;
        s->fullscreen = m->w[2] == SB_STATUS_FULLSCREEN;
        s->surf.h = s->fullscreen ? GZ_FB_HEIGHT : SB_APP_H;
        gfx_reset_clip(&s->surf);
        r.w[1] = (uint32_t)s->shm_id;
        r.w[2] = SB_APP_W;
        r.w[3] = (uint32_t)s->surf.h;
        r.w[4] = (uint32_t)gz_input_status();
        break;
    }
    case SB_PRESENT: {
        int sender_pid = (int)m->w[5];
        int slot = sender_pid > 0 ? find_slot_by_pid(sender_pid) : front_slot;
        if (slot < 0 || slot != front_slot)
            break;
        gfx_rect d = gfx_r((int)m->w[1], (int)m->w[2], (int)m->w[3], (int)m->w[4]);
        if (state == ST_APP)
            present_app_rect(d);
        else if (state == ST_LAUNCH_WAIT) {
            int app = front_app();
            bench_log("launch", app >= 0 ? sb_apps[app].path : "?", now_ms() - launch_t0);
            begin_zoom_in();
        }
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
            if (lock_over_app && front()) {
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
        int app = front_app();
        gfx_rect icon = app >= 0 ? sb_icon_rect(app, view.scroll_x) : gfx_r(SCREEN_W / 2, SCREEN_H / 2, 1, 1);
        gfx_rect full = gfx_r(0, app_top(), SB_APP_W, app_height());
        int q = state == ST_ZOOM_IN ? p : 1024 - p;
        gfx_rect r = lerp_rect(icon, full, q);
        gfx_blit(&bb, 0, 0, &snap, gfx_r(0, 0, SCREEN_W, SCREEN_H));
        int alpha = q > 512 ? 255 : 80 + q * 175 / 512;
        if (front())
            gfx_blit_scaled(&bb, r, &front()->surf, gfx_r(0, 0, SB_APP_W, app_height()), alpha);
        flush_all();
        if (done) {
            if (state == ST_ZOOM_IN) {
                state = ST_APP;
                render_app();
            } else {
                app_background();
                rescan(false);          /* the app may have been Files installing one */
                state = ST_HOME;
                if (open_switcher_after_zoom) {
                    open_switcher_after_zoom = false;
                    open_switcher();
                }
                render_home();
                bench_log("home", NULL, 0);
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

    for (int i = 0; i < SB_MAX_RUNNING; i++)
        slot_reset(i);
    if (!bb.px || !snap.px || !slot_ensure_shm(&running[0])) {
        puts("springboard: out of memory");
        gz_exit(1);
    }
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
    sb_calibration_load(&calibration);
    sb_display_load(&display_controls);
    sb_idle_load(&idle, now_ms());
    apps_scan(false);                   /* before notify_load: it keys on bundles */
    apps_changed();
    notify_load();
    view.pressed = -1;
    view.badges = sb_badges;
    view.lock_notes = sb_delivered;

    sb_port = gz_port_create();
    gz_port_register(sb_port, SB_PORT_NAME);
    gz_child_notify(sb_port);           /* GZ_NOTIFY_CHILD when an app exits */
    gz_input_bind(sb_port);

    update_clock(true);
    update_card_status();
    state = ST_LOCK;
    render_lock();
    timesync_start(sb_port);
    idle.last_activity = now_ms();
    puts("springboard: ready");
    bench_log("ready", NULL, now_ms());

    for (;;) {
        uint32_t t = now_ms();
        int timeout = ms_to_next_minute();

        if (anim.active) {
            int wait = (int)(next_frame - t);
            timeout = wait < 0 ? 0 : wait;
        } else if (!idle.dark && state == ST_LOCK && !dragging_knob && view.knob_x == 0) {
            int wait = (int)(shimmer_next - t);
            timeout = MIN(timeout, wait < 0 ? 0 : wait);
        } else if (state == ST_LAUNCH_WAIT) {
            timeout = MIN(timeout, MAX((int)(launch_t0 + LAUNCH_TIMEOUT + 1 - t), 0));
        }
        int freeze = suspend_next_ms(t);
        if (freeze >= 0)
            timeout = MIN(timeout, freeze);
        if (pin_wait)
            timeout = MIN(timeout, 1000);   /* count down "Try again" */
        if (!anim.active)
            timeout = MIN(timeout, MAX((int)(card_status_next - t), 0));
        if (state == ST_HOME && !anim.active) {
            if (view.jiggle || view.switcher_edit)
                timeout = MIN(timeout, MAX((int)(jiggle_next - t), 0));
            else if (touching && !swiping && (view.pressed >= 0 || view.switcher_pressed >= 0))
                timeout = MIN(timeout, MAX((int)(down_ms + HOLD_MS - t), 0));
            if (!touching)
                timeout = MIN(timeout, MAX((int)(card_poll_next - t), 0));
        }
        int due = notify_next_ms(now_secs());
        if (due >= 0)
            timeout = MIN(timeout, due);
        if (banner_visible)
            timeout = MIN(timeout, (int)(banner_until - t) < 0 ? 0 : (int)(banner_until - t));
        if (calibration.owner && calibration.preview)
            timeout = MIN(timeout, MAX((int32_t)(calibration.until - t), 0));
        int idle_wait = sb_idle_wait(&idle, t);
        if (idle_wait >= 0)
            timeout = MIN(timeout, idle_wait);

        struct gz_msg m;
        int tok = gz_recv(sb_port, &m, timeout);
        if (tok == GZ_RECV_NOTIFY) {
            if (m.w[0] & GZ_NOTIFY_INPUT)
                handle_input();
            if (m.w[0] & SB_NOTIFY_DISPLAY)
                sb_idle_load(&idle, now_ms());
            if (m.w[0] & SB_NOTIFY_TIME)
                timesync_wake();
            if (m.w[0] & (SB_NOTIFY_TIME | SB_NOTIFY_CLOCK))
                minute_shown = -1;      /* redraw the clock below */
        } else if (tok > 0) {
            handle_msg(tok, &m);
        }

        t = now_ms();
        if (sb_idle_wait(&idle, t) == 0)
            screen_sleep();
        if (calibration.owner && calibration.preview && (int32_t)(t - calibration.until) >= 0)
            calibration_cancel();
        if (anim.active && (int)(t - next_frame) >= 0) {
            frame();
            next_frame += FRAME_MS;
            if ((int)(t - next_frame) > 0)
                next_frame = t + FRAME_MS;      /* we fell behind; don't burst */
        }

        if (!idle.dark && state == ST_LOCK && !dragging_knob && view.knob_x == 0 && !anim.active &&
            (int)(t - shimmer_next) >= 0) {
            view.shimmer_x += 8;
            if (view.shimmer_x > TRACK_X + TRACK_W + 30)
                view.shimmer_x = TRACK_X + KNOB_W - 20;
            render_track();
            shimmer_next = t + 60;
        }

        if (state == ST_HOME && !anim.active) {
            if (view.switcher_open) {
                if (!view.switcher_edit && touching && view.switcher_pressed >= 0 && t - down_ms >= HOLD_MS) {
                    view.switcher_edit = true;
                    view.switcher_pressed = -1;
                    jiggle_next = t;
                }
                if (view.switcher_edit && (int)(t - jiggle_next) >= 0) {
                    view.jiggle_phase++;
                    render_home();
                    jiggle_next = t + JIGGLE_MS;
                }
            } else {
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
            }
            if (!touching && !alert.open && (int)(t - card_poll_next) >= 0) {
                if (rescan(true))
                    render_home();
                card_poll_next = now_ms() + CARD_POLL_MS;
            }
        }

        if (state == ST_LAUNCH_WAIT && t - launch_t0 > LAUNCH_TIMEOUT)
            begin_zoom_in();                    /* show whatever it has */

        suspend_due_apps(t);
        /* Woken by GZ_NOTIFY_CHILD when one has exited. */
        for (int i = 0; i < SB_MAX_RUNNING; i++) {
            if (running[i].pid <= 0 || gz_proc_alive(running[i].pid))
                continue;
            bool was_front = (i == front_slot) && (state == ST_APP || state == ST_ZOOM_IN || state == ST_LAUNCH_WAIT);
            slot_reset(i);
            if (was_front) {
                load_settings();
                rescan(false);
                anim.active = false;
                state = ST_HOME;
                render_home();
            } else if (state == ST_HOME && view.switcher_open) {
                sync_switcher();
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
        bool card_changed = false;
        if (!anim.active && (int)(now_ms() - card_status_next) >= 0)
            card_changed = update_card_status();
        if (card_changed && card_dialog.open && view.card_mounted)
            close_card_dialog();
        if (card_changed && card_dialog.open && !view.card_mounted) {
            struct sb_card_details *d = &card_dialog.details;
            strlcpy(d->lines[0], "Card is no longer mounted.", sizeof(d->lines[0]));
            d->num_lines = 1;
            card_dialog.touching = card_dialog.pressed = false;
            render_card_dialog();
        }
        if (minute_shown != old_minute || card_changed) {
            if (state == ST_LOCK)
                render_lock();
            else if (state == ST_PASSCODE)
                render_passcode();
            else if (state == ST_HOME)
                render_home();
            else if (state == ST_APP)
                render_app_status();
        }
    }
}
