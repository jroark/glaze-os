/* Settings: network, wallpaper, date & time, passcode lock, about. */

#include <glaze/net.h>
#include <glaze/passcode.h>
#include <glaze/prefs.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <string.h>

enum view { V_ROOT, V_WALLPAPER, V_PREVIEW, V_DATETIME, V_ABOUT, V_NETWORK, V_PASSCODE, V_PIN };

enum {
    R_NONE, R_WALLPAPER, R_DATETIME, R_ABOUT, R_24H,
    R_TZ_DOWN, R_TZ_UP, R_FIELD_DOWN, R_FIELD_UP = R_FIELD_DOWN + 8,
    R_WALL0 = R_FIELD_UP + 8, R_PREV_CANCEL = R_WALL0 + 8, R_PREV_SET, R_BACK,
    R_NETWORK, R_PING, R_PASSCODE, R_PIN_ON, R_PIN_OFF, R_PIN_CHANGE, R_PIN_CANCEL,
};

struct hit {
    gfx_rect r;
    int id;
};

static enum view view = V_ROOT;
static struct hit hits[40];
static int nhits;
static int pressed = R_NONE;
static int preview_wall;
static char ping_result[24];

/* Passcode entry: turning it on asks for a new code twice, changing it
 * asks for the old one first, turning it off asks for the old one only. */
enum pin_step { PIN_OLD, PIN_NEW, PIN_CONFIRM };
static int pin_mode;            /* R_PIN_ON, R_PIN_OFF or R_PIN_CHANGE */
static enum pin_step pin_step;
static char pin_entry[UI_PASSCODE_LEN + 1], pin_new[UI_PASSCODE_LEN + 1];
static struct ui_passcode pin_pc = { .pressed = -1, .dark = true };

static const char *const wall_names[] = { "wall_aqua.gzi", "wall_dusk.gzi", "wall_meadow.gzi" };
static gfx_image walls[3];

/* Persistent settings. */
static int32_t s_wallpaper, s_24h, s_tz;

static void load(void)
{
    pref_get("sb.wallpaper", &s_wallpaper, 4);
    pref_get("sb.24h", &s_24h, 4);
    pref_get("set.tz", &s_tz, 4);
}

static void save(const char *key, int32_t v)
{
    pref_put(key, &v, 4);
}

static void add_hit(gfx_rect r, int id)
{
    if (nhits < (int)(sizeof(hits) / sizeof(hits[0])))
        hits[nhits++] = (struct hit){ r, id };
}

/* ---- grouped table drawing ---------------------------------------------- */

#define CELL_X  10
#define CELL_W  220

static int section_y;

static void cell(gfx_surface *s, int pos, const char *label, const char *value, bool chevron, int id)
{
    gfx_rect r = gfx_r(CELL_X, section_y, CELL_W, UI_ROW_H);
    bool sel = pressed == id && chevron;
    const gfx_font *lf = ui_font(FONT_B13), *vf = ui_font(FONT_R13);

    ui_draw_group_cell(s, r, pos, sel);
    gfx_text(s, r.x + 10, r.y + (r.h - lf->line_height) / 2, lf, label, sel ? 0xFFFFFF : UI_TEXT, 255);
    int right = r.x + r.w - (chevron ? 22 : 10);
    if (value)
        gfx_text(s, right - gfx_text_width(vf, value), r.y + (r.h - vf->line_height) / 2, vf, value,
                 sel ? 0xFFFFFF : UI_TEXT_DETAIL, 255);
    if (chevron)
        ui_draw_chevron(s, r.x + r.w - 15, r.y + 10, sel ? 0xFFFFFF : 0x808080);
    if (id)
        add_hit(r, id);
    section_y += UI_ROW_H;
}

static void switch_cell(gfx_surface *s, int pos, const char *label, bool on, int id)
{
    gfx_rect r = gfx_r(CELL_X, section_y, CELL_W, UI_ROW_H);
    const gfx_font *lf = ui_font(FONT_B13);

    ui_draw_group_cell(s, r, pos, false);
    gfx_text(s, r.x + 10, r.y + (r.h - lf->line_height) / 2, lf, label, UI_TEXT, 255);
    ui_draw_switch(s, r.x + r.w - UI_SWITCH_W - 8, r.y + (r.h - UI_SWITCH_H) / 2, on);
    add_hit(r, id);
    section_y += UI_ROW_H;
}

static void small_button(gfx_surface *s, gfx_rect r, const char *label, int id)
{
    const gfx_font *f = ui_font(FONT_B14);
    bool p = pressed == id;

    gfx_round_rect(s, r, 5, 0x8A8F96, 255);
    gfx_round_rect_vgradient(s, gfx_r(r.x + 1, r.y + 1, r.w - 2, r.h - 2), 4, p ? UI_SELECT_TOP : 0xFFFFFF,
                             p ? UI_SELECT_BOTTOM : 0xE2E2E2, 255);
    gfx_text_center(s, r.x, r.y + (r.h - f->line_height) / 2 - 1, r.w, f, label, p ? 0xFFFFFF : 0x333333, 255);
    add_hit(r, id);
}

/* A cell with a value and -/+ steppers. */
static void stepper_cell(gfx_surface *s, int pos, const char *label, const char *value, int down_id, int up_id)
{
    gfx_rect r = gfx_r(CELL_X, section_y, CELL_W, UI_ROW_H);
    const gfx_font *lf = ui_font(FONT_B13), *vf = ui_font(FONT_R13);

    ui_draw_group_cell(s, r, pos, false);
    gfx_text(s, r.x + 10, r.y + (r.h - lf->line_height) / 2, lf, label, UI_TEXT, 255);
    gfx_text(s, r.x + 92, r.y + (r.h - vf->line_height) / 2, vf, value, UI_TEXT_DETAIL, 255);
    small_button(s, gfx_r(r.x + r.w - 64, r.y + 4, 26, 22), GFX_CH_MINUS, down_id);
    small_button(s, gfx_r(r.x + r.w - 34, r.y + 4, 26, 22), "+", up_id);
    section_y += UI_ROW_H;
}

static void section_gap(void)
{
    section_y += 14;
}

/* ---- views -------------------------------------------------------------- */

static const char *net_state_name(uint32_t state)
{
    switch (state) {
    case NET_CONFIGURING:   return "Connecting" GFX_CH_ELLIPSIS;
    case NET_UP:            return "Connected";
    case NET_UP_STATIC:     return "Connected (static)";
    default:                return "No Card";
    }
}

static void draw_root(gfx_surface *s)
{
    nhits = 0;
    ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H));
    ui_draw_navbar(s, "Settings", NULL, NULL, 0);
    section_y = UI_NAV_H + 12;
    struct net_status st;
    if (net_status(&st) < 0)
        st.state = NET_NO_CARD;
    cell(s, 0, "Network", net_state_name(st.state), true, R_NETWORK);
    section_gap();
    cell(s, 1, "Wallpaper", NULL, true, R_WALLPAPER);
    cell(s, 2, "Date & Time", NULL, true, R_DATETIME);
    cell(s, 3, "Passcode Lock", passcode_enabled() ? "On" : "Off", true, R_PASSCODE);
    section_gap();
    cell(s, 0, "About", NULL, true, R_ABOUT);
}

static gfx_rect back_rect;

static void draw_wallpaper(gfx_surface *s)
{
    nhits = 0;
    ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H));
    ui_draw_navbar(s, "Wallpaper", "Settings", &back_rect, 0);
    add_hit(back_rect, R_BACK);
    for (int i = 0; i < 3; i++) {
        gfx_rect t = gfx_r(12 + i * 76, UI_NAV_H + 16, 64, 85);
        gfx_fill(s, gfx_r(t.x - 2, t.y - 2, t.w + 4, t.h + 4), i == s_wallpaper ? UI_SELECT_BOTTOM : 0xFFFFFF);
        gfx_draw_image_scaled(s, t, &walls[i], 255);
        if (pressed == R_WALL0 + i)
            gfx_fill_alpha(s, t, 0x000000, 90);
        if (i == s_wallpaper)
            gfx_text(s, t.x + t.w - 14, t.y + t.h - 18, ui_font(FONT_B14), GFX_CH_CHECK, 0xFFFFFF, 255);
        add_hit(t, R_WALL0 + i);
    }
    gfx_text_center(s, 0, UI_NAV_H + 112, UI_W, ui_font(FONT_R12), "Tap a wallpaper to preview it.", 0x4C566C, 255);
}

static void draw_preview(gfx_surface *s)
{
    nhits = 0;
    gfx_rect saved = s->clip;
    gfx_surface sh = *s;
    sh.px -= SB_STATUS_H * s->stride;   /* show the wallpaper as it sits on screen */
    sh.clip = gfx_r(0, SB_STATUS_H, UI_W, UI_H);
    gfx_draw_image(&sh, 0, 0, &walls[preview_wall], 255);
    s->clip = saved;
    gfx_rect bar = gfx_r(0, UI_H - 44, UI_W, 44);
    gfx_fill_alpha(s, bar, 0x000000, 150);
    gfx_vgradient(s, gfx_r(0, bar.y, UI_W, 22), 0xFFFFFF, 0xFFFFFF, 25);
    const gfx_font *f = ui_font(FONT_B13);
    gfx_rect cancel = gfx_r(10, bar.y + 8, 100, 28), set = gfx_r(130, bar.y + 8, 100, 28);
    gfx_round_rect(s, cancel, 6, 0x000000, 255);
    gfx_round_rect_vgradient(s, gfx_r(cancel.x + 1, cancel.y + 1, cancel.w - 2, cancel.h - 2), 5,
                             pressed == R_PREV_CANCEL ? 0x333333 : 0x6A6A6A, 0x2A2A2A, 255);
    gfx_text_center(s, cancel.x, cancel.y + 7, cancel.w, f, "Cancel", 0xFFFFFF, 255);
    gfx_round_rect(s, set, 6, 0x000000, 255);
    gfx_round_rect_vgradient(s, gfx_r(set.x + 1, set.y + 1, set.w - 2, set.h - 2), 5,
                             pressed == R_PREV_SET ? 0x1E4FB0 : 0x5E95F2, 0x2A63D4, 255);
    gfx_text_center(s, set.x, set.y + 7, set.w, f, "Set", 0xFFFFFF, 255);
    add_hit(cancel, R_PREV_CANCEL);
    add_hit(set, R_PREV_SET);
}

static const char *const field_names[5] = { "Year", "Month", "Day", "Hour", "Minute" };

static void draw_datetime(gfx_surface *s)
{
    struct gz_datetime dt;
    char buf[24];

    gz_rtc_get(&dt);
    nhits = 0;
    ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H));
    ui_draw_navbar(s, "Date & Time", "Settings", &back_rect, 0);
    add_hit(back_rect, R_BACK);
    section_y = UI_NAV_H + 10;
    switch_cell(s, 1, "24-Hour Time", s_24h, R_24H);
    snprintf(buf, sizeof(buf), "UTC%s%d", s_tz < 0 ? GFX_CH_MINUS : "+", s_tz < 0 ? -s_tz : s_tz);
    stepper_cell(s, 3, "Time Zone", buf, R_TZ_DOWN, R_TZ_UP);
    section_gap();
    int values[5] = { dt.year, dt.month, dt.day, dt.hour, dt.minute };
    for (int i = 0; i < 5; i++) {
        snprintf(buf, sizeof(buf), i == 0 ? "%d" : "%02d", values[i]);
        stepper_cell(s, i == 0 ? 1 : i == 4 ? 3 : 2, field_names[i], buf, R_FIELD_DOWN + i, R_FIELD_UP + i);
    }
}

static void draw_about(gfx_surface *s)
{
    struct gz_sysinfo si;
    char ver[48], cap[16], avail[16], procs[8], up[24];

    gz_sysinfo(&si);
    nhits = 0;
    ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H));
    ui_draw_navbar(s, "About", "Settings", &back_rect, 0);
    add_hit(back_rect, R_BACK);
    snprintf(ver, sizeof(ver), "Glaze 1.0 (%s)", si.build);
    snprintf(cap, sizeof(cap), "%u MB", (si.total_pages * 4 + 1023) / 1024);
    snprintf(avail, sizeof(avail), "%u KB", si.free_pages * 4);
    snprintf(procs, sizeof(procs), "%u", si.nprocs);
    uint32_t sec = si.uptime_ms / 1000;
    snprintf(up, sizeof(up), "%u:%02u:%02u", sec / 3600, sec / 60 % 60, sec % 60);
    section_y = UI_NAV_H + 12;
    cell(s, 1, "Name", "BE-300", false, 0);
    cell(s, 2, "Version", ver, false, 0);
    cell(s, 3, "Model", "Casio BE-300", false, 0);
    section_gap();
    cell(s, 1, "Processor", "NEC VR4131", false, 0);
    cell(s, 2, "Memory", cap, false, 0);
    cell(s, 2, "Available", avail, false, 0);
    cell(s, 2, "Processes", procs, false, 0);
    cell(s, 3, "Uptime", up, false, 0);
}

static void draw_network(gfx_surface *s)
{
    struct net_status st;
    char mac[20], addr[16], mask[16], gw[16], dns[16];

    if (net_status(&st) < 0)
        memset(&st, 0, sizeof(st));
    nhits = 0;
    ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H));
    ui_draw_navbar(s, "Network", "Settings", &back_rect, 0);
    add_hit(back_rect, R_BACK);
    section_y = UI_NAV_H + 12;
    if (st.state == NET_NO_CARD) {
        cell(s, 0, "Status", "No Card", false, 0);
        gfx_text_center(s, 0, section_y + 10, UI_W, ui_font(FONT_R12),
                        "Insert an NE2000 Ethernet card.", 0x4C566C, 255);
        return;
    }
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
    cell(s, 1, "Status", net_state_name(st.state), false, 0);
    cell(s, 3, "MAC", mac, false, 0);
    section_gap();
    bool up = st.state == NET_UP || st.state == NET_UP_STATIC;
    cell(s, 1, "IP Address", up ? net_ntoa(st.addr, addr) : "", false, 0);
    cell(s, 2, "Subnet Mask", up ? net_ntoa(st.mask, mask) : "", false, 0);
    cell(s, 2, "Router", up ? net_ntoa(st.gateway, gw) : "", false, 0);
    cell(s, 3, "DNS", up ? net_ntoa(st.dns, dns) : "", false, 0);
    section_gap();
    cell(s, 0, "Ping Router", ping_result, up, up ? R_PING : 0);
}

static void draw_passcode(gfx_surface *s)
{
    nhits = 0;
    ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H));
    ui_draw_navbar(s, "Passcode Lock", "Settings", &back_rect, 0);
    add_hit(back_rect, R_BACK);
    section_y = UI_NAV_H + 12;
    if (passcode_enabled()) {
        cell(s, 1, "Turn Passcode Off", NULL, true, R_PIN_OFF);
        cell(s, 3, "Change Passcode", NULL, true, R_PIN_CHANGE);
    } else {
        cell(s, 0, "Turn Passcode On", NULL, true, R_PIN_ON);
    }
    gfx_text_center(s, 0, section_y + 10, UI_W, ui_font(FONT_R12),
                    "Asked for after slide to unlock.", 0x4C566C, 255);
}

static gfx_rect pin_area(void)
{
    return gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H);
}

static void draw_pin(gfx_surface *s)
{
    static const char *const prompts[3] = {
        "Enter your passcode", "Enter a new passcode", "Re-enter your passcode",
    };
    const char *title = pin_mode == R_PIN_ON ? "Set Passcode" : pin_mode == R_PIN_OFF ? "Turn Off Passcode"
                                                                                     : "Change Passcode";

    nhits = 0;
    ui_draw_pinstripes(s, pin_area());
    ui_draw_navbar(s, title, NULL, NULL, 0);
    add_hit(ui_draw_bar_button(s, "Cancel", true, pressed == R_PIN_CANCEL, false, 0), R_PIN_CANCEL);
    pin_pc.title = pin_step == PIN_OLD && pin_mode == R_PIN_CHANGE ? "Enter your old passcode" : prompts[pin_step];
    pin_pc.digits = (int)strlen(pin_entry);
    ui_draw_passcode(s, pin_area(), &pin_pc);
}

static void draw_view(gfx_surface *s, enum view v)
{
    switch (v) {
    case V_ROOT: draw_root(s); break;
    case V_WALLPAPER: draw_wallpaper(s); break;
    case V_PREVIEW: draw_preview(s); break;
    case V_DATETIME: draw_datetime(s); break;
    case V_ABOUT: draw_about(s); break;
    case V_NETWORK: draw_network(s); break;
    case V_PASSCODE: draw_passcode(s); break;
    case V_PIN: draw_pin(s); break;
    }
}

static enum view target_view;

static void draw_target(gfx_surface *s)
{
    draw_view(s, target_view);
}

static void go(enum view v, bool forward)
{
    pressed = R_NONE;
    target_view = v;
    ui_transition(draw_target, forward);
    view = v;
    draw_view(&ui.surf, view);      /* refresh hit rects for the live surface */
}

static void redraw(void)
{
    draw_view(&ui.surf, view);
    ui_invalidate_all();
    ui_present();
}

/* ---- actions ------------------------------------------------------------ */

static void adjust_field(int field, int delta)
{
    struct gz_datetime dt;
    static const uint8_t mdays[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    gz_rtc_get(&dt);
    switch (field) {
    case 0: dt.year = (uint16_t)(dt.year + delta < 2000 ? 2000 : dt.year + delta > 2099 ? 2099 : dt.year + delta); break;
    case 1: dt.month = (uint8_t)((dt.month - 1 + delta + 12) % 12 + 1); break;
    case 2: dt.day = (uint8_t)((dt.day - 1 + delta + 31) % 31 + 1); break;
    case 3: dt.hour = (uint8_t)((dt.hour + delta + 24) % 24); break;
    case 4: dt.minute = (uint8_t)((dt.minute + delta + 60) % 60); break;
    }
    int leap = (dt.year % 4 == 0 && dt.year % 100) || dt.year % 400 == 0;
    int maxd = mdays[dt.month - 1] - (dt.month == 2 && !leap);
    if (dt.day > maxd)
        dt.day = (uint8_t)maxd;
    dt.second = 0;
    gz_rtc_set(&dt);
}

static void pin_start(int mode)
{
    pin_mode = mode;
    pin_step = mode == R_PIN_ON ? PIN_NEW : PIN_OLD;
    memset(pin_entry, 0, sizeof(pin_entry));
    pin_pc.msg = NULL;
    pin_pc.pressed = -1;
    go(V_PIN, true);
}

/* A full code was entered at the current step. */
static void pin_complete(void)
{
    switch (pin_step) {
    case PIN_OLD:
        if (!passcode_check(pin_entry)) {
            pin_pc.msg = "Wrong passcode. Try again.";
            break;
        }
        if (pin_mode == R_PIN_OFF) {
            passcode_set(NULL);
            go(V_PASSCODE, false);
            return;
        }
        pin_step = PIN_NEW;
        pin_pc.msg = NULL;
        break;
    case PIN_NEW:
        strcpy(pin_new, pin_entry);
        pin_step = PIN_CONFIRM;
        pin_pc.msg = NULL;
        break;
    case PIN_CONFIRM:
        if (!strcmp(pin_new, pin_entry)) {
            passcode_set(pin_new);
            go(V_PASSCODE, false);
            return;
        }
        pin_step = PIN_NEW;
        pin_pc.msg = "Passcodes did not match. Try again.";
        break;
    }
    memset(pin_entry, 0, sizeof(pin_entry));
}

static void pin_key(int k)
{
    size_t n = strlen(pin_entry);

    if (k == '\b' && n > 0) {
        pin_entry[n - 1] = 0;
    } else if (k >= '0' && k <= '9' && n < UI_PASSCODE_LEN) {
        pin_entry[n++] = (char)k;
        if (n == UI_PASSCODE_LEN) {
            redraw();               /* show the last dot */
            pin_complete();
        }
    }
}

static void activate(int id)
{
    if (id == R_BACK || id == R_PIN_CANCEL) {
        go(view == V_PREVIEW ? V_WALLPAPER : view == V_PIN ? V_PASSCODE : V_ROOT, false);
        return;
    }
    switch (id) {
    case R_24H:
        s_24h = !s_24h;
        save("sb.24h", s_24h);
        break;
    case R_TZ_DOWN:
    case R_TZ_UP:
        s_tz += id == R_TZ_UP ? 1 : -1;
        if (s_tz < -12) s_tz = -12;
        if (s_tz > 14) s_tz = 14;
        save("set.tz", s_tz);
        break;
    case R_WALLPAPER: go(V_WALLPAPER, true); return;
    case R_DATETIME: go(V_DATETIME, true); return;
    case R_ABOUT: go(V_ABOUT, true); return;
    case R_PASSCODE: go(V_PASSCODE, true); return;
    case R_PIN_ON:
    case R_PIN_OFF:
    case R_PIN_CHANGE:
        pin_start(id);
        return;
    case R_NETWORK:
        ping_result[0] = 0;
        go(V_NETWORK, true);
        return;
    case R_PING: {
        struct net_status st;
        strcpy(ping_result, "Pinging" GFX_CH_ELLIPSIS);
        redraw();
        int ms = net_status(&st) < 0 ? GZ_ENETDOWN : net_ping(st.gateway, 2000);
        if (ms >= 0)
            snprintf(ping_result, sizeof(ping_result), "%d ms", ms);
        else
            strcpy(ping_result, ms == GZ_ETIMEDOUT ? "No reply" : "Failed");
        break;
    }
    case R_PREV_CANCEL:
        go(V_WALLPAPER, false);
        return;
    case R_PREV_SET:
        s_wallpaper = preview_wall;
        save("sb.wallpaper", s_wallpaper);
        go(V_WALLPAPER, false);
        return;
    default:
        if (id >= R_WALL0 && id < R_WALL0 + 3) {
            preview_wall = id - R_WALL0;
            go(V_PREVIEW, true);
            return;
        }
        if (id >= R_FIELD_DOWN && id < R_FIELD_DOWN + 5)
            adjust_field(id - R_FIELD_DOWN, -1);
        else if (id >= R_FIELD_UP && id < R_FIELD_UP + 5)
            adjust_field(id - R_FIELD_UP, 1);
        break;
    }
}

/* Touches on the passcode keypad; false if the event is not for it. */
static bool pin_touch(const struct gz_input_event *ev)
{
    int k = ui_passcode_hit(pin_area(), &pin_pc, ev->x, ev->y);

    if (view != V_PIN || (pin_pc.pressed < 0 && (ev->type != GZ_EV_TOUCH_DOWN || k < 0)))
        return false;
    if (ev->type == GZ_EV_TOUCH_DOWN) {
        pin_pc.pressed = k;
    } else if (ev->type == GZ_EV_TOUCH_MOVE) {
        if (k == pin_pc.pressed)
            return true;
        pin_pc.pressed = -1;
    } else if (ev->type == GZ_EV_TOUCH_UP) {
        int p = pin_pc.pressed;
        pin_pc.pressed = -1;
        if (p == k)
            pin_key(p);
    }
    if (view == V_PIN)
        redraw();
    return true;
}

static int hit_test(int x, int y)
{
    /* Later entries (buttons inside cells) take priority. */
    for (int i = nhits - 1; i >= 0; i--) {
        if (gfx_rect_contains(hits[i].r, x, y))
            return hits[i].id;
    }
    return R_NONE;
}

int main(void)
{
    ui_init(SB_STATUS_GRAY);
    load();
    for (int i = 0; i < 3; i++)
        res_image(&walls[i], wall_names[i]);
    redraw();

    for (;;) {
        struct gz_input_event ev;
        int timeout = view == V_ABOUT || view == V_DATETIME || view == V_NETWORK ? 1000 : -1;
        if (!ui_next_event(&ev, timeout)) {
            redraw();               /* live uptime / clock */
            continue;
        }
        if (pin_touch(&ev))
            continue;
        if (view == V_PIN && ev.type == GZ_EV_TEXT) {
            pin_key(ev.code);
            if (view == V_PIN)
                redraw();
            continue;
        }
        if (ev.type == GZ_EV_TOUCH_DOWN) {
            pressed = hit_test(ev.x, ev.y);
            redraw();
        } else if (ev.type == GZ_EV_TOUCH_MOVE) {
            if (pressed != R_NONE && hit_test(ev.x, ev.y) != pressed) {
                pressed = R_NONE;
                redraw();
            }
        } else if (ev.type == GZ_EV_TOUCH_UP) {
            int id = pressed;
            pressed = R_NONE;
            if (id != R_NONE)
                activate(id);
            redraw();
        } else if (ev.type == GZ_EV_KEY_DOWN && ev.code == GZ_KEY_ESC && view != V_ROOT) {
            activate(R_BACK);
            redraw();
        } else if (ev.type == GZ_EV_KEY_DOWN && ev.code == GZ_KEY_BACKSPACE && view == V_PIN) {
            pin_key('\b');
            redraw();
        }
    }
}
