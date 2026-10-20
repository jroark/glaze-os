/* Exercise production Settings paint/hit logic against full redraws. */
#define main settings_app_main
#include "../apps/settings/main.c"
#undef main
#include <stdlib.h>
#include "hostres.h"

struct ui_app ui;
static uint16_t screen[UI_W * UI_H], full[UI_W * UI_H];
static uint32_t clock_ms;
static int presents;
static int network_queries, passcode_queries;
static gfx_rect last_present;
static struct gz_datetime rtc = {.year = 2026, .month = 10, .day = 19, .hour = 12, .minute = 34};

uint32_t gz_time_ms(void)
{
    return clock_ms++;
}
void gz_sleep_ms(uint32_t ms)
{
    clock_ms += ms;
}
int gz_rtc_get(struct gz_datetime *dt)
{
    *dt = rtc;
    return 0;
}
int gz_rtc_set(const struct gz_datetime *dt)
{
    rtc = *dt;
    return 0;
}
int gz_sysinfo(struct gz_sysinfo *si)
{
    memset(si, 0, sizeof(*si));
    strcpy(si->build, "test");
    si->total_pages = 4096;
    return 0;
}
int gz_port_lookup(const char *name)
{
    return -1;
}
int gz_notify(int port, uint32_t bits)
{
    return 0;
}
int pref_get(const char *key, void *buf, uint32_t len)
{
    return -1;
}
int pref_put(const char *key, const void *buf, uint32_t len)
{
    return 0;
}
bool passcode_enabled(void)
{
    passcode_queries++;
    return true;
}
bool passcode_check(const char *code)
{
    return true;
}
int passcode_set(const char *code)
{
    return 0;
}
int net_status(struct net_status *st)
{
    network_queries++;
    memset(st, 0, sizeof(*st));
    return 0;
}
int net_ping(uint32_t addr, int timeout)
{
    return -1;
}
char *net_ntoa(uint32_t addr, char *out)
{
    strcpy(out, "10.0.0.1");
    return out;
}
const char *net_strerror(int err)
{
    return "test error";
}
int ntp_sync(void)
{
    return -1;
}
int audio_sound(int snd)
{
    return 0;
}
int audio_sound_reload(void)
{
    return 0;
}
void ui_init(int style)
{
}
int ui_next_event(struct gz_input_event *e, int timeout)
{
    return 0;
}
void ui_invalidate(gfx_rect r)
{
    ui.dirty = gfx_rect_union(ui.dirty, r);
}
void ui_invalidate_all(void)
{
    ui_invalidate(gfx_r(0, 0, UI_W, UI_H));
}
void ui_present(void)
{
    last_present = ui.dirty;
    ui.dirty = gfx_r(0, 0, 0, 0);
    presents++;
}
int ui_ease_out(uint32_t t, uint32_t dur)
{
    return t >= dur ? 1024 : (int)(t * 1024 / dur);
}

static void require(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "settings view %d: %s\n", view, what);
        exit(1);
    }
}

static void compare_full(void)
{
    struct hit saved[40];
    int count = nhits;
    memcpy(saved, hits, sizeof(hits));
    gfx_surface s;
    gfx_surface_init(&s, full, UI_W, UI_H, UI_W);
    memset(full, 0, sizeof(full));
    draw_view(&s, view);
    require(!memcmp(full, screen, sizeof(full)), "partial pixels differ from full draw");
    require(count == nhits && !memcmp(saved, hits, (size_t)count * sizeof(*hits)),
            "partial draw lost hit geometry");
}

int main(void)
{
    gfx_surface_init(&ui.surf, screen, UI_W, UI_H, UI_W);
    for (int i = 0; i < 3; i++)
        require(res_image(&walls[i], wall_names[i]), "wallpaper missing");
    s_volume = 8;
    ui_scroll_reset(&zone_scroll, tz_count() * UI_ROW_H + 24, UI_H - UI_NAV_H);
    redraw();
    network_queries = passcode_queries = 0;
    press(R_DATETIME);
    require(!network_queries && !passcode_queries, "row press queried unrelated services");
    require(last_present.w * last_present.h == CELL_W * UI_ROW_H, "row press presented the whole view");
    for (int v = V_ROOT; v <= V_ZONE; v++) {
        view = (enum view)v;
        pressed = R_NONE;
        redraw();
        compare_full();
        int ids[40], count = nhits;
        for (int i = 0; i < count; i++)
            ids[i] = hits[i].id;
        for (int i = 0; i < count; i++) {
            press(ids[i]);
            compare_full();
            int n = presents;
            press(ids[i]);
            require(presents == n, "unchanged press presented a frame");
            press(R_NONE);
            compare_full();
        }
        go((enum view)v, true);
        compare_full();
    }
    view = V_DATETIME;
    s_auto = 0;
    redraw();
    compare_full();
    press(R_FIELD_UP);
    compare_full();
    press(R_NONE);
    compare_full();
    view = V_ZONE;
    pressed = R_NONE;
    redraw();
    for (int offset = -20; offset < ui_scroll_max(&zone_scroll) + 40; offset += 13) {
        zone_scroll.offset = offset;
        redraw_rect(zone_area());
        require(last_present.y == UI_NAV_H && last_present.h == UI_H - UI_NAV_H, "scroll repainted navbar");
        compare_full();
    }
    ui_scroll_reset(&zone_scroll, tz_count() * UI_ROW_H + 24, UI_H - UI_NAV_H);
    redraw();
    struct gz_input_event ev = {.type = GZ_EV_TOUCH_DOWN, .x = 120, .y = UI_NAV_H + 20};
    require(zone_touch(&ev), "zone touch was not handled");
    compare_full();
    int n = presents;
    ev.type = GZ_EV_TOUCH_MOVE;
    zone_touch(&ev);
    require(presents == n, "stationary zone touch presented a frame");
    ev.y += 80;
    zone_touch(&ev);
    compare_full();
    ev.type = GZ_EV_TOUCH_UP;
    zone_touch(&ev);
    for (int i = 0; i < 200 && zone_scroll.animating; i++) {
        clock_ms += 16;
        int old = zone_scroll.offset;
        ui_scroll_step(&zone_scroll);
        if (old != zone_scroll.offset)
            redraw_rect(zone_area());
        compare_full();
    }
    require(!zone_scroll.animating, "overscroll did not settle");
    view = V_SOUNDS;
    redraw();
    for (int v = 0; v <= AUDIO_VOLUME_MAX; v++) {
        s_volume = v;
        redraw_rect(volume_rect);
        compare_full();
    }
    view = V_PREVIEW;
    pressed = R_NONE;
    redraw();
    host_write_bmp("../out/host/settings-preview.bmp", &ui.surf);
    view = V_DATETIME;
    redraw();
    host_write_bmp("../out/host/settings-datetime.bmp", &ui.surf);
    puts("settings: partial/full pixels, hit geometry, navigation, scrolling and slider PASS");
    return 0;
}
