/* SomaFM personal radio player. The OS owns playback while this UI sleeps. */
#include "client.h"
#include "stations.h"
#include <glaze/fs.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <string.h>

enum { V_LIST, V_NOW, V_ABOUT };
static int view, selected, cursor, rows[STATION_COUNT], nrows, armed = -1;
static bool favorites[STATION_COUNT], favorites_only, compatible = true;
static struct ui_list list;
static struct radio_state state;
static gfx_rect back, info, filter_all, filter_favs, now, play, favorite;
static char notice[96];

static int station_index(const char *id)
{
    for (int i = 0; i < STATION_COUNT; i++) if (!strcmp(stations[i].id, id)) return i;
    return -1;
}
static bool active(void) { return state.phase != RADIO_STOPPED && state.phase != RADIO_ERROR; }
static bool current(void) { return !strcmp(state.id, stations[selected].id); }
static void path(char *out, size_t n) { snprintf(out, n, "%s/data/radio", res_bundle()); }
static void load(void)
{
    char name[FS_PATH_MAX], data[4096];
    path(name, sizeof(name));
    int n = fs_read_file(name, data, sizeof(data)-1);
    if (n <= 0) return;
    data[n] = 0;
    for (char *s = data; s && *s;) {
        char *end = strchr(s, '\n');
        if (end) *end++ = 0;
        char *value = strchr(s, '=');
        if (value) {
            *value++ = 0;
            int i = station_index(value);
            if (i >= 0 && !strcmp(s, "last")) selected = i;
            if (i >= 0 && !strcmp(s, "favorite")) favorites[i] = true;
        }
        s = end;
    }
}
static void save(void)
{
    char name[FS_PATH_MAX], dir[FS_PATH_MAX], data[4096];
    int n = snprintf(data, sizeof(data), "last=%s\n", stations[selected].id);
    for (int i = 0; i < STATION_COUNT; i++) if (favorites[i])
        n += snprintf(data+n, sizeof(data)-(size_t)n, "favorite=%s\n", stations[i].id);
    path(name, sizeof(name));
    snprintf(dir, sizeof(dir), "%s/data", res_bundle());
    fs_mkdirs(dir);
    if (fs_write_file(name, data, (uint32_t)n) != n)
        snprintf(notice, sizeof(notice), "Could not save favorites.");
}
static void rebuild(void)
{
    nrows = 0; cursor = 0;
    for (int i = 0; i < STATION_COUNT; i++) if (!favorites_only || favorites[i]) {
        if (i == selected) cursor = nrows;
        rows[nrows++] = i;
    }
    ui_list_reset(&list, nrows);
    if (nrows && (cursor+1)*list.row_h > list.area.h)
        list.sc.offset = MIN((cursor+1)*list.row_h-list.area.h, ui_scroll_max(&list.sc));
}
static void poll_state(void)
{
    int rv = somafm_state(&state);
    if (rv == GZ_EINVAL) {
        compatible = false;
        snprintf(notice, sizeof(notice), "Requires an updated Glaze OS.");
    } else if (rv < 0) snprintf(notice, sizeof(notice), "Audio service unavailable.");
}
static const char *phase_text(void)
{
    if (!current() || state.phase == RADIO_STOPPED) return "Ready to listen";
    switch (state.phase) {
    case RADIO_CONNECTING: return "Connecting...";
    case RADIO_BUFFERING: return "Buffering...";
    case RADIO_PLAYING: return "Live from SomaFM";
    case RADIO_RECONNECTING: return "Reconnecting...";
    case RADIO_ERROR:
        switch (state.error) {
        case RADIO_E_NETWORK: return "Connect an Ethernet card.";
        case RADIO_E_CERT: return "Secure connection failed.";
        case RADIO_E_DATE: return "Check the date in Settings.";
        case RADIO_E_PLAYLIST: return "Station playlist unavailable.";
        case RADIO_E_FORMAT: return "Unsupported audio stream.";
        case RADIO_E_MEMORY: return "Not enough memory to play.";
        case RADIO_E_TIMEOUT: return "Connection timed out. Retry.";
        default: return "Connection lost. Tap Retry.";
        }
    default: return "Stopped";
    }
}
static void toggle(void)
{
    if (!compatible) return;
    notice[0] = 0;
    int rv;
    if (current() && active()) rv = somafm_stop();
    else {
        struct radio_request req = {0};
        snprintf(req.id, sizeof(req.id), "%s", stations[selected].id);
        snprintf(req.name, sizeof(req.name), "%s", stations[selected].name);
        snprintf(req.playlist, sizeof(req.playlist), "%s", stations[selected].playlist);
        rv = somafm_play(&req);
    }
    if (rv < 0) snprintf(notice, sizeof(notice), rv == GZ_ENOMEM ? "Not enough memory to play." : "Could not start the station.");
    poll_state();
}
static int wrapped(gfx_surface *s, const char *text, int y, int font, int max_lines, uint32_t color)
{
    int starts[16];
    const gfx_font *f = ui_font(font);
    int n = ui_wrap(f, text, UI_W-24, starts, 16);
    for (int i = 0; i < n && i < max_lines; i++) {
        char line[256];
        int len = ui_line_len(text, starts, n, i);
        if (len >= (int)sizeof(line)) len = sizeof(line)-1;
        memcpy(line, text + starts[i], (size_t)len); line[len] = 0;
        gfx_text_center(s, 12, y, UI_W-24, f, line, color, 255);
        y += f->line_height + 2;
    }
    return y;
}
static void draw_row(gfx_surface *s, int row, gfx_rect r, bool pressed, void *ctx)
{
    (void)ctx;
    int i = rows[row];
    bool sel = pressed || row == cursor;
    ui_draw_list_row(s, r, sel);
    gfx_text_fit(s, 10, r.y+6, UI_W-28, ui_font(FONT_B14), stations[i].name, sel ? 0xffffff : UI_TEXT, 255);
    gfx_text_fit(s, 10, r.y+27, UI_W-28, ui_font(FONT_R11), stations[i].genre, sel ? 0xffffff : UI_TEXT_DETAIL, 255);
    if (favorites[i]) gfx_text(s, UI_W-16, r.y+7, ui_font(FONT_B14), "*", sel ? 0xffffff : UI_BLUE_BUTTON, 255);
}
static void draw(void)
{
    gfx_surface *s = &ui.surf;
    gfx_set_clip(s, gfx_r(0, 0, UI_W, UI_H));
    gfx_fill(s, gfx_r(0, 0, UI_W, UI_H), 0xffffff);
    ui_draw_navbar(s, view == V_ABOUT ? "About" : view == V_NOW ? "Now Playing" : "SomaFM",
                   view ? "Stations" : NULL, &back, 0);
    if (view == V_LIST) info = ui_draw_bar_button(s, "Info", true, armed == 0, false, 0);
    if (view == V_LIST) {
        filter_all = gfx_r(8, UI_NAV_H+5, 108, 27);
        filter_favs = gfx_r(124, UI_NAV_H+5, 108, 27);
        ui_draw_button(s, filter_all, "All Stations", !favorites_only);
        ui_draw_button(s, filter_favs, "Favorites", favorites_only);
        gfx_set_clip(s, list.area);
        ui_list_draw(s, &list, draw_row, NULL);
        if (!nrows) {
            wrapped(s, "No favorites yet.", 125, FONT_B17, 1, UI_TEXT_DETAIL);
            wrapped(s, "Choose a station and tap Add Favorite.", 152, FONT_R12, 2, UI_TEXT_DETAIL);
        }
        gfx_set_clip(s, gfx_r(0, 0, UI_W, UI_H));
        now = gfx_r(0, UI_H-42, UI_W, 42);
        gfx_fill(s, now, 0xe5eaf0);
        gfx_text_fit(s, 10, now.y+5, UI_W-20, ui_font(FONT_B14), active() ? state.name : stations[selected].name, UI_TEXT, 255);
        gfx_text_fit(s, 10, now.y+24, UI_W-20, ui_font(FONT_R11), notice[0] ? notice : active() ? "Now Playing  >" : "Tap a station to listen", UI_TEXT_DETAIL, 255);
    } else if (view == V_NOW) {
        ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H-UI_NAV_H));
        int y = wrapped(s, stations[selected].name, 45, FONT_B20, 2, UI_TEXT);
        wrapped(s, stations[selected].genre, y+3, FONT_R12, 2, UI_TEXT_DETAIL);
        const char *track = current() && state.title[0] ? state.title : "Commercial-free internet radio";
        wrapped(s, track, 118, FONT_B14, 3, UI_TEXT);
        wrapped(s, notice[0] ? notice : phase_text(), 185, FONT_R12, 2, UI_TEXT_DETAIL);
        if (current() && state.bitrate && active()) {
            char text[32]; snprintf(text, sizeof(text), "MP3  %u kbps", state.bitrate);
            wrapped(s, text, 214, FONT_R11, 1, UI_TEXT_DETAIL);
        }
        play = gfx_r(35, UI_H-69, UI_W-70, 31);
        ui_draw_button(s, play, current() && active() ? "Stop" : current() && state.phase == RADIO_ERROR ? "Retry" : "Play", armed == 3);
        favorite = gfx_r(35, UI_H-33, UI_W-70, 27);
        ui_draw_button(s, favorite, favorites[selected] ? "Remove Favorite" : "Add Favorite", armed == 4);
    } else {
        wrapped(s, "SomaFM for Glaze", 55, FONT_B20, 2, UI_TEXT);
        wrapped(s, "An unofficial player for personal listening. Music is provided by SomaFM.", 103, FONT_R14, 4, UI_TEXT_DETAIL);
        wrapped(s, "SomaFM is listener supported.", 184, FONT_B14, 2, UI_TEXT);
        wrapped(s, "somafm.com", 228, FONT_R14, 1, UI_BLUE_BUTTON);
        wrapped(s, "somafm.com/support", 251, FONT_R14, 1, UI_BLUE_BUTTON);
    }
    ui_invalidate_all(); ui_present();
}
static void choose(int row)
{
    if (row < 0 || row >= nrows) return;
    selected = rows[row]; cursor = row; view = V_NOW; notice[0] = 0; save();
    if (!current() || !active()) toggle();
}
static int hit(int x, int y)
{
    if (view && gfx_rect_contains(back, x, y)) return 1;
    if (view == V_LIST) {
        if (gfx_rect_contains(info, x, y)) return 0;
        if (gfx_rect_contains(filter_all, x, y)) return 5;
        if (gfx_rect_contains(filter_favs, x, y)) return 6;
        if (gfx_rect_contains(now, x, y)) return 2;
    } else if (view == V_NOW) {
        if (gfx_rect_contains(play, x, y)) return 3;
        if (gfx_rect_contains(favorite, x, y)) return 4;
    }
    return -1;
}
static void action(int button)
{
    switch (button) {
    case 0: view = V_ABOUT; break;
    case 1: view = V_LIST; rebuild(); break;
    case 2: {
        int i = active() ? station_index(state.id) : -1;
        if (i >= 0) selected = i;
        view = V_NOW; break;
    }
    case 3: toggle(); break;
    case 4: favorites[selected] = !favorites[selected]; save(); break;
    case 5: case 6: favorites_only = button == 6; rebuild(); break;
    }
}
static void key(const struct gz_input_event *e)
{
    if (e->code == GZ_KEY_ESC) { view = V_LIST; rebuild(); return; }
    if (view == V_LIST) {
        if (e->code == GZ_KEY_OK) choose(cursor);
        if (e->code == GZ_KEY_LEFT || e->code == GZ_KEY_RIGHT) {
            favorites_only = !favorites_only; rebuild();
        }
        if (nrows && (e->code == GZ_KEY_UP || e->code == GZ_KEY_DOWN)) {
            cursor = (cursor + (e->code == GZ_KEY_UP ? nrows-1 : 1)) % nrows;
            int top = cursor * list.row_h, bottom = top + list.row_h;
            if (top < list.sc.offset) list.sc.offset = top;
            if (bottom > list.sc.offset + list.area.h) list.sc.offset = bottom-list.area.h;
        }
    } else if (view == V_NOW && e->code == GZ_KEY_OK) toggle();
}
static bool input(const struct gz_input_event *e)
{
    bool dirty = false;
    if (e->type == GZ_EV_TOUCH_CANCEL || e->type == GZ_EV_APP_DEACTIVATE) {
        armed = -1;
        struct gz_input_event cancel = {.type = GZ_EV_TOUCH_CANCEL};
        ui_list_event(&list, &cancel, &dirty);
        return true;
    }
    if (e->type == GZ_EV_KEY_DOWN) { key(e); return true; }
    if (view == V_LIST) {
        int row = ui_list_event(&list, e, &dirty);
        if (row >= 0) { choose(row); dirty = true; }
    }
    if (e->type == GZ_EV_TOUCH_DOWN) { armed = hit(e->x, e->y); dirty = true; }
    else if (e->type == GZ_EV_TOUCH_UP) {
        int button = armed;
        armed = -1;
        if (button >= 0 && button == hit(e->x, e->y)) action(button);
        dirty = true;
    }
    return dirty;
}
int main(void)
{
    ui_init(SB_STATUS_GRAY);
    selected = station_index("groovesalad");
    load(); poll_state();
    int i = active() ? station_index(state.id) : -1;
    if (i >= 0) { selected = i; view = V_NOW; }
    ui_list_init(&list, gfx_r(0, UI_NAV_H+38, UI_W, UI_H-UI_NAV_H-80), 50);
    rebuild(); draw();
    for (;;) {
        struct gz_input_event e;
        int got = ui_next_event(&e, list.sc.animating ? 16 : 250);
        bool dirty = false;
        if (got) {
            if (e.type == GZ_EV_APP_ACTIVATE) {
                poll_state(); dirty = true;
                char text[192];
                struct gz_sysinfo si = {0};
                gz_sysinfo(&si);
                snprintf(text, sizeof(text), "somafm: resume phase=%u station=%s bitrate=%u buffered=%u error=%u free=%u KiB\n",
                    state.phase, state.id, state.bitrate, state.buffered_bytes, state.error, si.free_pages*4);
                gz_log(text, (uint32_t)strlen(text));
            }
            else dirty = input(&e);
        }
        if (ui_in_background()) continue;
        if (view == V_LIST && ui_scroll_step(&list.sc)) dirty = true;
        struct radio_state old = state;
        poll_state();
        if (memcmp(&old, &state, sizeof(state))) dirty = true;
        if (dirty) draw();
    }
}
