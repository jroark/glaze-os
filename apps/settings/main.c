/* Settings: network, sounds, wallpaper, date & time, passcode lock, about. */

#include <glaze/audio.h>
#include <glaze/net.h>
#include <glaze/notify.h>
#include <glaze/ntp.h>
#include <glaze/passcode.h>
#include <glaze/prefs.h>
#include <glaze/sys.h>
#include <glaze/tz.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <string.h>

enum view { V_ROOT, V_WALLPAPER, V_PREVIEW, V_DATETIME, V_ABOUT, V_NETWORK, V_PASSCODE, V_PIN, V_SOUNDS, V_TONE, V_ZONE };

enum {
    R_NONE, R_WALLPAPER, R_DATETIME, R_ABOUT, R_24H,
    R_NTP, R_ZONE, R_FIELD_DOWN, R_FIELD_UP = R_FIELD_DOWN + 8,
    R_WALL0 = R_FIELD_UP + 8, R_PREV_CANCEL = R_WALL0 + 8, R_PREV_SET, R_BACK,
    R_NETWORK, R_PING, R_PASSCODE, R_PIN_ON, R_PIN_OFF, R_PIN_CHANGE, R_PIN_CANCEL,
    R_SOUNDS, R_VOLUME, R_SND_KEYS, R_SND_TOUCH, R_SND_SWIPE, R_SND_LOCK,
    R_TONE_TIMER, R_TONE_CAL, R_TONE_NOTIFY,
    R_TONE0,                    /* R_TONE0 + n picks tone n; + AUDIO_TONES is None */
    R_ZONE0 = R_TONE0 + 32,     /* R_ZONE0 + n picks time zone n */
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
static int32_t s_wallpaper, s_24h;

/* Date & time (glaze/tz.h). sync_msg, when set, replaces the time of the
 * last NTP sync under the switch. */
static int32_t s_zone = TZ_LOS_ANGELES, s_auto = 1;
static uint32_t s_synced;
static char sync_msg[40];
static struct ui_scroll zone_scroll;
static bool zone_drag;

/* Sounds (glaze/audio.h). The tones are, in order, the timer's, calendar
 * alerts' and other notifications'. */
static int32_t s_volume = AUDIO_VOLUME_DEFAULT, s_keys = 1, s_touch, s_swipe = 1, s_lock = 1;
static int32_t s_tones[3] = { AUDIO_TONE_TIMER_DEFAULT, AUDIO_TONE_CAL_DEFAULT, AUDIO_TONE_NOTIFY_DEFAULT };
static const char *const tone_keys[3] = { AUDIO_PREF_TONE_TIMER, AUDIO_PREF_TONE_CAL, AUDIO_PREF_TONE_NOTIFY };
static const char *const tone_titles[3] = { "Timer", "Calendar Alerts", "Notifications" };
static int tone_for;                    /* which of them V_TONE picks */
static gfx_rect volume_rect;
static bool volume_drag;
static int volume_before;           /* restored if the drag is cancelled */

static void load(void)
{
    pref_get("sb.wallpaper", &s_wallpaper, 4);
    pref_get("sb.24h", &s_24h, 4);
    s_zone = tz_current();
    pref_get(TZ_PREF_AUTO, &s_auto, 4);
    if (pref_get(TZ_PREF_SYNCED, &s_synced, 4) != 4)
        s_synced = 0;
    pref_get(AUDIO_PREF_VOLUME, &s_volume, 4);
    pref_get(AUDIO_PREF_KEYS, &s_keys, 4);
    pref_get(AUDIO_PREF_TOUCH, &s_touch, 4);
    pref_get(AUDIO_PREF_SWIPE, &s_swipe, 4);
    pref_get(AUDIO_PREF_LOCK, &s_lock, 4);
    for (int i = 0; i < 3; i++) {
        pref_get(tone_keys[i], &s_tones[i], 4);
        if (s_tones[i] < 0 || s_tones[i] >= AUDIO_TONES)
            s_tones[i] = AUDIO_TONE_NONE;
    }
}

static void save(const char *key, int32_t v)
{
    pref_put(key, &v, 4);
}

/* Tells SpringBoard the clock or the NTP settings changed. */
static void notify_time(void)
{
    int p = gz_port_lookup(SB_PORT_NAME);

    if (p > 0)
        gz_notify(p, SB_NOTIFY_TIME);
}

/* Saves a sound setting and has the audio server pick it up. */
static void save_sound(const char *key, int32_t v)
{
    save(key, v);
    audio_sound_reload();
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

static bool cell_visible(const gfx_surface *s)
{
    gfx_rect c;

    return gfx_rect_intersect(gfx_r(CELL_X, section_y, CELL_W, UI_ROW_H), s->clip, &c);
}

static void draw_root(gfx_surface *s)
{
    nhits = 0;
    ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H));
    ui_draw_navbar(s, "Settings", NULL, NULL, 0);
    section_y = UI_NAV_H + 12;
    struct net_status st = { .state = NET_NO_CARD };
    if (cell_visible(s) && net_status(&st) < 0)
        st.state = NET_NO_CARD;
    cell(s, 0, "Network", net_state_name(st.state), true, R_NETWORK);
    section_gap();
    cell(s, 1, "Sounds", NULL, true, R_SOUNDS);
    cell(s, 2, "Wallpaper", NULL, true, R_WALLPAPER);
    cell(s, 2, "Date & Time", NULL, true, R_DATETIME);
    /* A clipped-out value needs no file-server round trip. The cell still
     * registers its full hit rectangle below. */
    bool enabled = cell_visible(s) && passcode_enabled();
    cell(s, 3, "Passcode Lock", enabled ? "On" : "Off", true, R_PASSCODE);
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
    /* Keep the caller's clip, including when repainting just a button. */
    gfx_draw_image(s, 0, -SB_STATUS_H, &walls[preview_wall], 255);
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
    char buf[40];

    gz_rtc_get(&dt);
    nhits = 0;
    ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H));
    ui_draw_navbar(s, "Date & Time", "Settings", &back_rect, 0);
    add_hit(back_rect, R_BACK);
    section_y = UI_NAV_H + 10;
    switch_cell(s, 0, "24-Hour Time", s_24h, R_24H);
    section_gap();
    switch_cell(s, 1, "Set Automatically", s_auto, R_NTP);
    cell(s, 3, "Time Zone", tz_get(s_zone)->name, true, R_ZONE);
    if (s_auto) {
        if (sync_msg[0]) {
            strcpy(buf, sync_msg);
        } else if (s_synced) {
            struct gz_datetime st;
            gz_secs_to_dt(s_synced, &st);
            int h = s_24h ? st.hour : (st.hour + 11) % 12 + 1;
            snprintf(buf, sizeof(buf), "Last synced %d/%d at %d:%02d%s", st.month, st.day, h, st.minute,
                     s_24h ? "" : st.hour < 12 ? " AM" : " PM");
        } else {
            strcpy(buf, "Not synced yet");
        }
        gfx_text_center(s, 0, section_y + 8, UI_W, ui_font(FONT_R12), buf, 0x4C566C, 255);
        return;
    }
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

static void draw_sounds(gfx_surface *s)
{
    nhits = 0;
    ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H));
    ui_draw_navbar(s, "Sounds", "Settings", &back_rect, 0);
    add_hit(back_rect, R_BACK);
    section_y = UI_NAV_H + 10;

    /* Alerts and system sounds; music and video play at full level. */
    gfx_rect r = gfx_r(CELL_X, section_y, CELL_W, UI_ROW_H);
    const gfx_font *lf = ui_font(FONT_B13);
    ui_draw_group_cell(s, r, 0, false);
    gfx_text(s, r.x + 10, r.y + (r.h - lf->line_height) / 2, lf, "Volume", UI_TEXT, 255);
    volume_rect = gfx_r(r.x + 70, r.y, r.w - 80, r.h);
    ui_draw_slider(s, volume_rect, s_volume, AUDIO_VOLUME_MAX);
    add_hit(volume_rect, R_VOLUME);
    section_y += UI_ROW_H;
    section_y += 10;

    switch_cell(s, 1, "Keyboard Clicks", s_keys, R_SND_KEYS);
    switch_cell(s, 2, "Touch Sounds", s_touch, R_SND_TOUCH);
    switch_cell(s, 2, "Swipe Sounds", s_swipe, R_SND_SWIPE);
    switch_cell(s, 3, "Lock Sounds", s_lock, R_SND_LOCK);
    section_y += 10;

    for (int i = 0; i < 3; i++)
        cell(s, i + 1, tone_titles[i], audio_tone_name(s_tones[i]), true, R_TONE_TIMER + i);
}

static void draw_tone(gfx_surface *s)
{
    const gfx_font *lf = ui_font(FONT_B13);

    nhits = 0;
    ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H));
    ui_draw_navbar(s, tone_titles[tone_for], "Sounds", &back_rect, 0);
    add_hit(back_rect, R_BACK);
    section_y = UI_NAV_H + 12;
    for (int i = 0; i <= AUDIO_TONES; i++) {
        int tone = i < AUDIO_TONES ? i : AUDIO_TONE_NONE;
        gfx_rect r = gfx_r(CELL_X, section_y, CELL_W, UI_ROW_H);
        bool on = s_tones[tone_for] == tone;
        uint32_t ink = on ? UI_TEXT_DETAIL : UI_TEXT;
        ui_draw_group_cell(s, r, i == 0 ? 1 : i == AUDIO_TONES ? 3 : 2, pressed == R_TONE0 + i);
        if (pressed == R_TONE0 + i)
            ink = 0xFFFFFF;
        gfx_text(s, r.x + 10, r.y + (r.h - lf->line_height) / 2, lf, audio_tone_name(tone), ink, 255);
        if (on)
            gfx_text(s, r.x + r.w - 22, r.y + (r.h - lf->line_height) / 2, lf, GFX_CH_CHECK, ink, 255);
        add_hit(r, R_TONE0 + i);
        section_y += UI_ROW_H;
    }
}

static gfx_rect zone_area(void)
{
    return gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H);
}

static void draw_zone(gfx_surface *s)
{
    const gfx_font *lf = ui_font(FONT_B13);
    gfx_surface c = *s;
    gfx_rect area = zone_area();
    int n = tz_count();

    nhits = 0;
    ui_draw_pinstripes(s, area);
    gfx_rect_intersect(s->clip, area, &c.clip);
    section_y = UI_NAV_H + 12 - zone_scroll.offset;
    for (int i = 0; i < n; i++, section_y += UI_ROW_H) {
        gfx_rect r = gfx_r(CELL_X, section_y, CELL_W, UI_ROW_H), vis;
        if (!gfx_rect_intersect(r, area, &vis))
            continue;
        bool on = s_zone == i, sel = pressed == R_ZONE0 + i;
        uint32_t ink = sel ? 0xFFFFFF : on ? UI_TEXT_DETAIL : UI_TEXT;
        ui_draw_group_cell(&c, r, i == 0 ? 1 : i == n - 1 ? 3 : 2, sel);
        gfx_text(&c, r.x + 10, r.y + (r.h - lf->line_height) / 2, lf, tz_get(i)->name, ink, 255);
        if (on)
            gfx_text(&c, r.x + r.w - 22, r.y + (r.h - lf->line_height) / 2, lf, GFX_CH_CHECK, ink, 255);
        add_hit(vis, R_ZONE0 + i);
    }
    ui_draw_navbar(s, "Time Zone", "Date & Time", &back_rect, 0);
    add_hit(back_rect, R_BACK);
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
    case V_SOUNDS: draw_sounds(s); break;
    case V_TONE: draw_tone(s); break;
    case V_ZONE: draw_zone(s); break;
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
    /* draw_target already built the complete hit table in view coordinates. */
}

static void redraw_rect(gfx_rect r)
{
    gfx_set_clip(&ui.surf, r);
    if (gfx_rect_empty(ui.surf.clip)) {
        gfx_reset_clip(&ui.surf);
        return;
    }
    r = ui.surf.clip;
    /* Every view still registers all its hits, independently of the clip. */
    draw_view(&ui.surf, view);
    gfx_reset_clip(&ui.surf);
    ui_invalidate(r);
    ui_present();
}

static void redraw(void)
{
    redraw_rect(gfx_r(0, 0, UI_W, UI_H));
}

static gfx_rect hit_rect(int id)
{
    gfx_rect r = gfx_r(0, 0, 0, 0);

    if (id != R_NONE) {
        for (int i = 0; i < nhits; i++) {
            if (hits[i].id == id)
                r = gfx_rect_union(r, hits[i].r);
        }
    }
    return r;
}

static void press(int id)
{
    if (id == pressed)
        return;
    gfx_rect r = gfx_rect_union(hit_rect(pressed), hit_rect(id));
    pressed = id;
    redraw_rect(r);
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
    notify_time();
}

/* Sets the clock by NTP now, showing progress under the switch. */
static void sync_now(void)
{
    strcpy(sync_msg, "Setting the time" GFX_CH_ELLIPSIS);
    redraw();
    int rv = ntp_sync();
    if (rv == 0) {
        sync_msg[0] = 0;
        pref_get(TZ_PREF_SYNCED, &s_synced, 4);
    } else {
        snprintf(sync_msg, sizeof(sync_msg), "%s", rv == GZ_ETIMEDOUT ? "No reply from the time server"
                                                                       : net_strerror(rv));
    }
}

/* Moves the clock to a new zone: the RTC keeps local time. */
static void set_zone(int zone)
{
    struct gz_datetime dt;

    if (gz_rtc_get(&dt) == 0) {
        uint32_t utc = tz_local_to_utc(s_zone, gz_dt_to_secs(&dt));
        gz_secs_to_dt(tz_utc_to_local(zone, utc), &dt);
        gz_rtc_set(&dt);
    }
    s_zone = zone;
    save(TZ_PREF_ZONE, s_zone);
    notify_time();
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
        go(view == V_PREVIEW ? V_WALLPAPER : view == V_PIN ? V_PASSCODE : view == V_TONE ? V_SOUNDS
           : view == V_ZONE ? V_DATETIME : V_ROOT, false);
        return;
    }
    switch (id) {
    case R_24H:
        s_24h = !s_24h;
        save("sb.24h", s_24h);
        break;
    case R_NTP:
        s_auto = !s_auto;
        save(TZ_PREF_AUTO, s_auto);
        sync_msg[0] = 0;
        if (s_auto)
            sync_now();
        notify_time();
        break;
    case R_ZONE:
        ui_scroll_reset(&zone_scroll, tz_count() * UI_ROW_H + 24, UI_H - UI_NAV_H);
        zone_scroll.offset = MAX(0, MIN(s_zone * UI_ROW_H - 3 * UI_ROW_H, ui_scroll_max(&zone_scroll)));
        go(V_ZONE, true);
        return;
    case R_SOUNDS: go(V_SOUNDS, true); return;
    case R_SND_KEYS:
        s_keys = !s_keys;
        save_sound(AUDIO_PREF_KEYS, s_keys);
        break;
    case R_SND_TOUCH:
        s_touch = !s_touch;
        save_sound(AUDIO_PREF_TOUCH, s_touch);
        break;
    case R_SND_SWIPE:
        s_swipe = !s_swipe;
        save_sound(AUDIO_PREF_SWIPE, s_swipe);
        break;
    case R_SND_LOCK:
        s_lock = !s_lock;
        save_sound(AUDIO_PREF_LOCK, s_lock);
        break;
    case R_TONE_TIMER:
    case R_TONE_CAL:
    case R_TONE_NOTIFY:
        tone_for = id - R_TONE_TIMER;
        go(V_TONE, true);
        return;
    case R_WALLPAPER: go(V_WALLPAPER, true); return;
    case R_DATETIME:
        sync_msg[0] = 0;
        if (pref_get(TZ_PREF_SYNCED, &s_synced, 4) != 4)
            s_synced = 0;
        go(V_DATETIME, true);
        return;
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
        if (id >= R_TONE0 && id <= R_TONE0 + AUDIO_TONES) {
            int tone = id - R_TONE0 < AUDIO_TONES ? id - R_TONE0 : AUDIO_TONE_NONE;
            s_tones[tone_for] = tone;
            save_sound(tone_keys[tone_for], tone);
            if (tone != AUDIO_TONE_NONE)
                audio_sound(AUDIO_SND_TONE + tone);
            return;
        }
        if (id >= R_ZONE0 && id < R_ZONE0 + tz_count()) {
            if (id - R_ZONE0 != s_zone)
                set_zone(id - R_ZONE0);
            go(V_DATETIME, false);
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

/* Dragging the volume slider; false if the event is not for it. Lifting
 * off saves the level and plays the notification tone at it. */
static bool volume_touch(const struct gz_input_event *ev)
{
    if (view != V_SOUNDS)
        return false;
    if (ev->type == GZ_EV_TOUCH_DOWN) {
        volume_drag = gfx_rect_contains(volume_rect, ev->x, ev->y);
        volume_before = s_volume;
    }
    if (!volume_drag)
        return false;
    int v = ui_slider_value(volume_rect, AUDIO_VOLUME_MAX, ev->x);
    if (ev->type == GZ_EV_TOUCH_UP) {
        volume_drag = false;
        s_volume = v;
        save_sound(AUDIO_PREF_VOLUME, s_volume);
        int tone = s_tones[2] != AUDIO_TONE_NONE ? s_tones[2] : 0;
        audio_sound(AUDIO_SND_TONE + tone);
    } else if (v == s_volume) {
        return true;
    }
    s_volume = v;
    redraw_rect(volume_rect);
    return true;
}

static int hit_test(int x, int y);

/* Scrolling and tapping the time zone list; false if the event is not
 * for it (the nav bar's back button). */
static bool zone_touch(const struct gz_input_event *ev)
{
    if (view != V_ZONE || (ev->type != GZ_EV_TOUCH_DOWN && ev->type != GZ_EV_TOUCH_MOVE &&
                           ev->type != GZ_EV_TOUCH_UP))
        return false;
    if (ev->type == GZ_EV_TOUCH_DOWN)
        zone_drag = ev->y >= UI_NAV_H;
    if (!zone_drag)
        return false;
    int old_offset = zone_scroll.offset, old_pressed = pressed;
    gfx_rect old_hit = hit_rect(pressed);
    ui_scroll_event(&zone_scroll, ev, ev->y - UI_NAV_H);
    if (ev->type == GZ_EV_TOUCH_DOWN)
        pressed = hit_test(ev->x, ev->y);
    else if (zone_scroll.moved)
        pressed = R_NONE;
    if (ev->type == GZ_EV_TOUCH_UP) {
        int id = pressed;
        zone_drag = false;
        pressed = R_NONE;
        if (id != R_NONE && hit_test(ev->x, ev->y) == id) {
            activate(id);
            if (view != V_ZONE)
                return true;
        }
    }
    if (zone_scroll.offset != old_offset)
        redraw_rect(zone_area());
    else if (pressed != old_pressed)
        redraw_rect(gfx_rect_union(old_hit, hit_rect(pressed)));
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
        if (view == V_ZONE && zone_scroll.animating)
            timeout = 16;
        if (!ui_next_event(&ev, timeout)) {
            if (view == V_ZONE) {
                int old_offset = zone_scroll.offset;
                ui_scroll_step(&zone_scroll);
                if (zone_scroll.offset != old_offset)
                    redraw_rect(zone_area());
            } else {
                redraw_rect(zone_area());  /* live uptime / clock / network */
            }
            continue;
        }
        if (ev.type == GZ_EV_TOUCH_CANCEL) {
            /* Home mid-touch: drop the touch without acting on it. */
            pin_pc.pressed = -1;
            if (volume_drag)
                s_volume = volume_before;
            volume_drag = false;
            if (zone_drag)
                ui_scroll_event(&zone_scroll, &ev, ev.y - UI_NAV_H);
            zone_drag = false;
            pressed = R_NONE;
            redraw();
            continue;
        }
        if (pin_touch(&ev) || volume_touch(&ev) || zone_touch(&ev))
            continue;
        if (view == V_PIN && ev.type == GZ_EV_TEXT) {
            pin_key(ev.code);
            if (view == V_PIN)
                redraw();
            continue;
        }
        if (ev.type == GZ_EV_TOUCH_DOWN) {
            press(hit_test(ev.x, ev.y));
        } else if (ev.type == GZ_EV_TOUCH_MOVE) {
            if (pressed != R_NONE && hit_test(ev.x, ev.y) != pressed) {
                press(R_NONE);
            }
        } else if (ev.type == GZ_EV_TOUCH_UP) {
            int id = pressed;
            if (id != R_NONE) {
                enum view old_view = view;
                pressed = R_NONE;
                activate(id);
                if (view == old_view)
                    redraw();      /* activation may change layout or values */
            }
        } else if (ev.type == GZ_EV_KEY_DOWN && ev.code == GZ_KEY_ESC && view != V_ROOT) {
            activate(R_BACK);
        } else if (ev.type == GZ_EV_KEY_DOWN && ev.code == GZ_KEY_BACKSPACE && view == V_PIN) {
            pin_key('\b');
            redraw();
        }
    }
}
