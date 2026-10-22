/* Native curated App Store, with the two tabs Search and Updates. */
#include "catalog.h"
#include "net.h"
#include "web.h"
#include <glaze/fs.h>
#include <glaze/pkg.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/sha256.h>
#ifndef APPSTORE_URL
#define APPSTORE_URL "https://appstore.glaze-os.com"
#endif
#define CACHE "/Library/Caches/AppStore"
#define ROW_H 66
#define LIST_Y 63
static const struct ui_tab tabs[] = {{"Search", "tab_search.gzi"}, {"Updates", "tab_updates.gzi"}};
static struct store_catalog catalog, incoming;
static struct {
    char bundle[PKG_BUNDLE_MAX];
    struct pkg_info info;
    bool installed, update;
} local[STORE_MAX_APPS];
static struct {
    void *data;
    gfx_image img;
    bool ok, tried;
} icons[STORE_MAX_APPS];
static struct ui_list list;
static struct ui_scroll detail_scroll;
static struct ui_keyboard keyboard;
static int tab, selected = -1, rows[STORE_MAX_APPS], nrows, cursor;
static char query[64], message[96];
static bool editing, armed, all_updates, choose_volume, online, installing;
static gfx_rect back, action, refresh, all_button, flash_button, card_button;
static struct http *fetch;
static int kind, fetch_index, download_fd = -1, download_error, received;
static char temporary[FS_PATH_MAX], target_root[8];
static wc_Sha256 hash;
static bool hash_live;
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
static bool installed(void)
{
    int updates = 0;
    bool changed = false;
    static int last_badge = -1;
    for (int i = 0; i < catalog.count; i++) {
        bool was_installed = local[i].installed, was_update = local[i].update;
        local[i].installed = pkg_find(catalog.apps[i].id, local[i].bundle) == 0 &&
                             pkg_bundle_info(local[i].bundle, &local[i].info) == 0;
        local[i].update = local[i].installed && store_version_ok(local[i].info.version) &&
                          store_version_cmp(catalog.apps[i].version, local[i].info.version) > 0;
        updates += local[i].update;
        changed |= was_installed != local[i].installed || was_update != local[i].update;
    }
    if (last_badge != updates) {
        ui_set_badge(updates);
        last_badge = updates;
    }
    return changed;
}
static bool cached_catalog(const char *path)
{
    struct fs_stat st;
    if (fs_stat(path, &st) < 0 || !st.size || st.size > STORE_CATALOG_MAX)
        return false;
    char *data = malloc(st.size);
    if (!data)
        return false;
    int n = fs_read_file(path, data, st.size);
    bool ok = n == (int)st.size && store_parse(&incoming, data, n);
    free(data);
    if (!ok)
        return false;
    for (int i = 0; i < STORE_MAX_APPS; i++) {
        free(icons[i].data);
        memset(&icons[i], 0, sizeof(icons[i]));
    }
    catalog = incoming;
    installed();
    rebuild();
    return true;
}
static void icon_path(int i, char *path, size_t n)
{
    snprintf(path, n, CACHE "/%s-%.16s.gzi", catalog.apps[i].id, catalog.apps[i].icon_sha256);
}
/* Keep the disk cache bounded when publishers replace icons over time. */
static void prune_cache(void)
{
    struct fs_dirent entry;
    uint32_t skip = 0;
    while (fs_list(CACHE, skip, &entry, 1) == 1) {
        size_t len = strlen(entry.name);
        bool keep = true;
        if (entry.type == FS_FILE && len > 4 && !strcmp(entry.name + len - 4, ".gzi")) {
            keep = false;
            for (int i = 0; i < catalog.count; i++) {
                char path[FS_PATH_MAX];
                icon_path(i, path, sizeof(path));
                if (!strcmp(entry.name, strrchr(path, '/') + 1)) {
                    keep = true;
                    break;
                }
            }
        }
        if (!keep) {
            char path[FS_PATH_MAX];
            snprintf(path, sizeof(path), CACHE "/%s", entry.name);
            if (fs_remove(path) < 0)
                skip++;
        } else
            skip++;
    }
}
static bool digest_matches(const void *data, int n, const char *expected)
{
    uint8_t sum[32];
    char text[65];
    static const char hex[] = "0123456789abcdef";
    if (wc_Sha256Hash(data, n, sum))
        return false;
    for (int i = 0; i < 32; i++) {
        text[i * 2] = hex[sum[i] >> 4];
        text[i * 2 + 1] = hex[sum[i] & 15];
    }
    text[64] = 0;
    return !strcmp(text, expected);
}
static bool load_icon(int i)
{
    char path[FS_PATH_MAX];
    struct fs_stat st;
    icon_path(i, path, sizeof(path));
    if (fs_stat(path, &st) < 0 || st.size > STORE_ICON_MAX)
        return false;
    void *data = malloc(st.size);
    if (!data)
        return false;
    int n = fs_read_file(path, data, st.size);
    gfx_image img;
    if (n != (int)st.size || !digest_matches(data, n, catalog.apps[i].icon_sha256) ||
        !gfx_image_parse(&img, data, n) || img.w != 44 || img.h != 44) {
        free(data);
        return false;
    }
    free(icons[i].data);
    icons[i].data = data;
    icons[i].img = img;
    icons[i].ok = true;
    return true;
}
static const char *network_error(int e)
{
    switch (e) {
    case WE_NETDOWN:
        return "Connect an Ethernet card, then Retry.";
    case WE_DNS:
        return "Store not found. Check your connection.";
    case WE_TIMEOUT:
        return "Connection timed out. Please Retry.";
    case WE_CERT_DATE:
        return "Check the date in Settings, then Retry.";
    case WE_CERT:
    case WE_CERT_NAME:
    case WE_TLS:
        return "Cannot verify the secure store connection.";
    case WE_TOO_BIG:
        return "This download is too large.";
    default:
        return "Download interrupted. Please Retry.";
    }
}
static void stop_fetch(void)
{
    http_free(fetch);
    fetch = NULL;
    if (download_fd >= 0) {
        fs_close(download_fd);
        download_fd = -1;
    }
    if (hash_live) {
        wc_Sha256Free(&hash);
        hash_live = false;
    }
    if (*temporary)
        fs_remove(temporary);
    temporary[0] = 0;
    kind = 0;
}
static int write_bytes(const uint8_t *data, int n, void *arg)
{
    (void)arg;
    if (fs_write(download_fd, data, n) != n) {
        download_error = GZ_ENOSPC;
        return WE_TOO_BIG;
    }
    if (wc_Sha256Update(&hash, data, n)) {
        download_error = GZ_EIO;
        return WE_PROTOCOL;
    }
    received += n;
    return 0;
}
static bool begin_fetch(int what, int index, const char *asset)
{
    stop_fetch();
    kind = what;
    fetch_index = index;
    received = 0;
    download_error = 0;
    if (!store_path_ok(asset)) {
        kind = 0;
        return false;
    }
    if (what == 3)
        snprintf(temporary, sizeof(temporary), "%s/Applications/.store-download", target_root);
    else
        snprintf(temporary, sizeof(temporary), CACHE "/download.tmp");
    download_fd = fs_open(temporary, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    if (download_fd < 0) {
        snprintf(message, sizeof(message), "%s", fs_strerror(download_fd));
        stop_fetch();
        return false;
    }
    if (wc_InitSha256(&hash)) {
        stop_fetch();
        return false;
    }
    hash_live = true;
    char url[320];
    snprintf(url, sizeof(url), "%s/%s", APPSTORE_URL, asset);
    int max = what == 1 ? STORE_CATALOG_MAX : what == 2 ? STORE_ICON_MAX : (int)catalog.apps[index].size;
    fetch = http_start_stream(url, max, write_bytes, NULL);
    if (!fetch) {
        stop_fetch();
        strlcpy(message, "Cannot start download. Please Retry.", sizeof(message));
        return false;
    }
    return true;
}
static void refresh_catalog(void)
{
    all_updates = false;
    armed = false;
    choose_volume = false;
    strlcpy(message, "Connecting to the App Store...", sizeof(message));
    begin_fetch(1, -1, "catalog.txt");
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
static void step_fetch(void)
{
    if (!fetch)
        return;
    int rv = http_step(fetch);
    if (!rv)
        return;
    int what = kind, i = fetch_index, status = http_status(fetch);
    printf("appstore: fetch kind=%d result=%d HTTP=%d bytes=%d\n", what, rv, status, received);
    if (rv < 0 || status != 200) {
        if (what == 1)
            online = false;
        if (what != 2) {
            if (download_error)
                strlcpy(message, fs_strerror(download_error), sizeof(message));
            else if (rv < 0)
                strlcpy(message, network_error(rv), sizeof(message));
            else
                snprintf(message, sizeof(message), "Store returned HTTP %d. Please Retry.", status);
            all_updates = false;
        }
        stop_fetch();
        redraw();
        return;
    }
    int closed = fs_close(download_fd);
    download_fd = -1;
    uint8_t sum[32] = {0};
    char hex[65];
    static const char digits[] = "0123456789abcdef";
    if (wc_Sha256Final(&hash, sum))
        rv = WE_PROTOCOL;
    wc_Sha256Free(&hash);
    hash_live = false;
    for (int j = 0; j < 32; j++) {
        hex[2 * j] = digits[sum[j] >> 4];
        hex[2 * j + 1] = digits[sum[j] & 15];
    }
    hex[64] = 0;
    if (rv < 0 || closed < 0) {
        printf("appstore: finish error result=%d close=%d\n", rv, closed);
        strlcpy(message, "Could not save the download. Please Retry.", sizeof(message));
        all_updates = false;
        stop_fetch();
        redraw();
        return;
    }
    if (what == 1) {
        if (cached_catalog(temporary)) {
            online = true;
            fs_rename(temporary, CACHE "/catalog.txt");
            selected = -1;
            prune_cache();
            strlcpy(message, "Browse something new.", sizeof(message));
            for (int j = 0; j < catalog.count; j++)
                load_icon(j);
        } else {
            online = false;
            strlcpy(message, "Invalid store catalog. Please Retry.", sizeof(message));
        }
    } else if (what == 2) {
        if (!strcmp(hex, catalog.apps[i].icon_sha256)) {
            char path[FS_PATH_MAX];
            icon_path(i, path, sizeof(path));
            if (!fs_rename(temporary, path))
                load_icon(i);
        }
    } else {
        struct store_app *a = &catalog.apps[i];
        struct pkg_info info;
        int info_result = pkg_read_info(temporary, &info);
        if (received != (int)a->size || strcmp(hex, a->sha256) || info_result || strcmp(info.id, a->id) ||
            strcmp(info.version, a->version)) {
            printf("appstore: verification failed size=%d/%u hash=%d info=%d\n", received, a->size,
                   strcmp(hex, a->sha256), info_result);
            strlcpy(message, "Package verification failed. Please Retry.", sizeof(message));
            all_updates = false;
        } else {
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
                installing = true;
                redraw();
                char bundle[PKG_BUNDLE_MAX];
                printf("appstore: installing %s %s\n", a->id, a->version);
                uint32_t install_started = gz_time_ms();
                int result = pkg_install(temporary, target_root, bundle);
                printf("appstore: install %s %s result=%d duration=%u ms\n", a->id, a->version, result,
                       gz_time_ms() - install_started);
                installing = false;
                if (result) {
                    strlcpy(message, fs_strerror(result), sizeof(message));
                    all_updates = false;
                } else
                    snprintf(message, sizeof(message), "%s is on the Home screen.", a->name);
                installed();
                rebuild();
            }
        }
    }
    stop_fetch();
    next_update();
    redraw();
}
static void start_install(int i, const char *root)
{
    struct fs_stat st;
    struct fs_info volume;
    if (*root && fs_stat("/Card", &st)) {
        strlcpy(message, "Insert the card, then try again.", sizeof(message));
        all_updates = false;
        return;
    }
    if (fs_statvfs(*root ? root : "/", &volume) ||
        volume.total - volume.used < catalog.apps[i].size * 2u + 32768u) {
        strlcpy(message, "Not enough space to download and install.", sizeof(message));
        all_updates = false;
        return;
    }
    char apps_dir[32];
    snprintf(apps_dir, sizeof(apps_dir), "%s/Applications", root);
    int mkdir_result = fs_mkdirs(apps_dir);
    if (mkdir_result) {
        strlcpy(message, fs_strerror(mkdir_result), sizeof(message));
        all_updates = false;
        return;
    }
    strlcpy(target_root, root, sizeof(target_root));
    armed = false;
    choose_volume = false;
    snprintf(message, sizeof(message), "Downloading %s...", catalog.apps[i].name);
    if (!begin_fetch(3, i, catalog.apps[i].package))
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
    installed();
    if (local[selected].update)
        start_install(selected, !strncmp(local[selected].bundle, "/Card/", 6) ? "/Card" : "");
    else if (local[selected].installed)
        strlcpy(message, "This app is on the Home screen.", sizeof(message));
    else if (!armed)
        armed = true;
    else {
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
    ui_keyboard_init(&keyboard);
    ui_list_init(&list, gfx_r(0, LIST_Y, UI_W, list_height()), ROW_H);
    fs_mkdirs(CACHE);
    fs_mkdirs("/Applications");
    fs_remove(CACHE "/download.tmp");
    fs_remove("/Applications/.store-download");
    struct fs_stat card;
    if (fs_stat("/Card", &card) == 0)
        fs_remove("/Card/Applications/.store-download");
    if (cached_catalog(CACHE "/catalog.txt"))
        for (int i = 0; i < catalog.count; i++)
            load_icon(i);
    prune_cache();
    rebuild();
    refresh_catalog();
    redraw();
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
                    } else if (c == '\b') {
                        if (cursor > 0) {
                            memmove(query + cursor - 1, query + cursor, strlen(query + cursor) + 1);
                            cursor--;
                        }
                        rebuild();
                    } else if (c >= 32 && c < 127 && strlen(query) < sizeof(query) - 1) {
                        memmove(query + cursor + 1, query + cursor, strlen(query + cursor) + 1);
                        query[cursor++] = c;
                        rebuild();
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
                    installed();
                    rebuild();
                }
            }
            redraw();
        }
        step_fetch();
        if (!fetch && !all_updates)
            for (int i = 0; i < catalog.count; i++)
                if (!icons[i].ok && !icons[i].tried) {
                    icons[i].tried = true;
                    begin_fetch(2, i, catalog.apps[i].icon);
                    break;
                }
        if (gz_time_ms() - last_poll > 2000 && !(fetch && kind == 3)) {
            if (installed()) {
                rebuild();
                dirty = true;
            }
            last_poll = gz_time_ms();
        }
        dirty |= selected >= 0 ? ui_scroll_step(&detail_scroll) : ui_scroll_step(&list.sc);
        if (dirty || ((fetch || editing) && gz_time_ms() - last_draw > 150)) {
            redraw();
            dirty = false;
        }
    }
}
