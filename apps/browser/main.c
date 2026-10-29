/*
 * Web: the browser app, in the style of the first Mobile Safari.
 *
 * The top bar shows the page title and the address field (with the load's
 * progress behind the text, and reload/stop at its end); the toolbar has
 * back, forward, bookmarks, add-bookmark and the start page. Tapping the
 * address opens the keyboard in its URL layout. Links highlight while
 * pressed; form fields take the keyboard, and <select> opens a list.
 *
 * The engine (web.h, load.h) does the rest. State that survives the Home
 * button lives in the file system: /Browser/state (the current page and
 * scroll position), /Browser/bookmarks, and /Browser/seed (tls.c).
 */

#include <glaze/fs.h>
#include <glaze/notify.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "load.h"
#include "plat.h"
#include "script.h"
#include "frames.h"
#include "url.h"

#define BAR_H       44
#define TOOL_H      34
#define VIEW_Y      BAR_H
#define VIEW_H      (UI_H - BAR_H - TOOL_H)
#define KB_Y        (UI_H - UI_KB_H)
#define FIELD_X     6
#define FIELD_Y     18
#define FIELD_H     22
#define URL_MAX     1024
#define MAX_HISTORY 32
#define MAX_MARKS   40
#define SAVE_IDLE   1500
#define START_URL   "res:start.html"
#define STATE_PATH  "/Browser/state"
#define MARKS_PATH  "/Browser/bookmarks"
#define SEED_PATH   "/Browser/seed"     /* for TLS's random numbers */

enum view { V_PAGE, V_BOOKMARKS, V_PICKER };
enum mode { M_BROWSE, M_URL, M_FIELD };
enum press { P_NONE, P_FIELD, P_RELOAD, P_CANCEL, P_BACK, P_FWD, P_MARKS, P_ADD, P_HOME, P_DONE, P_EDIT,
             P_DELETE, P_CONTENT };

struct entry {
    char *url;
    int scroll;
};

struct mark {
    char title[64];
    char url[256];
};

static enum view view = V_PAGE;
static enum mode mode = M_BROWSE;
static struct load ld;
static struct page *page;
static struct ui_scroll sc, frame_sc;
static struct frame_slot *drag_frame;
static struct entry history[MAX_HISTORY];
static int nhist, hpos = -1;
static int pending_hist = -1;       /* history index the load is for, -2 push, -1 none */
static int pending_scroll = -1;
static char pending_frag[256];
static struct mark marks[MAX_MARKS];
static int nmarks;
static bool marks_editing;
static int confirm_mark = -1;       /* bookmark showing its Delete button */
static bool badge_press;
static gfx_rect delete_rect;

/* Input state. */
static enum press pressed;
static struct node *press_link;     /* link highlighted under the finger */
static struct ctl *press_ctl;
static struct ctl *focus;           /* form field with the keyboard */
static struct ui_keyboard kb;
static char edit_url[URL_MAX];
static int url_cursor;
static bool edit_fresh;             /* the first key replaces the whole address */
static struct ui_list list;
static bool in_keyboard;
static char toast[48];
static uint32_t toast_until;
static uint32_t scroll_changed;     /* when the scroll last moved, for saving */
static uint32_t refresh_at;
static char *refresh_url;

/* ---- platform (plat.h) ------------------------------------------------------- */

uint32_t plat_ms(void)
{
    return gz_time_ms();
}

int plat_read_file(const char *path, uint8_t **data, int *len)
{
    struct fs_stat st;

    if (fs_stat(path, &st) < 0 || st.type != FS_FILE)
        return WE_NOTFOUND;
    if (st.size > 1024 * 1024)
        return WE_TOO_BIG;
    *data = malloc(st.size + 1);
    if (!*data)
        return WE_NOMEM;
    int n = fs_read_file(path, *data, st.size);
    if (n < 0) {
        free(*data);
        return WE_NOTFOUND;
    }
    *len = n;
    return 0;
}

const void *plat_res(const char *name, uint32_t *size)
{
    char path[64];

    snprintf(path, sizeof(path), "web/%s", name);
    return gz_romfs_map(path, size);
}

/* Lookups block this thread: libc's network client runs one call at a
 * time per process, so a lookup thread would stall the loader anyway. The
 * result waits in a slot for the poll. */
static struct {
    bool used;
    int rv;
    uint32_t addr;
} lookups[4];

int plat_resolve_start(const char *host)
{
    struct addrinfo hints, *res;

    for (int i = 0; i < 4; i++) {
        if (lookups[i].used)
            continue;
        memset(&hints, 0, sizeof(hints));
        hints.ai_socktype = SOCK_STREAM;
        lookups[i].used = true;
        int e = getaddrinfo(host, NULL, &hints, &res);
        if (e == 0) {
            lookups[i].addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
            lookups[i].rv = 1;
            freeaddrinfo(res);
        } else {
            lookups[i].rv = e == EAI_SYSTEM ? WE_NETDOWN : e == EAI_AGAIN ? WE_TIMEOUT : WE_DNS;
        }
        return i;
    }
    return WE_NOMEM;
}

int plat_resolve_poll(int id, uint32_t *addr)
{
    lookups[id].used = false;
    *addr = lookups[id].addr;
    return lookups[id].rv;
}

void plat_resolve_cancel(int id)
{
    lookups[id].used = false;
}

const void *plat_roots(uint32_t *size)
{
    return gz_romfs_map("ssl/roots.der", size);
}

long long plat_time(void)
{
    struct gz_datetime dt;

    if (gz_rtc_get(&dt) < 0)
        return 0;
    return (long long)gz_dt_to_secs(&dt) + 946684800;   /* 2000-01-01 in Unix time */
}

/* There's no hardware random source: the microsecond clock read after each
 * trip through the scheduler varies with interrupts, network and flash
 * traffic, and other processes. */
void plat_noise(uint8_t *buf, int n)
{
    for (int i = 0; i < n; i++) {
        gz_yield();
        uint64_t t = gz_time_us();
        buf[i] = (uint8_t)(t ^ t >> 8 ^ t >> 16);
    }
}

void plat_load_seed(uint8_t *seed)
{
    fs_read_file(SEED_PATH, seed, PLAT_SEED_LEN);
}

void plat_save_seed(const uint8_t *seed)
{
    fs_mkdir("/Browser");
    fs_write_file(SEED_PATH, seed, PLAT_SEED_LEN);
}

int plat_storage_read(int slot, void *data, int cap)
{
    char path[48]; struct fs_stat st;
    snprintf(path,sizeof path,"/Browser/storage%d",slot);
    if (fs_stat(path,&st)<0) return 0;
    if (st.type!=FS_FILE || st.size>(uint32_t)cap) return -1;
    return fs_read_file(path,data,(uint32_t)cap);
}
int plat_storage_write(int slot, const void *data, int len)
{
    char path[48], tmp[48];
    fs_mkdir("/Browser");
    snprintf(path,sizeof path,"/Browser/storage%d",slot);
    snprintf(tmp,sizeof tmp,"/Browser/storage%d.tmp",slot);
    if (fs_write_file(tmp,data,(uint32_t)len)!=len) return -1;
    if (fs_rename(tmp,path)<0) { fs_remove(tmp);return -1; }
    return 0;
}

/* ---- persistence ---------------------------------------------------------------- */

static int view_h(void)
{
    return mode == M_BROWSE ? VIEW_H : UI_H - ui_keyboard_height() - VIEW_Y;
}

static void save_state(void)
{
    char buf[URL_MAX + 32];

    if (hpos < 0)
        return;
    int n = snprintf(buf, sizeof(buf), "%s\n%d\n", history[hpos].url, sc.offset > 0 ? sc.offset : 0);
    fs_mkdir("/Browser");
    fs_write_file(STATE_PATH, buf, (uint32_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
}

static void save_marks(void)
{
    static char buf[MAX_MARKS * 330];
    int n = 0;

    for (int i = 0; i < nmarks; i++)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s\t%s\n", marks[i].title, marks[i].url);
    fs_mkdir("/Browser");
    fs_write_file(MARKS_PATH, buf, (uint32_t)n);
}

static void add_mark(const char *title, const char *url)
{
    if (nmarks == MAX_MARKS)
        return;
    snprintf(marks[nmarks].title, sizeof(marks[nmarks].title), "%s", title && *title ? title : url);
    snprintf(marks[nmarks].url, sizeof(marks[nmarks].url), "%s", url);
    nmarks++;
}

static void load_marks(void)
{
    static char buf[MAX_MARKS * 330];
    int n = fs_read_file(MARKS_PATH, buf, sizeof(buf) - 1);

    if (n < 0) {
        add_mark("Welcome", START_URL);
        add_mark("68k.news", "http://68k.news/");
        add_mark("FrogFind", "http://frogfind.com/");
        add_mark("The first web site", "http://info.cern.ch/hypertext/WWW/TheProject.html");
        return;
    }
    buf[n] = 0;
    for (char *line = buf; *line && nmarks < MAX_MARKS;) {
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = 0;
            add_mark(line, tab + 1);
        }
        if (!end)
            break;
        line = end + 1;
    }
}

/* ---- navigation ------------------------------------------------------------------- */

static void go(const char *url, const char *post, bool reload, int hist)
{
    char *frag = strchr(url, '#');

    pending_frag[0] = 0;
    if (frag && !post)
        snprintf(pending_frag, sizeof(pending_frag), "%s", frag + 1);
    /* Same document, new fragment: just scroll. */
    if (!post && !reload && page && hist == -2 && frag && url_same_document(url, page->url)) {
        int y = page_anchor_y(page, pending_frag);
        if (y >= 0) {
            sc.offset = MIN(y, ui_scroll_max(&sc));
            sc.animating = false;
        }
        return;
    }
    free(refresh_url);
    refresh_url = NULL;
    pending_hist = hist;
    pending_scroll = -1;
    if (hist >= 0)
        pending_scroll = history[hist].scroll;
    drag_frame = NULL;
    load_begin(&ld, url, post, post ? "application/x-www-form-urlencoded" : NULL, reload, UI_W);
}

static void navigate(const char *url)
{
    go(url, NULL, false, -2);
}

static void history_push(const char *url)
{
    if (hpos >= 0)
        history[hpos].scroll = sc.offset;
    for (int i = hpos + 1; i < nhist; i++)
        free(history[i].url);
    nhist = hpos + 1;
    if (nhist == MAX_HISTORY) {
        free(history[0].url);
        memmove(&history[0], &history[1], sizeof(history[0]) * (MAX_HISTORY - 1));
        nhist--;
    }
    history[nhist].url = strdup(url);
    history[nhist].scroll = 0;
    hpos = nhist++;
}

static void go_history(int delta)
{
    int to = hpos + delta;

    if (to < 0 || to >= nhist)
        return;
    history[hpos].scroll = sc.offset;
    go(history[to].url, NULL, false, to);
}

/* A new page is up. */
static void show_page(struct page *p)
{
    drag_frame = NULL;
    if (page) {
        load_forget(&ld, page);
        page_free(page);
    }
    page = p;
    focus = NULL;
    press_link = NULL;
    press_ctl = NULL;
    if (mode == M_FIELD)
        mode = M_BROWSE;
    if (pending_hist == -2) {
        history_push(page->url);
    } else if (pending_hist >= 0) {
        hpos = pending_hist;
        free(history[hpos].url);
        history[hpos].url = strdup(page->url);
    } else if (hpos >= 0) {
        free(history[hpos].url);        /* reload or refresh: same entry */
        history[hpos].url = strdup(page->url);
    }
    ui_scroll_reset(&sc, page->content_h, view_h());
    if (pending_scroll > 0)
        sc.offset = MIN(pending_scroll, ui_scroll_max(&sc));
    if (pending_frag[0]) {
        int y = page_anchor_y(page, pending_frag);
        if (y > 0)
            sc.offset = MIN(y, ui_scroll_max(&sc));
    }
    pending_hist = -1;
    save_state();

    int delay;
    const char *r = page_refresh(page, &delay);
    if (r && delay <= 10) {
        refresh_url = url_resolve(page->base, r);
        if (refresh_url)
            refresh_url[strcspn(refresh_url, "'\"")] = 0;
        refresh_at = gz_time_ms() + (uint32_t)delay * 1000u;
    }
}

static void relayout(void)
{
    int old = sc.offset;

    sc.content_h = page->content_h;
    sc.view_h = view_h();
    if (!sc.dragging && !sc.animating)
        sc.offset = MIN(old, ui_scroll_max(&sc));
    if (pending_frag[0] && old == 0) {
        int y = page_anchor_y(page, pending_frag);
        if (y > 0)
            sc.offset = MIN(y, ui_scroll_max(&sc));
    }
}

/* ---- drawing ------------------------------------------------------------------------ */

static void triangle(gfx_surface *s, int cx, int cy, int size, bool left, uint32_t c, int alpha)
{
    for (int i = 0; i < size; i++) {
        int half = i * 9 / 10;
        int x = left ? cx - size / 2 + i : cx + size / 2 - i;
        gfx_fill_alpha(s, gfx_r(x, cy - half, 1, 2 * half + 1), c, alpha);
    }
}

static const char *display_url(const char *u)
{
    return !strncmp(u, "http://", 7) ? u + 7 : u;
}

static void draw_field(gfx_surface *s, bool editing)
{
    int w = editing ? UI_W - FIELD_X - 64 : UI_W - 2 * FIELD_X;
    gfx_rect r = gfx_r(FIELD_X, FIELD_Y, w, FIELD_H);
    const gfx_font *f = ui_font(FONT_R12);

    gfx_round_rect(s, r, 6, 0x56657E, 255);
    gfx_round_rect(s, gfx_r(r.x + 1, r.y + 1, r.w - 2, r.h - 2), 5, 0xFFFFFF, 255);
    int text_w = r.w - (editing ? 12 : 30);
    if (editing) {
        gfx_rect old = s->clip;
        gfx_set_clip(s, gfx_r(r.x + 6, r.y, text_w, r.h));
        int tw = gfx_text_width(f, edit_url);
        int cw = gfx_text_width_n(f, edit_url, url_cursor);
        int x = r.x + 6 - MAX(0, (edit_fresh ? tw : cw) - text_w + 3);
        if (edit_fresh && tw)
            gfx_fill(s, gfx_r(x - 1, r.y + 4, MIN(tw, text_w) + 2, r.h - 8), 0xB4D2F7);
        if (!edit_fresh) ui_draw_edit_selection(s, f, edit_url, url_cursor, 0, strlen(edit_url), x, r.y + 4, r.h - 8);
        gfx_text(s, x, r.y + (r.h - f->line_height) / 2, f, edit_url, 0x111111, 255);
        if (!edit_fresh)
            gfx_fill(s, gfx_r(x + cw + 1, r.y + 4, 2, r.h - 8), 0x3B7DDD);
        s->clip = old;
        return;
    }
    if (ld.state != LS_IDLE) {
        int p = load_progress(&ld);
        int pw = (r.w - 2) * p / 1000;
        gfx_round_rect_vgradient(s, gfx_r(r.x + 1, r.y + 1, pw, r.h - 2), 5, 0xC9E0FA, 0x96C0F0, 255);
    }
    const char *u = page ? page->url : ld.url ? ld.url : "";
    bool shown = true;
    if (ld.state != LS_IDLE && ld.url) {
        u = ld.url;
        shown = false;
    }
    int tx = r.x + 7;
    if (shown && page && !page->is_error && !strncmp(u, "https:", 6)) {
        /* A padlock: the page came over TLS with a verified certificate. */
        int lx = tx, ly = r.y + r.h / 2 - 1;
        uint32_t lc = 0x5A6474;
        gfx_line_aa(s, (lx + 1) * 16 + 8, (ly + 1) * 16, (lx + 1) * 16 + 8, (ly - 3) * 16, 20, lc, 255);
        gfx_line_aa(s, (lx + 6) * 16 + 8, (ly + 1) * 16, (lx + 6) * 16 + 8, (ly - 3) * 16, 20, lc, 255);
        gfx_line_aa(s, (lx + 1) * 16 + 8, (ly - 3) * 16, (lx + 3) * 16, (ly - 5) * 16, 20, lc, 255);
        gfx_line_aa(s, (lx + 5) * 16, (ly - 5) * 16, (lx + 6) * 16 + 8, (ly - 3) * 16, 20, lc, 255);
        gfx_line_aa(s, (lx + 3) * 16, (ly - 5) * 16, (lx + 5) * 16, (ly - 5) * 16, 20, lc, 255);
        gfx_round_rect(s, gfx_r(lx, ly, 8, 6), 1, lc, 255);
        tx += 11;
        text_w -= 11;
    }
    gfx_text_fit(s, tx, r.y + (r.h - f->line_height) / 2, text_w, f, display_url(u), 0x333333, 255);

    /* Reload, or stop while loading. */
    int cx = r.x + r.w - 12, cy = r.y + r.h / 2;
    uint32_t ic = pressed == P_RELOAD ? 0x1A4FA8 : 0x5A6474;
    if (ld.state != LS_IDLE) {
        gfx_line_aa(s, (cx - 4) * 16, (cy - 4) * 16, (cx + 4) * 16, (cy + 4) * 16, 28, ic, 255);
        gfx_line_aa(s, (cx + 4) * 16, (cy - 4) * 16, (cx - 4) * 16, (cy + 4) * 16, 28, ic, 255);
    } else {
        /* A circular arrow: an arc of short segments and a head. */
        static const int8_t pts[][2] = { { 5, 0 }, { 4, 3 }, { 2, 5 }, { -1, 5 }, { -4, 3 }, { -5, 0 },
                                         { -4, -3 }, { -2, -5 }, { 1, -5 } };
        for (int i = 0; i + 1 < (int)(sizeof(pts) / sizeof(pts[0])); i++)
            gfx_line_aa(s, (cx + pts[i][0]) * 16, (cy + pts[i][1]) * 16, (cx + pts[i + 1][0]) * 16,
                        (cy + pts[i + 1][1]) * 16, 24, ic, 255);
        triangle(s, cx + 2, cy - 5, 5, false, ic, 255);
    }
}

static void draw_bar(gfx_surface *s)
{
    gfx_vgradient(s, gfx_r(0, 0, UI_W, BAR_H / 2), 0xB4C0D0, 0x8B9DB5, 255);
    gfx_vgradient(s, gfx_r(0, BAR_H / 2, UI_W, BAR_H - BAR_H / 2), 0x8093AD, 0x6D84A2, 255);
    gfx_fill(s, gfx_r(0, BAR_H - 1, UI_W, 1), UI_NAV_LINE);
    gfx_fill_alpha(s, gfx_r(0, 0, UI_W, 1), 0xFFFFFF, 90);
    const gfx_font *tf = ui_font(FONT_B10);
    const char *title = page && page->title && *page->title ? page->title : "Web";
    if (ld.state == LS_DOC)
        title = "Loading" GFX_CH_ELLIPSIS;
    int tw = gfx_text_width(tf, title);
    if (tw > UI_W - 16) {
        gfx_rect old = s->clip;
        gfx_set_clip(s, gfx_r(8, 0, UI_W - 16, 18));
        gfx_text_fit(s, 8, 3, UI_W - 16, tf, title, 0xFFFFFF, 255);
        s->clip = old;
    } else {
        gfx_text_shadow(s, (UI_W - tw) / 2, 3, tf, title, 0xFFFFFF, 0x3E4E66, -1, 255);
    }
    draw_field(s, mode == M_URL);
    if (mode == M_URL) {
        gfx_rect b = gfx_r(UI_W - 58, FIELD_Y - 1, 52, 24);
        gfx_round_rect_vgradient(s, b, 5, pressed == P_CANCEL ? 0x5A6F8C : 0x7E93AF,
                                 pressed == P_CANCEL ? 0x3A4C68 : 0x4A6383, 255);
        gfx_text_center(s, b.x, b.y + (b.h - ui_font(FONT_B10)->line_height) / 2, b.w, ui_font(FONT_B10),
                        "Cancel", 0xFFFFFF, 255);
    }
}

static int tool_x(int i)
{
    return 24 + i * 48;
}

static void draw_toolbar(gfx_surface *s)
{
    int y = UI_H - TOOL_H, cy = y + TOOL_H / 2;

    gfx_vgradient(s, gfx_r(0, y, UI_W, TOOL_H / 2), 0x8B9DB5, 0x7187A5, 255);
    gfx_vgradient(s, gfx_r(0, y + TOOL_H / 2, UI_W, TOOL_H - TOOL_H / 2), 0x667D9C, 0x58708F, 255);
    gfx_fill(s, gfx_r(0, y, UI_W, 1), UI_NAV_LINE);
    gfx_fill_alpha(s, gfx_r(0, y + 1, UI_W, 1), 0xFFFFFF, 70);
    static const enum press order[5] = { P_BACK, P_FWD, P_MARKS, P_ADD, P_HOME };
    for (int i = 0; i < 5; i++) {
        int cx = tool_x(i);
        bool enabled = order[i] == P_BACK ? hpos > 0 : order[i] == P_FWD ? hpos + 1 < nhist
                     : order[i] == P_ADD ? page != NULL : true;
        int a = enabled ? 255 : 90;
        if (pressed == order[i] && enabled)
            gfx_circle(s, cx, cy, 15, 0xFFFFFF, 60);
        switch (order[i]) {
        case P_BACK:
            triangle(s, cx, cy, 13, true, 0xFFFFFF, a);
            break;
        case P_FWD:
            triangle(s, cx, cy, 13, false, 0xFFFFFF, a);
            break;
        case P_MARKS:
            /* An open book. */
            gfx_round_rect(s, gfx_r(cx - 11, cy - 7, 10, 14), 2, 0xFFFFFF, a);
            gfx_round_rect(s, gfx_r(cx + 1, cy - 7, 10, 14), 2, 0xFFFFFF, a);
            for (int k = 0; k < 3; k++) {
                gfx_fill(s, gfx_r(cx - 9, cy - 4 + 3 * k, 6, 1), 0x6D84A2);
                gfx_fill(s, gfx_r(cx + 3, cy - 4 + 3 * k, 6, 1), 0x6D84A2);
            }
            break;
        case P_ADD:
            gfx_fill_alpha(s, gfx_r(cx - 8, cy - 2, 16, 4), 0xFFFFFF, a);
            gfx_fill_alpha(s, gfx_r(cx - 2, cy - 8, 4, 16), 0xFFFFFF, a);
            break;
        default:
            /* A house. */
            for (int k = 0; k < 8; k++)
                gfx_fill_alpha(s, gfx_r(cx - k - 1, cy - 9 + k, 2 * k + 2, 1), 0xFFFFFF, a);
            gfx_fill_alpha(s, gfx_r(cx - 6, cy - 1, 12, 9), 0xFFFFFF, a);
            gfx_fill(s, gfx_r(cx - 2, cy + 3, 4, 5), 0x667D9C);
            break;
        }
    }
}

static void draw_page(gfx_surface *s)
{
    gfx_rect v = gfx_r(0, VIEW_Y, UI_W, view_h());

    if (!page) {
        gfx_fill(s, v, 0xFFFFFF);
        return;
    }
    paint_page(page, s, v, sc.offset, v, press_link, focus);
    /* A thin scroll indicator while moving. */
    if ((sc.dragging && sc.moved) || sc.animating) {
        int max = ui_scroll_max(&sc);
        if (max > 0) {
            int h = MAX(v.h * v.h / page->content_h, 20);
            int off = MAX(0, MIN(sc.offset, max));
            int y = v.y + 2 + (v.h - 4 - h) * off / max;
            gfx_round_rect(s, gfx_r(UI_W - 6, y, 4, h), 2, 0x000000, 110);
        }
    }
}

static void draw_toast(gfx_surface *s)
{
    if (!toast[0] || (int32_t)(gz_time_ms() - toast_until) >= 0) {
        toast[0] = 0;
        return;
    }
    const gfx_font *f = ui_font(FONT_B12);
    int w = gfx_text_width(f, toast) + 24;
    gfx_rect r = gfx_r((UI_W - w) / 2, VIEW_Y + VIEW_H / 2 - 18, w, 36);
    gfx_round_rect(s, r, 8, 0x000000, 170);
    gfx_text_center(s, r.x, r.y + (r.h - f->line_height) / 2, r.w, f, toast, 0xFFFFFF, 255);
}

static void draw_browser(gfx_surface *s)
{
    if (focus && mode == M_FIELD) {
        focus->selection_start = focus->selection_end = focus->cursor;
        ui_edit_selection(focus->value, focus->cursor, &focus->selection_start, &focus->selection_end);
    }
    draw_page(s);
    draw_bar(s);
    if (mode == M_BROWSE)
        draw_toolbar(s);
    else if (!ui_has_hardware_keyboard())
        ui_draw_keyboard(s, &kb, KB_Y);
    draw_toast(s);
}

/* Bookmarks and the <select> picker share a list view. */
static int list_count(void)
{
    return view == V_BOOKMARKS ? nmarks : focus ? focus->nopts : 0;
}

#define ROW_H   44
#define CELL_X  9
#define TEXT_X  (CELL_X + 10)

static void draw_list_row(gfx_surface *s, int i, gfx_rect row, bool pressed_here, void *ctx)
{
    gfx_rect r = gfx_r(CELL_X, row.y, UI_W - 2 * CELL_X, ROW_H);
    bool sel = pressed_here && !marks_editing;
    uint32_t ink = sel ? 0xFFFFFF : 0x000000;
    int x = TEXT_X;

    (void)ctx;
    ui_draw_group_cell(s, r, ui_group_pos(i, list.count), sel);
    if (view == V_BOOKMARKS) {
        int right = r.x + r.w - 10;
        if (marks_editing) {
            ui_draw_delete_badge(s, x + 7, r.y + ROW_H / 2, confirm_mark == i);
            x += UI_DELETE_BADGE_W;
        }
        if (confirm_mark == i) {
            delete_rect = ui_draw_delete_button(s, r.x + r.w - 8, r.y + ROW_H / 2, pressed == P_DELETE);
            right = delete_rect.x - 6;
        }
        gfx_text_fit(s, x, r.y + 5, right - x, ui_font(FONT_B13), marks[i].title, ink, 255);
        gfx_text_fit(s, x, r.y + 24, right - x, ui_font(FONT_R11), display_url(marks[i].url),
                     sel ? 0xFFFFFF : 0x7A808A, 255);
    } else {
        const char *t = focus->opts[i]->ctext;
        gfx_text_fit(s, x, r.y + (ROW_H - ui_font(FONT_B14)->line_height) / 2, r.w - 40, ui_font(FONT_B14),
                     t, i == focus->sel && !sel ? UI_TEXT_DETAIL : ink, 255);
        if (i == focus->sel)
            gfx_text(s, r.x + r.w - 24, r.y + (ROW_H - ui_font(FONT_B14)->line_height) / 2, ui_font(FONT_B14),
                     GFX_CH_CHECK, sel ? 0xFFFFFF : UI_TEXT_DETAIL, 255);
    }
}

static void draw_list(gfx_surface *s)
{
    gfx_rect back;
    const char *title = view == V_BOOKMARKS ? "Bookmarks" : "Choose";

    ui_draw_pinstripes(s, list.area);
    ui_list_draw(s, &list, draw_list_row, NULL);
    if (!list.count)
        gfx_text_center(s, 0, UI_NAV_H + 60, UI_W, ui_font(FONT_B14), "No Bookmarks", 0x6A7280, 255);
    ui_draw_navbar(s, title, NULL, &back, 0);
    if (!marks_editing)         /* the left button ends editing */
        ui_draw_bar_button(s, "Done", true, pressed == P_DONE, true, 0);
    if (view == V_BOOKMARKS)
        ui_draw_bar_button(s, marks_editing ? "Done" : "Edit", false, pressed == P_EDIT, marks_editing, 0);
}

static void draw_view(gfx_surface *s)
{
    if (view == V_PAGE)
        draw_browser(s);
    else
        draw_list(s);
}

static void redraw(void)
{
    draw_view(&ui.surf);
    ui_invalidate_all();
    ui_present();
}

static void open_list(enum view v)
{
    view = v;
    pressed = P_NONE;
    marks_editing = false;
    confirm_mark = -1;
    ui_list_init(&list, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H), ROW_H);
    list.pad = list.footer_h = 10;
    ui_list_reset(&list, list_count());
    ui_transition(draw_view, true);
}

static void close_list(void)
{
    view = V_PAGE;
    pressed = P_NONE;
    ui_transition(draw_view, false);
}

/* ---- form fields ---------------------------------------------------------------------- */

static void submit(struct node *form, struct ctl *submitter)
{
    if (!form || (form->owner->host && form->owner->host->sandboxed) ||
        !script_event(form->owner, form, "submit"))
        return;
    char *post = NULL;
    struct page *owner = form->owner;
    char *url = owner ? forms_submit(owner, form, submitter, &post) : NULL;

    if (!url)
        return;
    mode = M_BROWSE;
    focus = NULL;
    page_navigate(owner, url, node_attr(form, "target"), post);
    free(url);
    free(post);
}

/* Scrolls so the focused field sits in the space above the keyboard. */
static void reveal_focus(void)
{
    struct page *owner = focus ? focus->node->owner : NULL;
    for (int i = 0; owner && i < owner->nitems; i++) {
        struct ditem *d = &owner->items[i];
        if (d->kind != DI_CTL || d->u.ctl != focus)
            continue;
        if (owner->host) {
            struct frame_slot *f = owner->host;
            f->scroll = MAX(0, MIN(d->y - 12, owner->content_h - f->h));
            return;
        }
        sc.view_h = view_h();
        int want = d->y - 20;
        if (d->y < sc.offset || d->y + d->h > sc.offset + sc.view_h)
            sc.offset = MAX(0, MIN(want, ui_scroll_max(&sc)));
        return;
    }
}

static void activate_ctl(struct ctl *c)
{
    if (c->disabled)
        return;
    switch (c->type) {
    case C_TEXT:
    case C_PASSWORD:
    case C_TEXTAREA:
        focus = c;
        c->cursor = c->len;
        c->selection_start = c->selection_end = c->cursor;
        mode = M_FIELD;
        ui_keyboard_edit_init(&kb, c->type == C_PASSWORD ? UI_EDIT_SECRET : c->type == C_TEXTAREA ? UI_EDIT_MULTILINE : 0);
        kb.shift = false;
        sc.view_h = view_h();
        reveal_focus();
        break;
    case C_CHECKBOX:
    case C_RADIO:
        ctl_toggle(c->node->owner, c);
        script_event(c->node->owner, c->node, "change");
        break;
    case C_SUBMIT:
    case C_IMAGE:
        submit(c->form, c);
        break;
    case C_RESET:
        if (script_event(c->node->owner, c->form, "reset")) forms_reset(c->node->owner, c->form);
        break;
    case C_SELECT:
        focus = c;
        open_list(V_PICKER);
        break;
    default:
        break;
    }
}

static struct gz_input_event char_event(int ch)
{
    struct gz_input_event e = { .type = GZ_EV_TEXT, .code = (uint16_t)ch };
    if (ch == '\b' || ch == '\n') {
        e.type = GZ_EV_KEY_DOWN;
        e.code = ch == '\b' ? GZ_KEY_BACKSPACE : GZ_KEY_ENTER;
    }
    return e;
}

static void field_input(const struct gz_input_event *e)
{
    struct ctl *c = focus;
    if (!c) return;
    if (ui_input_char(e) == '\n' && c->type != C_TEXTAREA) {
        submit(c->form, NULL);
        return;
    }
    ui_edit_event(c->value, c->cap, &c->cursor, e,
        c->type == C_PASSWORD ? UI_EDIT_SECRET : c->type == C_TEXTAREA ? UI_EDIT_MULTILINE : 0);
    c->len = (int)strlen(c->value);
    if (ui_edit_modified()) script_event(c->node->owner, c->node, "input");
}

static void field_key(int ch)
{
    struct gz_input_event e = char_event(ch);
    field_input(&e);
}

/* ---- the address field ------------------------------------------------------------------ */

static void start_url_edit(void)
{
    mode = M_URL;
    focus = NULL;
    ui_keyboard_edit_init(&kb, UI_EDIT_URL);
    kb.url = true;
    kb.shift = false;
    snprintf(edit_url, sizeof(edit_url), "%s", page ? page->url : "");
    edit_fresh = true;
    url_cursor = (int)strlen(edit_url);
    ui_edit_key(edit_url, sizeof(edit_url), &url_cursor, UI_KEY_SELECT_ALL, UI_EDIT_URL);
    sc.view_h = view_h();
}

static void finish_url_edit(bool go_there)
{
    mode = M_BROWSE;
    sc.view_h = view_h();
    if (page)
        sc.offset = MIN(sc.offset, ui_scroll_max(&sc));
    if (!go_there || !edit_url[0])
        return;
    char url[URL_MAX + 16];
    if (strstr(edit_url, ":") && (strstr(edit_url, "://") || !strncasecmp(edit_url, "about:", 6) ||
                                  !strncasecmp(edit_url, "res:", 4) || !strncasecmp(edit_url, "data:", 5)))
        snprintf(url, sizeof(url), "%s", edit_url);
    else if (!strchr(edit_url, '.') && !strchr(edit_url, '/'))
        snprintf(url, sizeof(url), "http://frogfind.com/?q=%s", edit_url);     /* a search */
    else
        snprintf(url, sizeof(url), "http://%s", edit_url);
    char *abs = url_resolve(NULL, url);
    navigate(abs ? abs : url);
    free(abs);
}

static void url_input(const struct gz_input_event *e)
{
    int ch = ui_input_char(e);
    if (ch == '\n') { finish_url_edit(true); return; }
    if (edit_fresh && (ch || (e->type == GZ_EV_KEY_DOWN && e->code == GZ_KEY_DELETE))) {
        edit_fresh = false;
        edit_url[0] = 0;
        url_cursor = 0;
        if (ch == '\b' || e->code == GZ_KEY_DELETE) return;
    }
    if (e->type == GZ_EV_KEY_DOWN) edit_fresh = false;
    if (ch != ' ' && ui_edit_event(edit_url, sizeof(edit_url), &url_cursor, e, UI_EDIT_URL)) edit_fresh = false;
}

static void url_key(int ch)
{
    if (ch == UI_KEY_DOTCOM) {
        for (const char *p = ".com"; *p; p++) {
            struct gz_input_event e = char_event(*p);
            url_input(&e);
        }
    } else {
        struct gz_input_event e = char_event(ch);
        url_input(&e);
    }
}

/* ---- input ---------------------------------------------------------------------------------- */

static enum press bar_hit(int x, int y)
{
    if (y < BAR_H) {
        if (mode == M_URL)
            return x >= UI_W - 64 ? P_CANCEL : P_FIELD;
        if (y >= FIELD_Y - 4 && x >= UI_W - FIELD_X - 26)
            return P_RELOAD;
        return y >= FIELD_Y - 6 ? P_FIELD : P_NONE;
    }
    if (mode == M_BROWSE && y >= UI_H - TOOL_H) {
        static const enum press order[5] = { P_BACK, P_FWD, P_MARKS, P_ADD, P_HOME };
        return order[MIN(x / 48, 4)];
    }
    return P_NONE;
}

static void bar_action(enum press p)
{
    switch (p) {
    case P_FIELD:
        if (mode != M_URL)
            start_url_edit();
        break;
    case P_CANCEL:
        finish_url_edit(false);
        break;
    case P_RELOAD:
        if (ld.state != LS_IDLE)
            load_stop(&ld);
        else if (page)
            go(page->url, NULL, true, hpos);
        break;
    case P_BACK:
        go_history(-1);
        break;
    case P_FWD:
        go_history(1);
        break;
    case P_MARKS:
        open_list(V_BOOKMARKS);
        break;
    case P_ADD:
        if (page && nmarks < MAX_MARKS) {
            add_mark(page->title, page->url);
            save_marks();
            snprintf(toast, sizeof(toast), "Added to Bookmarks");
            toast_until = gz_time_ms() + 1200;
        }
        break;
    case P_HOME:
        navigate(START_URL);
        break;
    default:
        break;
    }
}

static void content_tap(int x, int y)
{
    struct ditem *d = page ? page_hit(page, x, y + sc.offset - VIEW_Y) : NULL;
    if (!d) return;
    struct node *target = d->node ? d->node : d->link;
    if (target && target->type == N_TEXT) target = target->parent;
    struct page *owner = target ? target->owner : page;
    struct ctl *control = d->kind == DI_CTL ? d->u.ctl : NULL;
    for (struct node *n = target; n && !control; n = n->parent)
        control = n->ctl;
    if (control && control->disabled)
        return;
    if (target && !script_event(owner, target, "click"))
        return;
    if (control) {
        activate_ctl(control);
        return;
    }
    if (d->link) {
        owner = d->link->owner;
        const char *href = node_attr(d->link, "href");
        if (href && !strncasecmp(href, "javascript:", 11)) {
            if (script_allowed(owner, NULL, NULL))
                script_eval(owner, href + 11, (int)strlen(href + 11), NULL);
            return;
        }
        char *url = page_link_url(owner, d->link);
        if (url)
            page_navigate(owner, url, node_attr(d->link, "target"), NULL);
        free(url);
    }
}

static void browser_event(const struct gz_input_event *e)
{
    /* The keyboard, when up, owns its area. */
    if (!ui_has_hardware_keyboard() && mode != M_BROWSE && (e->y >= KB_Y || in_keyboard) &&
        pressed == P_NONE) {
        bool changed;
        if (e->type == GZ_EV_TOUCH_DOWN)
            in_keyboard = true;
        int ch = ui_keyboard_event(&kb, e, KB_Y, &changed);
        if (e->type == GZ_EV_TOUCH_UP)
            in_keyboard = false;
        if (ch) {
            if (mode == M_URL)
                url_key(ch);
            else
                field_key(ch);
        }
        return;
    }
    if (e->type == GZ_EV_TOUCH_DOWN) {
        pressed = bar_hit(e->x, e->y);
        press_link = NULL;
        press_ctl = NULL;
        if (pressed != P_NONE)
            return;
        if (e->y >= VIEW_Y && e->y < VIEW_Y + view_h()) {
            pressed = P_CONTENT;
            drag_frame = page ? page_frame_at(page, e->x, e->y + sc.offset - VIEW_Y) : NULL;
            if (drag_frame && (!drag_frame->page || drag_frame->page->content_h <= drag_frame->h)) drag_frame = NULL;
            if (drag_frame) {
                ui_scroll_reset(&frame_sc, drag_frame->page->content_h, drag_frame->h);
                frame_sc.offset = drag_frame->scroll;
            }
            struct ui_scroll *active = drag_frame ? &frame_sc : &sc;
            ui_scroll_event(active, e, e->y - VIEW_Y);
            struct ditem *d = page && !active->moved ? page_hit(page, e->x, e->y + sc.offset - VIEW_Y) : NULL;
            if (d && d->link)
                press_link = d->link;
            else if (d && d->kind == DI_CTL)
                press_ctl = d->u.ctl;
        }
        return;
    }
    if (pressed == P_CONTENT) {
        struct ui_scroll *active = drag_frame ? &frame_sc : &sc;
        bool changed = ui_scroll_event(active, e, e->y - VIEW_Y);
        if (drag_frame) drag_frame->scroll = MAX(0, MIN(active->offset, ui_scroll_max(active)));
        if (changed || active->moved) {
            press_link = NULL;
            press_ctl = NULL;
            scroll_changed = gz_time_ms();
        }
        if (e->type == GZ_EV_TOUCH_UP) {
            pressed = P_NONE;
            bool tap = !active->moved;
            press_link = NULL;
            press_ctl = NULL;
            if (tap) {
                if (mode == M_FIELD) {
                    /* A tap elsewhere puts the keyboard away. */
                    struct ditem *d = page ? page_hit(page, e->x, e->y + sc.offset - VIEW_Y) : NULL;
                    if (!d || d->kind != DI_CTL || d->u.ctl != focus) {
                        mode = M_BROWSE;
                        focus = NULL;
                        sc.view_h = view_h();
                        sc.offset = MIN(sc.offset, ui_scroll_max(&sc));
                    }
                    if (d && d->kind == DI_CTL && d->u.ctl != focus)
                        content_tap(e->x, e->y);
                    return;
                }
                if (mode == M_URL) {
                    finish_url_edit(false);
                    return;
                }
                content_tap(e->x, e->y);
            }
        }
        return;
    }
    if (e->type == GZ_EV_TOUCH_UP) {
        enum press p = pressed;
        pressed = P_NONE;
        if (p != P_NONE && bar_hit(e->x, e->y) == p)
            bar_action(p);
    }
}

static void delete_mark(int i)
{
    memmove(&marks[i], &marks[i + 1], sizeof(marks[0]) * (size_t)(nmarks - i - 1));
    nmarks--;
    save_marks();
    ui_list_set_count(&list, nmarks);
}

static void list_event(const struct gz_input_event *e)
{
    if (e->type == GZ_EV_TOUCH_DOWN) {
        pressed = P_NONE;
        badge_press = false;
        if (e->y < UI_NAV_H) {
            if (e->x >= UI_W - 70 && !marks_editing)
                pressed = P_DONE;
            else if (e->x < 70 && view == V_BOOKMARKS)
                pressed = P_EDIT;
            return;
        }
        if (confirm_mark >= 0 && gfx_rect_contains(delete_rect, e->x, e->y)) {
            pressed = P_DELETE;
            return;
        }
    }
    bool dirty = false;
    int row = ui_list_event(&list, e, &dirty);
    if (e->type == GZ_EV_TOUCH_DOWN && marks_editing && list.pressed >= 0)
        badge_press = e->x < TEXT_X + UI_DELETE_BADGE_W;
    if (e->type != GZ_EV_TOUCH_UP)
        return;
    enum press p = pressed;
    pressed = P_NONE;
    if (p == P_DONE && e->y < UI_NAV_H) {
        if (view == V_PICKER)
            focus = NULL;
        close_list();
    } else if (p == P_EDIT && e->y < UI_NAV_H) {
        marks_editing = !marks_editing;
        confirm_mark = -1;
    } else if (p == P_DELETE) {
        if (gfx_rect_contains(delete_rect, e->x, e->y))
            delete_mark(confirm_mark);
        confirm_mark = -1;
    } else if (row >= 0) {
        if (view == V_PICKER) {
            focus->sel = row;
            script_event(focus->node->owner, focus->node, "change");
            focus = NULL;
            close_list();
        } else if (confirm_mark >= 0) {
            confirm_mark = -1;      /* any other tap cancels a pending delete */
        } else if (marks_editing) {
            if (badge_press)
                confirm_mark = row;
        } else {
            char url[256];
            snprintf(url, sizeof(url), "%s", marks[row].url);
            close_list();
            navigate(url);
        }
    }
}

static void back_key(void)
{
    if (view != V_PAGE && confirm_mark >= 0) {
        confirm_mark = -1;
    } else if (view != V_PAGE && marks_editing) {
        marks_editing = false;
    } else if (view != V_PAGE) {
        focus = NULL;
        close_list();
    } else if (mode == M_URL) {
        finish_url_edit(false);
    } else if (mode == M_FIELD) {
        mode = M_BROWSE;
        focus = NULL;
        sc.view_h = view_h();
    } else if (ld.state != LS_IDLE) {
        load_stop(&ld);
    } else {
        go_history(-1);
    }
}

static void key_event(const struct gz_input_event *e)
{
    if (view == V_PAGE && page && e->type == GZ_EV_KEY_DOWN && e->code == GZ_KEY_TAB) {
        struct ctl *first = NULL, *next = NULL;
        bool after = focus == NULL;
        for (struct ctl *c = page->ctls; c; c = c->next) {
            if (!c->disabled && (c->type == C_TEXT || c->type == C_PASSWORD || c->type == C_TEXTAREA)) {
                if (!first) first = c;
                if (after && !next) next = c;
            }
            if (c == focus) after = true;
        }
        if (next || first) activate_ctl(next ? next : first);
        return;
    }
    if (view == V_PAGE && mode != M_BROWSE &&
        (e->type == GZ_EV_TEXT || (e->type == GZ_EV_KEY_DOWN && e->code != GZ_KEY_ESC))) {
        if (mode == M_URL) url_input(e);
        else field_input(e);
        return;
    }
    if (e->type != GZ_EV_KEY_DOWN)
        return;
    switch (e->code) {
    case GZ_KEY_ESC:
        back_key();
        break;
    case GZ_KEY_UP:
    case GZ_KEY_DOWN:
    case GZ_KEY_PAGEUP:
    case GZ_KEY_PAGEDOWN:
        if (view == V_PAGE && page) {
            sc.animating = false;
            sc.offset += (e->code == GZ_KEY_UP || e->code == GZ_KEY_PAGEUP) ? -view_h() * 3 / 4 : view_h() * 3 / 4;
            sc.offset = MAX(0, MIN(sc.offset, ui_scroll_max(&sc)));
            scroll_changed = gz_time_ms();
        }
        break;
    case GZ_KEY_LEFT:
        if (view == V_PAGE && mode == M_BROWSE)
            go_history(-1);
        break;
    case GZ_KEY_RIGHT:
        if (view == V_PAGE && mode == M_BROWSE)
            go_history(1);
        break;
    }
}

/* ---- main loop --------------------------------------------------------------------------- */

static void start(void)
{
    static char buf[URL_MAX + 32];
    int n = fs_read_file(STATE_PATH, buf, sizeof(buf) - 1);

    load_marks();
    if (n > 0) {
        buf[n] = 0;
        char *nl = strchr(buf, '\n');
        if (nl) {
            *nl = 0;
            history_push(buf);
            go(buf, NULL, false, hpos);
            pending_scroll = atoi(nl + 1);
            return;
        }
    }
    navigate(START_URL);
}

int main(void)
{
    ui_init(SB_STATUS_GRAY);
    ui_scroll_reset(&sc, 0, VIEW_H);
    start();
    redraw();

    for (;;) {
        struct gz_input_event ev;
        bool busy = ld.state != LS_IDLE;
        struct ui_scroll *s = view == V_PAGE ? (drag_frame ? &frame_sc : &sc) : &list.sc;
        int timeout = s->animating ? 16 : busy || ld.shown ? 30 : toast[0] || refresh_url ? 100 : scroll_changed ? 300 : -1;
        bool dirty = false;

        if (ui_next_event(&ev, timeout)) {
            do {
                if (ev.type == GZ_EV_KEY_DOWN || ev.type == GZ_EV_KEY_UP || ev.type == GZ_EV_TEXT)
                    key_event(&ev);
                else if (ev.type == GZ_EV_APP_ACTIVATE)
                    ;
                else if (ev.type == GZ_EV_TOUCH_CANCEL) {
                    /* Home mid-touch: drop the touch without acting on it. */
                    bool changed;
                    pressed = P_NONE;
                    press_link = NULL;
                    press_ctl = NULL;
                    in_keyboard = false;
                    ui_scroll_event(&sc, &ev, 0);
                    ui_scroll_event(&frame_sc, &ev, 0);
                    drag_frame = NULL;
                    ui_list_event(&list, &ev, &changed);
                    ui_keyboard_event(&kb, &ev, 0, &changed);
                } else if (ev.type == GZ_EV_APP_DEACTIVATE) {
                    if (scroll_changed)
                        save_state();   /* it may be ended while suspended */
                    scroll_changed = 0;
                }
                else if (view == V_PAGE)
                    browser_event(&ev);
                else
                    list_event(&ev);
            } while (ui_next_event(&ev, 0));
            dirty = true;
        }
        /* The old page stays visible during a top-level fetch. A new click
         * there should replace that request too. */
        if(page && page!=ld.shown && page->navigate_url) {
            char *url=page->navigate_url,*post=page->navigate_post;
            page->navigate_url=page->navigate_post=NULL;
            if(post)go(url,post,false,-2);else navigate(url);
            free(url);free(post);
        }
        if (ld.state != LS_IDLE || ld.shown) {
            if(page)page->scroll_y=sc.offset;
            int f = load_step(&ld);
            if (f & LF_CONTEXT) {
                pressed = P_NONE;
                focus = NULL;
                press_ctl = NULL;
                press_link = NULL;
                drag_frame = NULL;
                mode = M_BROWSE;
            }
            if (f & LF_NEW_PAGE)
                show_page(ld.shown);
            else if (f & LF_LAYOUT)
                relayout();
            if ((f & LF_NAVIGATE) && ld.shown && ld.shown->navigate_url) {
                char *url = ld.shown->navigate_url, *post = ld.shown->navigate_post;
                ld.shown->navigate_url = ld.shown->navigate_post = NULL;
                if (post)
                    go(url, post, false, -2);
                else
                    navigate(url);
                free(url);
                free(post);
                drag_frame = NULL;
            }
            dirty |= f != 0 || busy;
        }
        s = view == V_PAGE ? (drag_frame ? &frame_sc : &sc) : &list.sc;
        if (ui_scroll_step(s)) {
            if (drag_frame && s == &frame_sc)
                drag_frame->scroll = MAX(0, MIN(s->offset, ui_scroll_max(s)));
            dirty = true;
            if (s == &sc)
                scroll_changed = gz_time_ms();
        }
        uint32_t now = gz_time_ms();
        if (scroll_changed && !sc.dragging && !sc.animating && now - scroll_changed > SAVE_IDLE) {
            save_state();
            scroll_changed = 0;
            dirty = true;       /* the scroll indicator goes */
        }
        if (refresh_url && (int32_t)(now - refresh_at) >= 0 && ld.state == LS_IDLE) {
            char *u = refresh_url;
            refresh_url = NULL;
            go(u, NULL, false, hpos);
            free(u);
        }
        if (toast[0])
            dirty = true;
        if (dirty)
            redraw();
    }
}
