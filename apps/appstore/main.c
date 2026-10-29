/* Native curated App Store, with the two tabs Search and Updates. */
#include "catalog.h"
#include "worker.h"
#include <glaze/fs.h>
#include <glaze/pkg.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ROW_H 66
#define LIST_Y 63
static const struct ui_tab tabs[] = {{"Search", "tab_search.gzi"}, {"Updates", "tab_updates.gzi"}};
static struct store_catalog catalog;
static struct store_local local[STORE_MAX_APPS];
static struct store_icon icons[STORE_MAX_APPS];
static struct ui_list list;
static struct ui_scroll detail_scroll;
static struct ui_keyboard keyboard;
static int tab, selected = -1, rows[STORE_MAX_APPS], nrows, cursor;
static char query[64], message[96];
static bool editing, armed, all_updates, choose_volume, online, installing;
static gfx_rect back, action, refresh, all_button, flash_button, card_button;
/* UI state is never read by the worker. */
static bool fetch, worker_busy, worker_ok, refresh_pending, scan_pending;
static bool cache_checked[STORE_MAX_APPS];
static unsigned generation;
static int kind, fetch_index, received;
static char target_root[8];
static uint32_t last_poll, last_draw;
static int view_bottom(void)
{
    return editing ? UI_H - ui_keyboard_height() : UI_H - UI_TAB_H - 22;
}
static int list_height(void)
{
    return MAX(0, view_bottom() - LIST_Y);
}
static void redraw(void);
static void start_install(int i, const char *root);
static void rebuild(void)
{
    nrows = 0;
    for (int i = 0; i < catalog.count; i++)
        if (tab ? local[i].update : store_matches(&catalog.apps[i], query))
            rows[nrows++] = i;
    ui_list_set_area(&list, gfx_r(0, LIST_Y, UI_W, list_height()));
    ui_list_reset(&list, nrows);
}
static void update_badge(void)
{
    static int last_badge = -1;
    int updates = 0;
    for (int i = 0; i < catalog.count; i++) updates += local[i].update;
    if (last_badge != updates) {
        ui_set_badge(updates);
        last_badge = updates;
    }
}
static void installed(void)
{
    for (int i = 0; i < catalog.count; i++) {
        local[i].installed = pkg_find(catalog.apps[i].id, local[i].bundle) == 0 &&
                             pkg_bundle_info(local[i].bundle, &local[i].info) == 0;
        local[i].update = local[i].installed && store_version_ok(local[i].info.version) &&
                          store_version_cmp(catalog.apps[i].version, local[i].info.version) > 0;
    }
    update_badge();
}
static void stop_fetch(void)
{
    if (worker_busy && kind == 2 && fetch_index >= 0)
        icons[fetch_index].tried = false;
    if (worker_ok)
        store_worker_cancel();
    worker_busy = fetch = false;
    kind = 0;
}
static void submit(struct store_job *job)
{
    job->generation = generation;
    store_worker_submit(job);
    worker_busy = true;
}
static bool begin_fetch(int what, int index)
{
    stop_fetch();
    if (!worker_ok) {
        strlcpy(message, "Cannot start loading. Reopen the App Store.", sizeof(message));
        return false;
    }
    struct store_job job = {.kind = what == 1 ? STORE_REFRESH : what == 2 ? STORE_ICON : STORE_DOWNLOAD,
                            .index = index};
    if (index >= 0)
        job.app = catalog.apps[index];
    strlcpy(job.root, target_root, sizeof(job.root));
    submit(&job);
    kind = what;
    fetch_index = index;
    fetch = true;
    received = 0;
    return true;
}
/* Keep verified pixels when a catalog changes order or metadata. */
static void accept_catalog(const struct store_catalog *next)
{
    struct store_local saved[STORE_MAX_APPS] = {0};
    int new_selected = -1;
    for (int i = 0; i < next->count; i++) {
        for (int j = 0; j < catalog.count; j++)
            if (!strcmp(next->apps[i].id, catalog.apps[j].id)) {
                saved[i] = local[j];
                if (selected == j) new_selected = i;
                break;
            }
    }
    store_icons_reconcile(&catalog, next, icons);
    memcpy(local, saved, sizeof(local));
    memset(cache_checked, 0, sizeof(cache_checked));
    catalog = *next;
    generation++;
    selected = new_selected;
    if (selected >= 0) {
        int starts[48], lines = ui_wrap(ui_font(FONT_R12), catalog.apps[selected].description, 216, starts, 48);
        detail_scroll.content_h = 180 + lines * ui_font(FONT_R12)->line_height;
        detail_scroll.view_h = view_bottom() - UI_NAV_H;
        detail_scroll.offset = MIN(detail_scroll.offset, ui_scroll_max(&detail_scroll));
    }
    armed = choose_volume = false;
    int offset = list.sc.offset;
    rebuild();
    list.sc.offset = MIN(offset, ui_scroll_max(&list.sc));
    scan_pending = true;
}
static int priorities(int *priority)
{
    int n = 0;
    if (selected >= 0) {
        priority[n++] = selected;
        return n;
    }
    for (int r = 0; r < nrows; r++) {
        int y = ui_list_row_y(&list, r);
        if (y < list.area.y + list.area.h && y + ROW_H > list.area.y)
            priority[n++] = rows[r];
    }
    return n;
}
static void schedule(void)
{
    if (worker_busy || !worker_ok || all_updates) return;
    int priority[STORE_MAX_APPS + 1], n = priorities(priority);
    /* Give the first screen its cached icons before the online refresh. */
    if (refresh_pending) {
        for (int p = 0; p < n; p++) {
            int i = priority[p];
            if (!icons[i].ok && !cache_checked[i]) {
                cache_checked[i] = true;
                struct store_job job = {.kind = STORE_ICON_CACHE, .index = i, .app = catalog.apps[i]};
                submit(&job);
                return;
            }
        }
        refresh_pending = false;
        begin_fetch(1, -1);
        return;
    }
    if (scan_pending) {
        struct store_catalog *snapshot = malloc(sizeof(*snapshot));
        if (snapshot) {
            *snapshot = catalog;
            struct store_job job = {.kind = STORE_SCAN, .catalog = snapshot};
            submit(&job);
        }
        scan_pending = false;
        last_poll = gz_time_ms();
        return;
    }
    bool done[STORE_MAX_APPS];
    for (int i = 0; i < catalog.count; i++) done[i] = icons[i].ok || icons[i].tried;
    int i = store_icon_next(catalog.count, priority, n, done);
    if (i >= 0) {
        icons[i].tried = true;
        begin_fetch(2, i);
    }
}
static void refresh_catalog(void)
{
    all_updates = false;
    armed = false;
    choose_volume = false;
    strlcpy(message, "Connecting to the App Store...", sizeof(message));
    stop_fetch();
    refresh_pending = true;
    for (int i = 0; i < catalog.count; i++) {
        icons[i].tried = false;
        cache_checked[i] = false;
    }
}
static void next_update(void)
{
    if (!all_updates)
        return;
    for (int i = 0; i < catalog.count; i++)
        if (local[i].update) {
            start_install(i, !strncmp(local[i].bundle, "/Card/", 6) ? "/Card" : "");
            return;
        }
    all_updates = false;
    strlcpy(message, "All apps are up to date.", sizeof(message));
}
static void finish_install(struct store_result *r)
{
    int i = r->index;
    struct store_app *a = &catalog.apps[i];
    /* Recheck after download in case a card or install changed. */
    installed();
    bool on_card = !strncmp(local[i].bundle, "/Card/", 6);
    if (local[i].installed && (!store_version_ok(local[i].info.version) ||
                               store_version_cmp(a->version, local[i].info.version) <= 0 ||
                               on_card != (*target_root != 0))) {
        strlcpy(message, "Installation changed. Please try again.", sizeof(message));
        all_updates = false;
    } else {
        strlcpy(message, "Installing...", sizeof(message));
        installing = fetch = true;
        kind = 3;
        redraw();
        char bundle[PKG_BUNDLE_MAX];
        printf("appstore: installing %s %s\n", a->id, a->version);
        uint32_t install_started = gz_time_ms();
        int result = pkg_install(r->package, target_root, bundle);
        printf("appstore: install %s %s result=%d duration=%u ms\n", a->id, a->version, result,
               gz_time_ms() - install_started);
        installing = fetch = false;
        kind = 0;
        if (result) {
            strlcpy(message, fs_strerror(result), sizeof(message));
            all_updates = false;
        } else
            snprintf(message, sizeof(message), "%s is on the Home screen.", a->name);
        installed();
        rebuild();
    }
    fs_remove(r->package);
    scan_pending = false;
    last_poll = gz_time_ms();
}
static bool step_worker(void)
{
    struct store_result r;
    int bytes;
    bool got = store_worker_poll(&r, &bytes);
    bool changed = fetch && kind == 3 && bytes != received && gz_time_ms() - last_draw >= 150;
    received = bytes;
    if (!got) return changed;
    worker_busy = fetch = false;
    kind = 0;
    if (r.generation != generation) {
        store_result_free(&r);
        return false;
    }
    if (r.kind == STORE_BOOT || r.kind == STORE_REFRESH) {
        if (r.catalog) accept_catalog(r.catalog);
        if (r.kind == STORE_REFRESH) {
            online = r.catalog != NULL;
            strlcpy(message, online ? "Browse something new." :
                    (*r.message ? r.message : "Cannot load the store. Please Retry."), sizeof(message));
        }
        changed = true;
    } else if (r.kind == STORE_ICON || r.kind == STORE_ICON_CACHE) {
        if (r.data && r.index >= 0 && r.index < catalog.count) {
            free(icons[r.index].data);
            icons[r.index].data = r.data;
            icons[r.index].img = r.image;
            icons[r.index].ok = true;
            r.data = NULL;
            int priority[STORE_MAX_APPS + 1], n = priorities(priority);
            for (int p = 0; p < n; p++) changed |= priority[p] == r.index;
        }
    } else if (r.kind == STORE_SCAN && r.local) {
        bool different = false;
        for (int i = 0; i < catalog.count; i++) {
            different |= local[i].installed != r.local[i].installed || local[i].update != r.local[i].update;
        }
        memcpy(local, r.local, sizeof(local));
        update_badge();
        if (different) {
            int old_rows[STORE_MAX_APPS], old_n = nrows, offset = list.sc.offset;
            memcpy(old_rows, rows, sizeof(rows));
            /* Update labels without resetting an ongoing drag or fling. */
            nrows = 0;
            for (int i = 0; i < catalog.count; i++)
                if (tab ? local[i].update : store_matches(&catalog.apps[i], query)) rows[nrows++] = i;
            if (old_n != nrows || memcmp(old_rows, rows, nrows * sizeof(*rows))) {
                ui_list_reset(&list, nrows);
                list.sc.offset = MIN(offset, ui_scroll_max(&list.sc));
            }
            changed = true;
        }
    } else if (r.kind == STORE_DOWNLOAD) {
        if (r.error || !*r.package) {
            strlcpy(message, *r.message ? r.message : "Download interrupted. Please Retry.", sizeof(message));
            all_updates = false;
        } else finish_install(&r);
        next_update();
        changed = true;
    }
    store_result_free(&r);
    return changed;
}
static void start_install(int i, const char *root)
{
    strlcpy(target_root, root, sizeof(target_root));
    armed = false;
    choose_volume = false;
    snprintf(message, sizeof(message), "Downloading %s...", catalog.apps[i].name);
    if (!begin_fetch(3, i))
        all_updates = false;
}
static void wrapped(gfx_surface *s, const char *text, int y, int width, const gfx_font *f, uint32_t color)
{
    int starts[48], n = ui_wrap(f, text, width, starts, 48);
    for (int i = 0; i < n; i++)
        gfx_text_n(s, 12, y + i * f->line_height, f, text + starts[i], ui_line_len(text, starts, n, i), color,
                   255);
}
static void draw_row(gfx_surface *s, int r, gfx_rect rc, bool sel, void *ctx)
{
    (void)ctx;
    int i = rows[r], y = rc.y;
    struct store_app *a = &catalog.apps[i];
    if (sel)
        gfx_vgradient(s, gfx_r(0, y, UI_W, ROW_H - 1), UI_SELECT_TOP, UI_SELECT_BOTTOM, 255);
    else
        gfx_fill(s, gfx_r(0, y, UI_W, ROW_H), r % 2 ? 0xF7F8FA : 0xFFFFFF);
    if (icons[i].ok)
        gfx_draw_image(s, 9, y + 10, &icons[i].img, 255);
    else {
        gfx_round_rect_vgradient(s, gfx_r(9, y + 10, 44, 44), 8, 0x7CB2E0, 0x335F9F, 255);
        gfx_text_center(s, 9, y + 20, 44, ui_font(FONT_B20), "+", 0xFFFFFF, 255);
    }
    gfx_text_fit(s, 62, y + 8, 132, ui_font(FONT_B14), a->name, sel ? 0xFFFFFF : UI_TEXT, 255);
    gfx_text_fit(s, 62, y + 27, 154, ui_font(FONT_R10), a->summary, sel ? 0xFFFFFF : 0x526179, 255);
    const char *label = local[i].update ? "Update" : local[i].installed ? "Installed" : "Free";
    gfx_text(s, 62, y + 43, ui_font(FONT_B10), label, sel ? 0xFFFFFF : local[i].update ? 0x277124 : 0x385487, 255);
    ui_draw_chevron(s, 224, y + 27, sel ? 0xFFFFFF : 0x7A879B);
    gfx_fill(s, gfx_r(0, y + ROW_H - 1, UI_W, 1), 0xD3D8E0);
}
static void draw(gfx_surface *s)
{
    gfx_fill(s, gfx_r(0, 0, UI_W, UI_H), 0xFFFFFF);
    if (selected >= 0) {
        struct store_app *a = &catalog.apps[selected];
        ui_draw_navbar(s, a->name, tab ? "Updates" : "Search", &back, 0);
        int offset = detail_scroll.offset;
        gfx_set_clip(s, gfx_r(0, UI_NAV_H, UI_W, view_bottom() - UI_NAV_H));
        gfx_fill(s, gfx_r(0, 30 - offset, 240, 105), 0xF4F6F9);
        if (icons[selected].ok)
            gfx_draw_image(s, 12, 43 - offset, &icons[selected].img, 255);
        gfx_text_fit(s, 66, 43 - offset, 162, ui_font(FONT_B17), a->name, UI_TEXT, 255);
        gfx_text_fit(s, 66, 65 - offset, 162, ui_font(FONT_R11), a->developer, 0x60718A, 255);
        char info[64];
        snprintf(info, sizeof(info), "Version %s  |  %u KB", a->version, (a->size + 1023) / 1024);
        gfx_text(s, 12, 110 - offset, ui_font(FONT_R10), info, 0x60718A, 255);
        action = gfx_r(135, 86 - offset, 93, 21);
        const char *label = local[selected].update      ? "Update"
                            : local[selected].installed ? "Installed"
                            : armed                     ? "Install"
                                                        : "Free";
        gfx_round_rect_vgradient(s, action, 4, armed ? 0x67B451 : 0x648FC8, armed ? 0x2A7D28 : 0x27558F, 255);
        gfx_text_center(s, action.x, action.y + 4, action.w, ui_font(FONT_B11), label, 0xFFFFFF, 255);
        gfx_text(s, 12, 147 - offset, ui_font(FONT_B14), "About this app", 0x253C59, 255);
        wrapped(s, a->description, 171 - offset, 216, ui_font(FONT_R12), 0x303B4B);
        gfx_reset_clip(s);
    } else {
        ui_draw_navbar(s, tab ? "Updates" : "App Store", NULL, NULL, 0);
        refresh = ui_draw_bar_button(s,
                                     fetch    ? "Cancel"
                                     : online ? "Reload"
                                              : "Retry",
                                     true, false, false, 0);
        if (tab) {
            gfx_fill(s, gfx_r(0, 30, 240, 33), 0xE9EDF3);
            gfx_text(s, 10, 41, ui_font(FONT_B11), "Available updates", 0x54657E, 255);
            all_button = gfx_r(153, 35, 79, 23);
            ui_draw_button(s, all_button, "Update All", false);
        } else {
            gfx_vgradient(s, gfx_r(0, 30, 240, 33), 0xCFD6DF, 0xAFBBCB, 255);
            gfx_round_rect(s, gfx_r(7, 35, 226, 23), 11, 0x8C9AAF, 255);
            gfx_round_rect(s, gfx_r(8, 36, 224, 21), 10, 0xFFFFFF, 255);
            if (editing) ui_draw_edit_selection(s, ui_font(FONT_R12), query, cursor, 0, strlen(query), 17, 39, 16);
            gfx_text_fit(s, 17, 40, 196, ui_font(FONT_R12), *query ? query : "Search apps",
                         *query ? UI_TEXT : 0x8793A2, 255);
            if (editing && (gz_time_ms() / 500) % 2 == 0) {
                int x = 17 + gfx_text_width_n(ui_font(FONT_R12), query, cursor);
                gfx_fill(s, gfx_r(MIN(x, 220), 39, 1, 15), 0x2466C0);
            }
        }
        gfx_set_clip(s, list.area);
        ui_list_draw(s, &list, draw_row, NULL);
        if (!nrows) {
            gfx_text_center(s, 4, LIST_Y + 35, 232, ui_font(FONT_B14),
                            tab      ? "All up to date"
                            : *query ? "No results"
                                     : "Welcome to the App Store",
                            0x60718A, 255);
            gfx_text_center(s, 4, LIST_Y + 57, 232, ui_font(FONT_R11),
                            tab      ? "Your installed apps appear here."
                            : *query ? "Try another search."
                                     : "Connect to discover apps for Glaze.",
                            0x718097, 255);
        }
        gfx_reset_clip(s);
    }
    if (editing && ui_keyboard_height())
        ui_draw_keyboard(s, &keyboard, UI_H - ui_keyboard_height());
    if (!editing) {
        gfx_vgradient(s, gfx_r(0, UI_H - UI_TAB_H - 22, 240, 22), 0xECF0F5, 0xDCE3EC, 255);
        gfx_text_fit(s, 7, UI_H - UI_TAB_H - 16, 226, ui_font(FONT_R10), message, 0x4C5F79, 255);
        ui_draw_tabbar(s, tabs, 2, tab);
    }
    if (fetch && kind == 3) {
        gfx_fill_alpha(s, gfx_r(0, 0, UI_W, UI_H), 0x0E213A, 165);
        gfx_round_rect_vgradient(s, gfx_r(18, 88, 204, 131), 10, 0x6C819F, 0x263F64, 255);
        gfx_text_center(s, 25, 101, 190, ui_font(FONT_B14), installing ? "Installing" : "Downloading",
                        0xFFFFFF, 255);
        gfx_text_center(s, 25, 123, 190, ui_font(FONT_R12), catalog.apps[fetch_index].name, 0xFFFFFF, 255);
        gfx_round_rect(s, gfx_r(34, 148, 172, 10), 5, 0x20334E, 255);
        int w = (int)((uint64_t)received * 168 / catalog.apps[fetch_index].size);
        if (w > 0)
            gfx_round_rect_vgradient(s, gfx_r(36, 150, MIN(w, 168), 6), 3, 0x9BDCFC, 0x2D8CF0, 255);
        if (installing)
            gfx_text_center(s, 25, 180, 190, ui_font(FONT_R12), "Please wait...", 0xFFFFFF, 255);
        else
            ui_draw_button(s, gfx_r(66, 174, 108, 29), "Cancel", false);
    }
    if (choose_volume) {
        gfx_fill_alpha(s, gfx_r(0, 0, UI_W, UI_H), 0x0E213A, 165);
        gfx_round_rect_vgradient(s, gfx_r(18, 83, 204, 146), 10, 0x6C819F, 0x263F64, 255);
        gfx_text_center(s, 20, 97, 200, ui_font(FONT_B14), "Install App", 0xFFFFFF, 255);
        flash_button = gfx_r(33, 125, 174, 27);
        card_button = gfx_r(33, 159, 174, 27);
        ui_draw_button(s, flash_button, "Install on Flash", false);
        ui_draw_button(s, card_button, "Install on Card", false);
        gfx_text_center(s, 20, 202, 200, ui_font(FONT_R12), "Cancel", 0xFFFFFF, 255);
    }
}
static void redraw(void)
{
    draw(&ui.surf);
    ui_invalidate_all();
    ui_present();
    last_draw = gz_time_ms();
}
static void show_details(int i)
{
    selected = i;
    editing = false;
    ui_list_set_area(&list, gfx_r(0, LIST_Y, UI_W, list_height()));
    armed = false;
    int starts[48], lines = ui_wrap(ui_font(FONT_R12), catalog.apps[i].description, 216, starts, 48);
    ui_scroll_reset(&detail_scroll, 180 + lines * ui_font(FONT_R12)->line_height, view_bottom() - UI_NAV_H);
    ui_transition(draw, true);
}
static void activate(void)
{
    if (selected < 0)
        return;
    scan_pending = true;
    if (local[selected].update)
        start_install(selected, !strncmp(local[selected].bundle, "/Card/", 6) ? "/Card" : "");
    else if (local[selected].installed)
        strlcpy(message, "This app is on the Home screen.", sizeof(message));
    else if (!armed)
        armed = true;
    else {
        /* Installation decisions use the current card, including a swap
         * since the last background scan. */
        struct fs_stat st;
        if (fs_stat("/Card", &st) == 0)
            choose_volume = true;
        else
            start_install(selected, "");
    }
}
int main(void)
{
    ui_init(SB_STATUS_GRAY);
    ui_keyboard_edit_init(&keyboard, 0);
    ui_list_init(&list, gfx_r(0, LIST_Y, UI_W, list_height()), ROW_H);
    rebuild();
    strlcpy(message, "Connecting to the App Store...", sizeof(message));
    redraw();
    worker_ok = store_worker_start();
    if (worker_ok) {
        struct store_job job = {.kind = STORE_BOOT};
        submit(&job);
        refresh_pending = true;
    } else {
        strlcpy(message, "Cannot start loading. Reopen the App Store.", sizeof(message));
        redraw();
    }
    struct gz_input_event e;
    bool dirty = false;
    for (;;) {
        if (ui_next_event(&e, 20)) {
            if (e.type == GZ_EV_TOUCH_CANCEL) {
                /* Home mid-touch: drop the touch without acting on it. */
                bool changed;
                ui_list_event(&list, &e, &changed);
                ui_scroll_event(&detail_scroll, &e, 0);
                ui_keyboard_event(&keyboard, &e, 0, &changed);
                redraw();
                continue;
            }
            if (fetch && kind == 3) {
                if (e.type == GZ_EV_TOUCH_UP && gfx_rect_contains(gfx_r(66, 174, 108, 29), e.x, e.y)) {
                    all_updates = false;
                    stop_fetch();
                    strlcpy(message, "Download cancelled.", sizeof(message));
                    redraw();
                }
                continue;
            }
            if (choose_volume) {
                if (e.type == GZ_EV_TOUCH_UP) {
                    if (gfx_rect_contains(flash_button, e.x, e.y))
                        start_install(selected, "");
                    else if (gfx_rect_contains(card_button, e.x, e.y))
                        start_install(selected, "/Card");
                    else
                        choose_volume = false;
                    redraw();
                }
                continue;
            }
            if (e.type == GZ_EV_KEY_DOWN && (e.code == GZ_KEY_ESC || e.code == GZ_KEY_ENTER)) {
                if (editing) {
                    editing = false;
                    rebuild();
                } else if (selected >= 0) {
                    selected = -1;
                    armed = false;
                    ui_transition(draw, false);
                }
                redraw();
                continue;
            }
            if (editing) {
                bool changed = false;
                if (ui_edit_input(query, sizeof(query), &cursor, &e, false)) {
                    rebuild();
                    redraw();
                    continue;
                }
                if (ui_keyboard_height() && e.y >= UI_H - ui_keyboard_height() &&
                    (e.type == GZ_EV_TOUCH_DOWN || e.type == GZ_EV_TOUCH_MOVE || e.type == GZ_EV_TOUCH_UP)) {
                    int c = ui_keyboard_event(&keyboard, &e, UI_H - ui_keyboard_height(), &changed);
                    if (c == '\n') {
                        editing = false;
                        rebuild();
                    } else if (c && ui_edit_key(query, sizeof(query), &cursor, c, 0)) {
                        if (ui_edit_modified()) rebuild();
                    }
                    if (changed || c)
                        redraw();
                    continue;
                }
            }
            bool touch = e.type == GZ_EV_TOUCH_DOWN || e.type == GZ_EV_TOUCH_MOVE || e.type == GZ_EV_TOUCH_UP;
            if (!touch)
                continue;
            if (selected >= 0) {
                if (e.y >= UI_NAV_H && e.y < view_bottom())
                    dirty |= ui_scroll_event(&detail_scroll, &e, e.y - UI_NAV_H);
                if (e.type == GZ_EV_TOUCH_UP) {
                    if (gfx_rect_contains(back, e.x, e.y)) {
                        selected = -1;
                        armed = false;
                        ui_transition(draw, false);
                    } else if (!detail_scroll.moved && gfx_rect_contains(action, e.x, e.y))
                        activate();
                }
            } else {
                int r = ui_list_event(&list, &e, &dirty);
                if (r >= 0)
                    show_details(rows[r]);
                else if (e.type == GZ_EV_TOUCH_UP) {
                    if (gfx_rect_contains(refresh, e.x, e.y)) {
                        if (fetch) {
                            stop_fetch();
                            refresh_pending = false;
                            strlcpy(message, "Download cancelled.", sizeof(message));
                        } else
                            refresh_catalog();
                    } else if (!tab && e.y >= 30 && e.y < LIST_Y) {
                        editing = true;
                        cursor = strlen(query);
                        rebuild();
                    } else if (tab && gfx_rect_contains(all_button, e.x, e.y)) {
                        all_updates = true;
                        next_update();
                    }
                }
            }
            if (!editing && e.type == GZ_EV_TOUCH_UP && e.y >= UI_H - UI_TAB_H) {
                int t = ui_tabbar_hit(2, e.x, e.y);
                if (t >= 0) {
                    tab = t;
                    selected = -1;
                    armed = false;
                    scan_pending = true;
                    rebuild();
                }
            }
            redraw();
        }
        if (worker_ok)
            dirty |= step_worker();
        if (gz_time_ms() - last_poll > 2000 && !(fetch && kind == 3))
            scan_pending = true;
        schedule();
        dirty |= selected >= 0 ? ui_scroll_step(&detail_scroll) : ui_scroll_step(&list.sc);
        if (dirty || (editing && gz_time_ms() - last_draw > 500)) {
            redraw();
            dirty = false;
        }
    }
}
