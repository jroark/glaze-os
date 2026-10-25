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
static uint32_t touch_op;
static int touch_error;
static struct touch_calibration preview_calibration;
static struct display_settings display_actual = DISPLAY_DEFAULTS, display_saved = DISPLAY_DEFAULTS;
static int display_error, display_saves;
static int32_t timeout_disk;
static int timeout_len = GZ_ENOENT, timeout_error, timeout_notifications;
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
    return !strcmp(name, SB_PORT_NAME) ? 11 : -1;
}
int gz_notify(int port, uint32_t bits)
{
    if (bits == SB_NOTIFY_DISPLAY) {
        timeout_notifications++;
    }
    return 0;
}
int gz_getpid(void) { return 7; }
int gz_send(int port, const struct gz_msg *msg, struct gz_msg *reply)
{
    touch_op = msg->w[0];
    memset(reply, 0, sizeof(*reply));
    reply->w[0] = (uint32_t)touch_error;
    if (touch_op >= SB_DISPLAY_GET && touch_op <= SB_DISPLAY_RESET) {
        reply->w[0] = (uint32_t)display_error;
        if (touch_op == SB_DISPLAY_CANCEL || (touch_op == SB_DISPLAY_SAVE && display_error))
            display_actual = display_saved;
        if (!display_error) {
            if (touch_op == SB_DISPLAY_PREVIEW || touch_op == SB_DISPLAY_SAVE)
                display_actual = (struct display_settings){DISPLAY_VERSION, (int32_t)msg->w[1], (int32_t)msg->w[2]};
            if (touch_op == SB_DISPLAY_SAVE) {
                display_saved = display_actual;
                display_saves++;
            }
            if (touch_op == SB_DISPLAY_RESET)
                display_actual = display_saved = (struct display_settings)DISPLAY_DEFAULTS;
        }
        reply->w[1] = display_actual.brightness;
        reply->w[2] = display_actual.contrast;
    }
    if (touch_op == SB_TOUCH_PREVIEW)
        memcpy(&preview_calibration, &msg->w[4], sizeof(preview_calibration));
    return 0;
}
int pref_get(const char *key, void *buf, uint32_t len)
{
    if (!strcmp(key, DISPLAY_TIMEOUT_PREF)) {
        if (timeout_len > 0)
            memcpy(buf, &timeout_disk, MIN(len, sizeof(timeout_disk)));
        return timeout_len;
    }
    return -1;
}
int pref_put(const char *key, const void *buf, uint32_t len)
{
    if (!strcmp(key, DISPLAY_TIMEOUT_PREF)) {
        if (timeout_error)
            return timeout_error;
        memcpy(&timeout_disk, buf, sizeof(timeout_disk));
        timeout_len = len;
    }
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
static struct wifi_status test_wifi;
static int test_wifi_error = GZ_ENOENT;
static int test_wifi_connects, test_wifi_disconnects, test_wifi_scans;
int net_wifi_status(struct wifi_status *st) { *st = test_wifi; return test_wifi_error; }
int net_wifi_scan(void) { test_wifi_scans++; return test_wifi_error; }
int net_wifi_connect(const void *ssid, uint32_t length)
{
    test_wifi_connects++;
    test_wifi.state = WIFI_CONNECTING;
    return test_wifi_error;
}
int net_wifi_forget(void) { test_wifi.state = WIFI_DISCONNECTED; return 0; }
int net_wifi_connect_secure(const struct wifi_credentials *c) { test_wifi_connects++; return test_wifi_error; }
int net_wifi_disconnect(void)
{
    test_wifi_disconnects++;
    test_wifi.state = WIFI_DISCONNECTED;
    return test_wifi_error;
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
    struct hit saved[sizeof(hits) / sizeof(hits[0])];
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

static void calibration_tap(int x, int y)
{
    struct gz_input_event e = {.type = GZ_EV_TOUCH_DOWN, .x = x, .y = y};
    require(touch_calibration_event(&e), "sample down was not handled");
    e.type = GZ_EV_TOUCH_UP;
    require(touch_calibration_event(&e), "sample up was not handled");
}

static void calibration_tests(void)
{
    activate(R_TOUCH);
    require(view == V_TOUCH, "calibration row did not open");
    compare_full();
    host_write_bmp("../out/host/settings-touch.bmp", &ui.surf);
    activate(R_TOUCH_START);
    require(view == V_TOUCH_SAMPLE && touch_op == SB_TOUCH_BEGIN, "sampling did not begin");
    host_write_bmp("../out/host/settings-touch-sample.bmp", &ui.surf);
    struct gz_input_event e = {.type = GZ_EV_TOUCH_DOWN, .x = 100, .y = 100};
    touch_calibration_event(&e);
    e.type = GZ_EV_TOUCH_MOVE; e.x += 20;
    touch_calibration_event(&e);
    e.type = GZ_EV_TOUCH_UP;
    touch_calibration_event(&e);
    require(touch_step == 0, "drag advanced a calibration target");
    for (int i = 0; i < TOUCH_POINTS; i++) {
        calibration_tap(touch_targets[i].x - 15, touch_targets[i].y - SB_STATUS_H + 10);
        require(touch_step == i + 1, "tap did not advance a target");
        compare_full();
    }
    require(view == V_TOUCH_TEST && touch_op == SB_TOUCH_PREVIEW, "fifth target did not start preview");
    struct touch_point p;
    touch_apply(&preview_calibration, 100, 100, &p);
    require(p.x == 115 && p.y == 90, "status offset or calibration fit was wrong");
    host_write_bmp("../out/host/settings-touch-test.bmp", &ui.surf);
    touch_error = GZ_ENOSPC;
    activate(R_TOUCH_SAVE);
    require(view == V_TOUCH_TEST && touch_op == SB_TOUCH_SAVE, "save failure left preview");
    compare_full();
    touch_error = 0;
    activate(R_TOUCH_SAVE);
    require(view == V_TOUCH, "successful save did not finish");
    activate(R_TOUCH_RESET);
    require(touch_op == SB_TOUCH_RESET, "restore defaults did not request reset");
    touch_error = GZ_EIO;
    activate(R_TOUCH_RESET);
    require(strstr(touch_msg, "Could not"), "reset failure was not displayed");
    touch_error = 0;
    activate(R_TOUCH_START);
    calibration_tap(100, 100);
    activate(R_TOUCH_RETRY);
    require(touch_step == 0, "retry did not reset targets");
    for (int i = 0; i < TOUCH_POINTS; i++)
        calibration_tap(100, 100);
    require(view == V_TOUCH && touch_op == SB_TOUCH_CANCEL, "invalid samples started a preview");
    activate(R_TOUCH_START);
    e.type = GZ_EV_KEY_DOWN; e.code = GZ_KEY_ESC;
    require(touch_calibration_event(&e), "Esc was not handled");
    require(view == V_TOUCH && touch_op == SB_TOUCH_CANCEL, "Esc did not cancel");
    activate(R_TOUCH_START);
    e.type = GZ_EV_APP_DEACTIVATE;
    require(touch_calibration_event(&e) && view == V_TOUCH, "backgrounding did not end sampling");
    activate(R_TOUCH_START);
    for (int i = 0; i < TOUCH_POINTS; i++)
        calibration_tap(touch_targets[i].x, touch_targets[i].y - SB_STATUS_H);
    e.type = GZ_EV_TOUCH_CAL_END;
    require(touch_calibration_event(&e) && view == V_TOUCH, "server cancellation did not end preview");
    activate(R_TOUCH_START);
    for (int i = 0; i < TOUCH_POINTS; i++)
        calibration_tap(touch_targets[i].x, touch_targets[i].y - SB_STATUS_H);
    clock_ms = touch_until;
    touch_test_tick();
    require(view == V_TOUCH && touch_op == SB_TOUCH_CANCEL, "preview timeout did not restore calibration");
    touch_error = GZ_EIO;
    activate(R_TOUCH_START);
    require(view == V_TOUCH, "failed begin entered sampling");
    touch_error = 0;
}

static void display_tests(void)
{
    view = V_ROOT;
    redraw();
    for (int i = 0; i < nhits; i++)
        require(hits[i].r.y + hits[i].r.h <= UI_H, "root row extends beyond screen");
    host_write_bmp("../out/host/settings-root.bmp", &ui.surf);
    activate(R_DISPLAY);
    require(view == V_DISPLAY && touch_op == SB_DISPLAY_GET, "Display did not query current levels");
    require(display_values.brightness == 3 && display_values.contrast == 5, "wrong display defaults");
    compare_full();
    host_write_bmp("../out/host/settings-display.bmp", &ui.surf);
    struct gz_input_event e = {.type = GZ_EV_TOUCH_DOWN, .x = brightness_rect.x, .y = brightness_rect.y + 15};
    require(display_touch(&e), "brightness down not handled");
    require(display_values.brightness == 1 && touch_op == SB_DISPLAY_PREVIEW, "brightness did not preview minimum");
    require(display_saved.brightness == 3 && !display_saves, "drag persisted before release");
    e.type = GZ_EV_TOUCH_MOVE; e.x = 300;
    require(display_touch(&e) && display_values.brightness == 5, "brightness did not clamp at maximum");
    compare_full();
    e.type = GZ_EV_TOUCH_UP;
    display_touch(&e);
    require(display_saves == 1 && display_saved.brightness == 5 && !display_drag, "release did not save");
    e = (struct gz_input_event){.type = GZ_EV_TOUCH_DOWN, .x = contrast_rect.x, .y = contrast_rect.y + 15};
    require(display_touch(&e) && display_values.contrast == 0, "contrast minimum was unreachable");
    e.type = GZ_EV_TOUCH_MOVE; e.x = 300;
    display_touch(&e);
    require(display_values.contrast == 9, "contrast maximum was unreachable");
    e.type = GZ_EV_TOUCH_UP;
    display_touch(&e);
    require(display_saved.contrast == 9 && display_saves == 2, "contrast did not persist");
    /* Every interrupted-drag path restores the saved physical levels. */
    const uint16_t cancellations[] = {GZ_EV_TOUCH_CANCEL, GZ_EV_APP_DEACTIVATE, GZ_EV_KEY_DOWN};
    for (unsigned i = 0; i < ARRAY_SIZE(cancellations); i++) {
        e = (struct gz_input_event){.type = GZ_EV_TOUCH_DOWN, .x = brightness_rect.x, .y = brightness_rect.y + 15};
        display_touch(&e);
        require(display_actual.brightness == 1, "cancel test did not preview");
        e.type = cancellations[i]; e.code = GZ_KEY_ESC;
        display_touch(&e);
        require(!display_drag && display_values.brightness == 5 && display_actual.brightness == 5,
                "interruption did not restore saved brightness");
        require(display_saves == 2, "cancel saved a drag");
        compare_full();
    }
    e = (struct gz_input_event){.type = GZ_EV_TOUCH_DOWN, .x = brightness_rect.x, .y = brightness_rect.y + 15};
    display_touch(&e);
    display_actual = display_saved;    /* Power cancels server-side, with no pen-up to Settings. */
    e = (struct gz_input_event){.type = GZ_EV_DISPLAY_REVERT, .x = 5, .y = 9};
    require(display_touch(&e) && !display_drag && display_values.brightness == 5,
            "Power rollback did not restore the slider");
    compare_full();
    e = (struct gz_input_event){.type = GZ_EV_TOUCH_DOWN, .x = brightness_rect.x, .y = brightness_rect.y + 15};
    display_touch(&e);
    display_error = GZ_ENOSPC;
    e.type = GZ_EV_TOUCH_UP;
    display_touch(&e);
    require(display_values.brightness == 5 && display_msg && !display_drag, "save failure did not revert");
    compare_full();
    activate(R_DISPLAY_RESET);
    require(display_values.brightness == 5 && strstr(display_msg, "Could not"), "failed reset changed levels");
    display_error = 0;
    activate(R_DISPLAY_RESET);
    require(display_values.brightness == 3 && display_values.contrast == 5, "reset did not restore native defaults");
    compare_full();
    activate(R_BACK);
    require(view == V_ROOT, "Display back did not return to Settings");
}

static void timeout_tests(void)
{
    load();
    require(s_timeout == 60, "missing timeout did not default to one minute");
    activate(R_DISPLAY);
    activate(R_WALLPAPER);
    require(view == V_WALLPAPER, "Display did not open Wallpaper");
    activate(R_BACK);
    require(view == V_DISPLAY, "Wallpaper did not return to Display");
    for (unsigned i = 0; i < ARRAY_SIZE(timeout_seconds); i++) {
        activate(R_TIMEOUT);
        require(view == V_TIMEOUT, "Display did not open Screen Timeout");
        compare_full();
        activate(R_TIMEOUT0 + i);
        require(view == V_DISPLAY && s_timeout == timeout_seconds[i], "timeout selection not applied");
        require(timeout_disk == timeout_seconds[i] && timeout_len == 4, "timeout was not persisted");
        require(timeout_notifications == (int)i + 1, "timeout did not notify SpringBoard");
        s_timeout = -1;
        load();
        require(s_timeout == timeout_seconds[i], "saved timeout not loaded");
        redraw();
        compare_full();
    }
    activate(R_TIMEOUT);
    timeout_error = GZ_ENOSPC;
    activate(R_TIMEOUT0);
    require(view == V_TIMEOUT && s_timeout == 0 && display_msg, "failed write changed timeout");
    require(timeout_notifications == 6 && timeout_disk == 0, "failed write notified or changed disk");
    timeout_error = 0;
    activate(R_BACK);
    require(view == V_DISPLAY, "timeout Back did not return to Display");
    const int32_t invalid[] = {-1, 1, 31, 601, 2147483647};
    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
        timeout_disk = invalid[i];
        load();
        require(s_timeout == 60, "invalid timeout did not default");
    }
    timeout_disk = 30;
    for (int len = 0; len <= 8; len++) {
        timeout_len = len;
        load();
        require(s_timeout == (len == 4 ? 30 : 60), "wrong-size timeout accepted");
    }
    display_msg = NULL;
    s_timeout = 30;                 /* longest picker label, compact row summary */
    redraw();
    host_write_bmp("../out/host/settings-display.bmp", &ui.surf);
    activate(R_TIMEOUT);
    host_write_bmp("../out/host/settings-timeout.bmp", &ui.surf);
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
    for (int v = V_ROOT; v <= V_TIMEOUT; v++) {
        view = (enum view)v;
        pressed = R_NONE;
        redraw();
        compare_full();
        int ids[sizeof(hits) / sizeof(hits[0])], count = nhits;
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
    test_wifi_error = 0;
    test_wifi.state = WIFI_DISCONNECTED;
    wifi_available = true;
    view = V_ROOT;
    redraw();
    press(R_DISPLAY); compare_full();
    press(R_NONE); compare_full();
    test_wifi.count = 1;
    test_wifi.aps[0].ssid_len = 5;
    memcpy(test_wifi.aps[0].ssid, "BE300", 5);
    activate(R_WIFI);
    require(view == V_WIFI && test_wifi_scans == 1, "Wi-Fi page did not scan");
    redraw(); compare_full();
    host_write_bmp("../out/host/settings-wifi.bmp", &ui.surf);
    activate(R_WIFI_AP0);
    require(test_wifi_connects == 1 && wireless_status.state == WIFI_CONNECTING, "Wi-Fi connect");
    test_wifi.state = WIFI_CONNECTED;
    test_wifi.current = test_wifi.aps[0];
    test_wifi.current.channel = 3; test_wifi.current.signal = 100; test_wifi.current.rate = 22;
    net_wifi_status(&wireless_status);
    redraw(); compare_full();
    zone_scroll.offset = ui_scroll_max(&zone_scroll);
    redraw(); compare_full();
    host_write_bmp("../out/host/settings-wifi-details.bmp", &ui.surf);
    gfx_rect disconnect = hit_rect(R_WIFI_DISCONNECT);
    struct gz_input_event tap = { .type = GZ_EV_TOUCH_DOWN,
        .x = disconnect.x + 20, .y = disconnect.y + 15 };
    require(zone_touch(&tap), "Wi-Fi disconnect touch down");
    tap.type = GZ_EV_TOUCH_UP;
    require(zone_touch(&tap), "Wi-Fi disconnect touch up");
    compare_full();
    require(zone_scroll.offset == 0, "Wi-Fi shorter page must clamp scroll");
    require(test_wifi_disconnects == 1 && wireless_status.state == WIFI_DISCONNECTED, "Wi-Fi disconnect");
    test_wifi.aps[0].privacy = 1;
    net_wifi_status(&wireless_status);
    activate(R_WIFI_AP0);
    require(test_wifi_connects == 1 && view == V_WIFI_CREDENTIALS, "secured network must prompt for credentials");
    redraw(); compare_full();
    host_write_bmp("../out/host/settings-wifi-password.bmp", &ui.surf);
    activate(R_WIFI_DONE); redraw(); compare_full();
    strcpy(wifi_password, "bad"); activate(R_WIFI_JOIN);
    require(wifi_error == GZ_EINVAL && test_wifi_connects == 1, "invalid WEP key submitted");
    activate(R_WIFI_SHOW); redraw(); compare_full();
    activate(R_BACK);
    require(view == V_WIFI && !wifi_password[0], "cancel must erase credentials");
    test_wifi_error = GZ_EIO;
    activate(R_WIFI_SCAN);
    redraw(); compare_full();
    activate(R_BACK);
    require(view == V_ROOT, "Wi-Fi back navigation");
    test_wifi_error = GZ_ENOENT;
    wifi_available = false;
    calibration_tests();
    display_tests();
    timeout_tests();
    puts("settings: rendering, navigation, scrolling, sliders, calibration and display PASS");
    return 0;
}

void uisnd_sounded(void) {}
