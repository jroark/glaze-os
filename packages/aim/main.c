/*
 * AIM: an AOL Instant Messenger client, as an installable package. AOL's
 * own servers are gone, so it signs on to a server the user names (a
 * revival, or a self-hosted Open OSCAR Server) and speaks OSCAR to it
 * (oscar.c). The session stays signed on in the background after Home.
 *
 * Views: Sign On (and its Server settings), the Buddy List, a
 * conversation, a buddy's info, My Status (away messages, profile,
 * sounds) and Add Buddy. The account, away messages and profile live in
 * <bundle>/data/account, saved as soon as they change.
 */

#include <glaze/fs.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "aim.h"
#include "oscar.h"

#define MARGIN          8
#define CELL_H          30
#define ROW_H           26
#define TOOLBAR_H       34
#define BAR_H           36
#define STRIP_H         18
#define LINE_H          15
#define MAX_AWAY        8
#define AWAY_LEN        128
#define PROFILE_LEN     256
#define DRAFT_MAX       400
#define MAX_CONVS       16
#define LOG_MAX         6144
#define AUTOREPLY_MS    120000
#define DOOR_QUIET_MS   5000    /* no door sounds while the list first fills */
#define SIGNON_MS       45000   /* give up on a server that stops answering */

#define C_ME            0xC8102E    /* AIM's red for you, blue for them */
#define C_THEM          0x1A3FB0
#define C_SYS           0x7A7A7A
#define C_HEADER_TEXT   0x4C566C
#define C_ONLINE        0x34B233
#define C_AWAY          0xE8B100
#define C_IDLE          0xA0A8B0

enum view { V_SIGNON, V_SERVER, V_BUDDIES, V_CHAT, V_INFO, V_STATUS, V_ADD };
enum phase { P_IDLE, P_LOGIN, P_BOS, P_ONLINE };

/* ---- saved state -------------------------------------------------------- */

#define ACCOUNT_VERSION 1

static struct account {
    uint32_t version;
    char sn[OSCAR_SN_MAX];
    char password[64];          /* only with save_password */
    char host[OSCAR_HOST_MAX];
    uint16_t port;
    uint8_t save_password, sounds;
    char profile[PROFILE_LEN];
    char away[MAX_AWAY][AWAY_LEN];
} acct;

static const char *const default_away[] = {
    "I am away from my computer right now.",
    "Out to lunch. Back soon!",
    "In a meeting.",
};

static char password[64];
static char port_text[6];

static void account_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/data/account", res_bundle());
}

static void save_account(void)
{
    char path[FS_PATH_MAX], dir[FS_PATH_MAX];
    struct account copy = acct;

    acct.port = (uint16_t)(atoi(port_text) > 0 && atoi(port_text) < 65536 ? atoi(port_text) : OSCAR_PORT);
    copy.port = acct.port;
    if (acct.save_password)
        strlcpy(copy.password, password, sizeof(copy.password));
    else
        memset(copy.password, 0, sizeof(copy.password));
    account_path(path, sizeof(path));
    if (fs_write_file(path, &copy, sizeof(copy)) == GZ_ENOENT) {
        snprintf(dir, sizeof(dir), "%s/data", res_bundle());
        fs_mkdirs(dir);
        fs_write_file(path, &copy, sizeof(copy));
    }
}

static void load_account(void)
{
    char path[FS_PATH_MAX];

    account_path(path, sizeof(path));
    if (fs_read_file(path, &acct, sizeof(acct)) != sizeof(acct) || acct.version != ACCOUNT_VERSION) {
        memset(&acct, 0, sizeof(acct));
        acct.version = ACCOUNT_VERSION;
        acct.port = OSCAR_PORT;
        acct.sounds = 1;
        for (size_t i = 0; i < sizeof(default_away) / sizeof(default_away[0]); i++)
            strlcpy(acct.away[i], default_away[i], AWAY_LEN);
    }
    /* Always terminated, whatever the file held. */
    acct.sn[sizeof(acct.sn) - 1] = acct.password[sizeof(acct.password) - 1] = 0;
    acct.host[sizeof(acct.host) - 1] = acct.profile[sizeof(acct.profile) - 1] = 0;
    for (int i = 0; i < MAX_AWAY; i++)
        acct.away[i][AWAY_LEN - 1] = 0;
    strlcpy(password, acct.password, sizeof(password));
    snprintf(port_text, sizeof(port_text), "%u", acct.port ? acct.port : OSCAR_PORT);
}

/* ---- session ------------------------------------------------------------ */

static struct oscar o;
static enum phase phase;
static bool conn_up, redirect_pending;
static char progress[64];
static uint32_t signon_ms, online_ms;
static char away_now[AWAY_LEN];     /* empty: available */

/* Deferred from protocol events, which arrive mid-input. */
static bool went_online, went_offline;
static char offline_msg[128];

/* ---- conversations ------------------------------------------------------ */

enum { K_ME, K_THEM, K_ME_AUTO, K_THEM_AUTO, K_SYS, K_NOTE };     /* SYS: about them; NOTE: plain */

struct conv {
    char name[OSCAR_SN_MAX];
    char log[LOG_MAX];          /* entries: kind byte, text, NUL */
    int log_len;
    int unread;
    int typing;                 /* theirs: 0, 1 paused, 2 typing */
    uint32_t typing_ms, autoreply_ms, last_ms;
    char draft[DRAFT_MAX];
    int sent_typing;            /* ours, as last sent */
};

static struct conv convs[MAX_CONVS];
static int nconvs;
static struct conv *cur;

static struct conv *conv_find(const char *name, bool create)
{
    for (int i = 0; i < nconvs; i++)
        if (oscar_same_name(convs[i].name, name))
            return &convs[i];
    if (!create)
        return NULL;
    int at = nconvs;
    if (nconvs == MAX_CONVS) {
        /* Reuse the quietest conversation that isn't open or unread. */
        at = -1;
        for (int i = 0; i < nconvs; i++)
            if (&convs[i] != cur && !convs[i].unread && (at < 0 || convs[i].last_ms < convs[at].last_ms))
                at = i;
        if (at < 0)
            return NULL;
    } else {
        nconvs++;
    }
    memset(&convs[at], 0, sizeof(convs[at]));
    strlcpy(convs[at].name, name, sizeof(convs[at].name));
    return &convs[at];
}

static void sync_badge(void)
{
    int total = 0;
    for (int i = 0; i < nconvs; i++)
        total += convs[i].unread;
    ui_set_badge(total);
}

static void log_add(struct conv *c, int kind, const char *text)
{
    size_t n = strlen(text);

    if (n > LOG_MAX / 2)
        n = LOG_MAX / 2;
    while (c->log_len + (int)n + 2 > LOG_MAX) {
        int first = (int)strlen(c->log + 1) + 2;
        memmove(c->log, c->log + first, (size_t)(c->log_len - first));
        c->log_len -= first;
    }
    c->log[c->log_len++] = (char)kind;
    memcpy(c->log + c->log_len, text, n);
    c->log_len += (int)n;
    c->log[c->log_len++] = 0;
    c->last_ms = gz_time_ms();
}

/* ---- UI state ----------------------------------------------------------- */

static enum view view;
static bool dirty;

struct field {
    char *buf;
    int cap, cursor;
    bool secret, url, digits, caps;
    const char *placeholder;
    gfx_rect r;
};

static struct field f_sn = { acct.sn, OSCAR_SN_MAX, 0, false, false, false, false, "Required", { 0 } };
static struct field f_pw = { password, sizeof(password), 0, true, false, false, false, "Required", { 0 } };
static struct field f_host = { acct.host, OSCAR_HOST_MAX, 0, false, true, false, false, "aim.example.com", { 0 } };
static struct field f_port = { port_text, sizeof(port_text), 0, false, false, true, false, "5190", { 0 } };
static struct field f_draft = { NULL, DRAFT_MAX, 0, false, false, false, true, NULL, { 0 } };
static char new_away[AWAY_LEN];
static struct field f_away = { new_away, AWAY_LEN, 0, false, false, false, true, "New away message", { 0 } };
static struct field f_profile = { acct.profile, PROFILE_LEN, 0, false, false, false, true, "Tell buddies about yourself",
                                  { 0 } };
static char add_name[OSCAR_SN_MAX], add_group[OSCAR_GROUP_NAME];
static struct field f_add_name = { add_name, OSCAR_SN_MAX, 0, false, false, false, false, "Screen Name", { 0 } };
static struct field f_add_group = { add_group, OSCAR_GROUP_NAME, 0, false, false, false, true, "Buddies", { 0 } };

static struct field *focus;
static struct ui_keyboard kb;
static bool in_keyboard;

/* Touch targets, registered while drawing (topmost last). */
enum {
    H_NONE, H_FIELD, H_SIGNON, H_SERVER, H_SAVEPW, H_BACK, H_LEFT, H_RIGHT, H_TOOLBAR, H_SEND, H_STATUS_ROW,
    H_SOUNDS, H_GROUP_PICK, H_DELETE, H_ALERT,
};

struct hit {
    gfx_rect r;
    int id, arg;
    struct field *f;
};

static struct hit hits[48];
static int nhits;
static struct hit pressed;

static void add_hit(gfx_rect r, int id, int arg, struct field *f)
{
    if (nhits < (int)(sizeof(hits) / sizeof(hits[0])))
        hits[nhits++] = (struct hit){ r, id, arg, f };
}

static struct hit hit_at(int x, int y)
{
    for (int i = nhits - 1; i >= 0; i--)
        if (gfx_rect_contains(hits[i].r, x, y))
            return hits[i];
    return (struct hit){ { 0, 0, 0, 0 }, H_NONE, 0, NULL };
}

static bool is_pressed(int id, int arg)
{
    return pressed.id == id && pressed.arg == arg;
}

static bool alert_open;
static char alert_title[32], alert_msg[160];

static void show_alert(const char *title, const char *msg)
{
    strlcpy(alert_title, title, sizeof(alert_title));
    strlcpy(alert_msg, msg, sizeof(alert_msg));
    alert_open = true;
    dirty = true;
}

/* Scroll positions. */
static struct ui_list blist;
static struct ui_scroll chat_sc, info_sc, status_sc;

/* Buddy list rows. */
enum { R_GROUP, R_BUDDY, R_OFFLINE, R_OTHERS, R_OTHER };

static struct row {
    uint8_t type;
    int16_t index;              /* group, buddy or conversation */
} rows[OSCAR_GROUPS + OSCAR_BUDDIES + MAX_CONVS + 2];
static int nrows;
static bool editing;
static int armed = -1;          /* row whose Delete button shows */

#define GID_OFFLINE     0xFFFF
#define GID_OTHERS      0xFFFE
static uint16_t collapsed[OSCAR_GROUPS + 2];
static int ncollapsed;

/* Buddy info. */
static char info_name[OSCAR_SN_MAX];
static char info_text[OSCAR_INFO_MAX * 2 + 64];
static bool info_loaded;

static int kb_height(void)
{
    return focus ? ui_keyboard_height() : 0;
}

static bool show_keyboard(void)
{
    return focus && !ui_has_hardware_keyboard();
}

/* ---- presence and sounds ------------------------------------------------ */

static char was_online[OSCAR_BUDDIES][OSCAR_SN_MAX];
static int nwas;

static bool name_in(char list[][OSCAR_SN_MAX], int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (oscar_same_name(list[i], name))
            return true;
    return false;
}

static void play(int snd)
{
    if (acct.sounds)
        sound_play(snd);
}

/* Door sounds and conversation notes for buddies who came or went. */
static void presence_changed(void)
{
    static char now_online[OSCAR_BUDDIES][OSCAR_SN_MAX];
    int n = 0, opened = 0, closed = 0;
    bool quiet = gz_time_ms() - online_ms < DOOR_QUIET_MS;

    for (int i = 0; i < o.nbuddies; i++)
        if (o.buddies[i].online && !name_in(now_online, n, o.buddies[i].name))
            strlcpy(now_online[n++], o.buddies[i].name, OSCAR_SN_MAX);
    for (int i = 0; i < n; i++)
        if (!name_in(was_online, nwas, now_online[i])) {
            struct conv *c = conv_find(now_online[i], false);
            if (c && !quiet)
                log_add(c, K_SYS, "signed on.");
            opened++;
        }
    for (int i = 0; i < nwas; i++)
        if (!name_in(now_online, n, was_online[i])) {
            struct conv *c = conv_find(was_online[i], false);
            if (c) {
                log_add(c, K_SYS, "signed off.");
                c->typing = 0;
            }
            closed++;
        }
    memcpy(was_online, now_online, (size_t)n * OSCAR_SN_MAX);
    nwas = n;
    if (!quiet && opened)
        play(SND_DOOR_OPEN);
    else if (!quiet && closed)
        play(SND_DOOR_SLAM);
}

/* ---- buddy list rows ---------------------------------------------------- */

static bool is_collapsed(unsigned gid)
{
    for (int i = 0; i < ncollapsed; i++)
        if (collapsed[i] == gid)
            return true;
    return false;
}

static void toggle_collapsed(unsigned gid)
{
    for (int i = 0; i < ncollapsed; i++)
        if (collapsed[i] == gid) {
            collapsed[i] = collapsed[--ncollapsed];
            return;
        }
    if (ncollapsed < (int)(sizeof(collapsed) / sizeof(collapsed[0])))
        collapsed[ncollapsed++] = (uint16_t)gid;
}

static void add_row(int type, int index)
{
    if (nrows < (int)(sizeof(rows) / sizeof(rows[0])))
        rows[nrows++] = (struct row){ (uint8_t)type, (int16_t)index };
}

/* Online buddies by group, then everyone offline once each, with people
 * not on the list who have sent a message at the top. Edit mode lists the
 * groups as they are. */
static void build_rows(void)
{
    nrows = 0;
    if (!editing) {
        int others = 0;
        for (int i = 0; i < nconvs; i++)
            if (!oscar_find_buddy(&o, convs[i].name))
                others++;
        if (others) {
            add_row(R_OTHERS, 0);
            if (!is_collapsed(GID_OTHERS))
                for (int i = 0; i < nconvs; i++)
                    if (!oscar_find_buddy(&o, convs[i].name))
                        add_row(R_OTHER, i);
        }
    }
    for (int g = 0; g < o.ngroups; g++) {
        add_row(R_GROUP, g);
        if (!editing && is_collapsed(o.groups[g].gid))
            continue;
        for (int i = 0; i < o.nbuddies; i++)
            if (o.buddies[i].gid == o.groups[g].gid && (editing || o.buddies[i].online))
                add_row(R_BUDDY, i);
    }
    if (!editing) {
        add_row(R_OFFLINE, 0);
        if (!is_collapsed(GID_OFFLINE))
            for (int i = 0; i < o.nbuddies; i++) {
                bool dup = false;
                for (int j = 0; j < i; j++)
                    if (oscar_same_name(o.buddies[j].name, o.buddies[i].name))
                        dup = true;
                if (!o.buddies[i].online && !dup)
                    add_row(R_BUDDY, i);
            }
    }
    if (armed >= nrows)
        armed = -1;
    ui_list_set_count(&blist, nrows);
}

/* ---- text fields -------------------------------------------------------- */

static void set_focus(struct field *f)
{
    focus = f;
    if (!f)
        return;
    ui_keyboard_init(&kb);
    kb.url = f->url;
    kb.numbers = f->digits;
    kb.shift = f->caps && !f->buf[0];
    f->cursor = (int)strlen(f->buf);
}

static void draw_field(gfx_surface *s, struct field *f, gfx_rect r, const gfx_font *font, uint32_t rgb)
{
    char shown[DRAFT_MAX];
    int n = (int)strlen(f->buf), start = 0;
    bool focused = focus == f;

    f->r = r;
    add_hit(r, H_FIELD, 0, f);
    if (f->secret) {
        n = n < (int)sizeof(shown) - 1 ? n : (int)sizeof(shown) - 1;
        memset(shown, GFX_CH_BULLET[0], (size_t)n);
        shown[n] = 0;
    } else {
        strlcpy(shown, f->buf, sizeof(shown));
    }
    int cursor = f->cursor > n ? n : f->cursor;
    while (start < cursor && gfx_text_width_n(font, shown + start, cursor - start) > r.w - 3)
        start++;
    int ty = r.y + (r.h - font->line_height) / 2;
    gfx_rect saved = s->clip;
    gfx_set_clip(s, r);
    if (!n && f->placeholder)
        gfx_text(s, r.x, ty, font, f->placeholder, 0xB4B4B4, 255);
    else
        gfx_text(s, r.x, ty, font, shown + start, rgb, 255);
    if (focused) {
        int cx = r.x + gfx_text_width_n(font, shown + start, cursor - start);
        gfx_fill(s, gfx_r(cx, ty + 1, 2, font->line_height - 2), 0x3B6FD8);
    }
    s->clip = saved;
}

/* ---- drawing helpers ---------------------------------------------------- */

static void section_header(gfx_surface *s, int y, const char *text)
{
    const gfx_font *f = ui_font(FONT_B13);
    gfx_text_shadow(s, 20, y, f, text, C_HEADER_TEXT, 0xFFFFFF, 1, 255);
}

static void footer_text(gfx_surface *s, int y, const char *text)
{
    const gfx_font *f = ui_font(FONT_R12);
    int starts[8], n = ui_wrap(f, text, UI_W - 40, starts, 8);

    for (int i = 0; i < n; i++)
        gfx_text_n(s, 20, y + i * (f->line_height + 1), f, text + starts[i], ui_line_len(text, starts, n, i),
                   C_HEADER_TEXT, 255);
}

static gfx_rect group_row(int y, int i)
{
    return gfx_r(10, y + i * CELL_H, UI_W - 20, CELL_H);
}

static void cell_label(gfx_surface *s, gfx_rect r, const char *label, uint32_t rgb)
{
    const gfx_font *f = ui_font(FONT_B14);
    gfx_text(s, r.x + 10, r.y + (r.h - f->line_height) / 2, f, label, rgb, 255);
}

/* A labelled text field cell. */
static void field_cell(gfx_surface *s, gfx_rect r, int pos, const char *label, struct field *f)
{
    /* One column for every label: the widest is "Screen Name". */
    int lw = label ? gfx_text_width(ui_font(FONT_B14), "Screen Name") + 18 : 10;

    ui_draw_group_cell(s, r, pos, false);
    if (label)
        cell_label(s, r, label, UI_TEXT);
    draw_field(s, f, gfx_r(r.x + lw, r.y + 2, r.w - lw - 8, r.h - 4), ui_font(FONT_R14), UI_TEXT_DETAIL);
    add_hit(r, H_FIELD, 0, f);
}

static void status_dot(gfx_surface *s, int cx, int cy, const struct oscar_buddy *b)
{
    if (!b || !b->online)
        return;
    uint32_t rgb = b->away ? C_AWAY : oscar_idle_minutes(&o, b) ? C_IDLE : C_ONLINE;
    gfx_circle(s, cx, cy, 5, 0x000000, 60);
    gfx_circle(s, cx, cy, 4, rgb, 255);
    gfx_circle(s, cx - 1, cy - 1, 1, 0xFFFFFF, 140);
}

static void unread_badge(gfx_surface *s, int right, int cy, int n)
{
    const gfx_font *f = ui_font(FONT_B11);
    char t[8];

    snprintf(t, sizeof(t), "%d", n > 99 ? 99 : n);
    int w = gfx_text_width(f, t) + 12;
    if (w < 20)
        w = 20;
    gfx_round_rect(s, gfx_r(right - w, cy - 9, w, 18), 9, 0x2F6CE0, 255);
    gfx_text_center(s, right - w, cy - f->line_height / 2, w, f, t, 0xFFFFFF, 255);
}

static void idle_text(char *buf, size_t n, int minutes)
{
    if (minutes >= 60)
        snprintf(buf, n, "%dh%02d", minutes / 60, minutes % 60);
    else
        snprintf(buf, n, "%dm", minutes);
}

/* ---- sign-on view ------------------------------------------------------- */

static void draw_signon(gfx_surface *s)
{
    bool kbd = show_keyboard();
    int y = UI_NAV_H + 10;
    const gfx_font *f = ui_font(FONT_R13);

    ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H));
    if (!kbd) {
        gfx_image icon;
        if (res_image(&icon, "icon.gzi"))
            gfx_draw_image(s, (UI_W - icon.w) / 2, y, &icon, 255);
        y += 52;
    }
    field_cell(s, group_row(y, 0), 1, "Screen Name", &f_sn);
    field_cell(s, group_row(y, 1), 3, "Password", &f_pw);
    y += 2 * CELL_H + 10;

    gfx_rect r = group_row(y, 0);
    ui_draw_group_cell(s, r, 1, false);
    cell_label(s, r, "Save Password", UI_TEXT);
    ui_draw_switch(s, r.x + r.w - UI_SWITCH_W - 8, r.y + (r.h - UI_SWITCH_H) / 2, acct.save_password);
    add_hit(r, H_SAVEPW, 0, NULL);
    r = group_row(y, 1);
    ui_draw_group_cell(s, r, 3, is_pressed(H_SERVER, 0));
    cell_label(s, r, "Server", is_pressed(H_SERVER, 0) ? 0xFFFFFF : UI_TEXT);
    int lw = gfx_text_width(ui_font(FONT_B14), "Screen Name") + 18;
    gfx_text_fit(s, r.x + lw, r.y + (r.h - f->line_height) / 2, r.w - lw - 22, f, acct.host[0] ? acct.host : "Not set",
                 is_pressed(H_SERVER, 0) ? 0xFFFFFF : UI_TEXT_DETAIL, 255);
    ui_draw_chevron(s, r.x + r.w - 16, r.y + r.h / 2 - 5, is_pressed(H_SERVER, 0) ? 0xFFFFFF : 0x8A8A8A);
    add_hit(r, H_SERVER, 0, NULL);
    y += 2 * CELL_H + 12;

    r = gfx_r(10, y, UI_W - 20, 36);
    ui_draw_button(s, r, phase == P_IDLE ? "Sign On" : "Cancel", is_pressed(H_SIGNON, 0));
    add_hit(r, H_SIGNON, 0, NULL);
    if (phase != P_IDLE)
        gfx_text_center(s, 0, y + 44, UI_W, f, progress, C_HEADER_TEXT, 255);

    ui_draw_navbar(s, "AIM", NULL, NULL, 0);
}

static void draw_server(gfx_surface *s)
{
    int y = UI_NAV_H + 14;
    gfx_rect back;

    ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H));
    field_cell(s, group_row(y, 0), 1, "Host", &f_host);
    field_cell(s, group_row(y, 1), 3, "Port", &f_port);
    if (!show_keyboard())
        footer_text(s, y + 2 * CELL_H + 10,
                    "AOL's servers closed in 2017. Enter the address of an AIM revival server, or of your own "
                    "Open OSCAR Server. The usual port is 5190.");
    ui_draw_navbar(s, "Server", "AIM", &back, 0);
    add_hit(back, H_BACK, 0, NULL);
}

/* ---- buddy list view ---------------------------------------------------- */

static gfx_rect delete_rect;

static void draw_row(gfx_surface *s, int i, gfx_rect r, bool selected, void *ctx)
{
    const struct row *row = &rows[i];
    const gfx_font *bf = ui_font(FONT_B14), *rf = ui_font(FONT_R14), *sf = ui_font(FONT_R11);
    char t[64];
    (void)ctx;

    if (row->type == R_GROUP || row->type == R_OFFLINE || row->type == R_OTHERS) {
        unsigned gid = row->type == R_OFFLINE ? GID_OFFLINE : row->type == R_OTHERS ? GID_OTHERS
                                                                                   : o.groups[row->index].gid;
        int on = 0, all = 0;
        if (row->type == R_GROUP) {
            for (int b = 0; b < o.nbuddies; b++)
                if (o.buddies[b].gid == gid) {
                    all++;
                    on += o.buddies[b].online;
                }
            snprintf(t, sizeof(t), "%s (%d/%d)", o.groups[row->index].name, on, all);
        } else if (row->type == R_OFFLINE) {
            for (int b = 0; b < o.nbuddies; b++)
                all += !o.buddies[b].online;
            snprintf(t, sizeof(t), "Offline (%d)", all);
        } else {
            strlcpy(t, "Not on Buddy List", sizeof(t));
        }
        gfx_vgradient(s, r, 0xE6EAF0, 0xCDD4DE, 255);
        gfx_fill(s, gfx_r(r.x, r.y + r.h - 1, r.w, 1), 0xA9B1BD);
        int x = r.x + MARGIN;
        if (editing) {
            ui_draw_delete_badge(s, x + 10, r.y + r.h / 2, armed == i);
            x += UI_DELETE_BADGE_W;
        } else {
            /* Disclosure triangle. */
            int cx = x + 4, cy = r.y + r.h / 2;
            if (is_collapsed(gid))
                for (int k = 0; k < 5; k++)
                    gfx_fill(s, gfx_r(cx - 2 + k, cy - 4 + k, 1, 9 - 2 * k), C_HEADER_TEXT);
            else
                for (int k = 0; k < 5; k++)
                    gfx_fill(s, gfx_r(cx - 4 + k, cy - 2 + k, 9 - 2 * k, 1), C_HEADER_TEXT);
            x += 14;
        }
        gfx_text_fit(s, x, r.y + (r.h - ui_font(FONT_B12)->line_height) / 2, r.w - x - 70, ui_font(FONT_B12), t,
                     C_HEADER_TEXT, 255);
        if (editing && armed == i)
            delete_rect = ui_draw_delete_button(s, r.x + r.w - 8, r.y + r.h / 2, is_pressed(H_DELETE, i));
        return;
    }

    const struct oscar_buddy *b = row->type == R_BUDDY ? &o.buddies[row->index] : NULL;
    const char *name = b ? b->name : convs[row->index].name;
    struct conv *c = conv_find(name, false);
    ui_draw_list_row(s, r, selected);
    int x = r.x + MARGIN;
    if (editing) {
        ui_draw_delete_badge(s, x + 10, r.y + r.h / 2, armed == i);
        x += UI_DELETE_BADGE_W;
    }
    status_dot(s, x + 6, r.y + r.h / 2, b);
    x += 16;
    bool online = b && b->online;
    uint32_t rgb = selected ? 0xFFFFFF : online || !b ? UI_TEXT : 0x8C8C8C;
    int right = r.x + r.w - 10;
    if (editing && armed == i) {
        delete_rect = ui_draw_delete_button(s, right + 2, r.y + r.h / 2, is_pressed(H_DELETE, i));
        right = delete_rect.x - 6;
    } else {
        if (c && c->unread) {
            unread_badge(s, right, r.y + r.h / 2, c->unread);
            right -= 30;
        }
        t[0] = 0;
        if (online && b->away)
            strlcpy(t, "Away", sizeof(t));
        else if (online && oscar_idle_minutes(&o, b))
            idle_text(t, sizeof(t), oscar_idle_minutes(&o, b));
        if (t[0]) {
            int tw = gfx_text_width(sf, t);
            gfx_text(s, right - tw, r.y + (r.h - sf->line_height) / 2 + 1, sf, t,
                     selected ? 0xFFFFFF : 0x8C8C8C, 255);
            right -= tw + 6;
        }
    }
    gfx_text_fit(s, x, r.y + (r.h - bf->line_height) / 2, right - x, online ? bf : rf, name, rgb, 255);
}

static void draw_buddies(gfx_surface *s)
{
    gfx_rect area = blist.area, r;
    const gfx_font *f = ui_font(FONT_B13);

    gfx_set_clip(s, area);
    gfx_fill(s, area, 0xFFFFFF);
    delete_rect = gfx_r(0, 0, 0, 0);
    ui_list_draw(s, &blist, draw_row, NULL);
    if (!nrows || (!o.nbuddies && !editing && !nconvs))
        gfx_text_center(s, 0, area.y + 80, UI_W, ui_font(FONT_R14),
                        o.feedbag ? "Tap Edit, then + to add a buddy." : "This server keeps no buddy list.",
                        0x8C8C8C, 255);
    gfx_reset_clip(s);
    if (!gfx_rect_empty(delete_rect))
        add_hit(delete_rect, H_DELETE, armed, NULL);

    /* Toolbar: your status. */
    r = gfx_r(0, UI_H - TOOLBAR_H, UI_W, TOOLBAR_H);
    gfx_vgradient(s, r, is_pressed(H_TOOLBAR, 0) ? 0x7D8DA5 : 0xB0BCCD, is_pressed(H_TOOLBAR, 0) ? 0x4E6584 : 0x6D84A2,
                  255);
    gfx_fill(s, gfx_r(0, r.y, UI_W, 1), UI_NAV_LINE);
    int cy = r.y + r.h / 2;
    gfx_circle(s, 16, cy, 6, 0x000000, 80);
    gfx_circle(s, 16, cy, 5, away_now[0] ? C_AWAY : C_ONLINE, 255);
    char t[AWAY_LEN + 8];
    snprintf(t, sizeof(t), "%s", away_now[0] ? away_now : "Available");
    gfx_text_fit(s, 28, cy - f->line_height / 2, UI_W - 28 - 26, f, t, 0xFFFFFF, 255);
    ui_draw_chevron(s, UI_W - 16, cy - 5, 0xFFFFFF);
    add_hit(r, H_TOOLBAR, 0, NULL);

    ui_draw_navbar(s, "Buddy List", NULL, NULL, 0);
    add_hit(ui_draw_bar_button(s, editing ? "+" : "Sign Off", false, is_pressed(H_LEFT, 0), false, 0), H_LEFT, 0, NULL);
    if (o.feedbag)
        add_hit(ui_draw_bar_button(s, editing ? "Done" : "Edit", true, is_pressed(H_RIGHT, 0), editing, 0), H_RIGHT, 0,
                NULL);
}

/* ---- conversation view -------------------------------------------------- */

#define MAX_LINES 700

static struct line {
    uint16_t off, len;
    uint8_t kind, prefix;
} lines[MAX_LINES];
static int nlines;
static char disp[LOG_MAX * 2];

static int bar_y(void)
{
    return UI_H - kb_height() - BAR_H;
}

static gfx_rect transcript_area(void)
{
    int top = UI_NAV_H + STRIP_H;
    return gfx_r(0, top, UI_W, bar_y() - top);
}

/* Wraps cur's log into lines, newest last; drops the oldest that don't fit. */
static void chat_layout(void)
{
    const gfx_font *f = ui_font(FONT_R12);
    int width = UI_W - 2 * MARGIN - 8;
    static int starts[200];
    size_t used = 0;

    nlines = 0;
    if (!cur)
        return;
    for (int off = 0; off < cur->log_len;) {
        int kind = (unsigned char)cur->log[off];
        const char *text = cur->log + off + 1;
        off += (int)strlen(text) + 2;
        char prefix[OSCAR_SN_MAX + 16] = "";
        if (kind == K_ME || kind == K_ME_AUTO)
            snprintf(prefix, sizeof(prefix), "%s%s: ", o.sn[0] ? o.sn : acct.sn, kind == K_ME_AUTO ? " (auto)" : "");
        else if (kind == K_THEM || kind == K_THEM_AUTO)
            snprintf(prefix, sizeof(prefix), "%s%s: ", cur->name, kind == K_THEM_AUTO ? " (auto)" : "");
        else if (kind == K_SYS)
            snprintf(prefix, sizeof(prefix), "%s ", cur->name);
        size_t need = strlen(prefix) + strlen(text) + 1;
        if (used + need > sizeof(disp)) {
            /* Start over from a later entry: keep the newest. */
            used = 0;
            nlines = 0;
            if (need > sizeof(disp))
                continue;
        }
        char *line = disp + used;
        snprintf(line, need, "%s%s", prefix, text);
        int n = ui_wrap(f, line, width, starts, 200);
        for (int i = 0; i < n; i++) {
            if (nlines == MAX_LINES) {
                memmove(lines, lines + 100, (MAX_LINES - 100) * sizeof(lines[0]));
                nlines -= 100;
            }
            int len = ui_line_len(line, starts, n, i);
            int plen = i == 0 ? (int)strlen(prefix) : 0;
            lines[nlines++] = (struct line){ (uint16_t)(used + (size_t)starts[i]), (uint16_t)len, (uint8_t)kind,
                                             (uint8_t)(plen < len ? plen : len) };
        }
        used += need;
    }
}

static void chat_relayout(bool to_bottom)
{
    gfx_rect area = transcript_area();
    bool at_bottom = chat_sc.offset >= ui_scroll_max(&chat_sc) - 4;

    chat_layout();
    chat_sc.content_h = nlines * LINE_H + 8;
    chat_sc.view_h = area.h;
    if (to_bottom || at_bottom)
        chat_sc.offset = ui_scroll_max(&chat_sc);
    if (chat_sc.offset > ui_scroll_max(&chat_sc))
        chat_sc.offset = ui_scroll_max(&chat_sc);
    if (chat_sc.offset < 0)
        chat_sc.offset = 0;
}

static void draw_chat(gfx_surface *s)
{
    gfx_rect area = transcript_area(), back;
    const gfx_font *rf = ui_font(FONT_R12), *bf = ui_font(FONT_B12), *itf = ui_font(FONT_I12);
    const struct oscar_buddy *b = oscar_find_buddy(&o, cur->name);
    char t[96];

    gfx_fill(s, area, 0xFFFFFF);
    gfx_set_clip(s, area);
    for (int i = 0; i < nlines; i++) {
        int y = area.y + 4 + i * LINE_H - chat_sc.offset;
        if (y + LINE_H < area.y || y > area.y + area.h)
            continue;
        const struct line *l = &lines[i];
        const char *p = disp + l->off;
        int x = MARGIN;
        if (l->kind == K_SYS || l->kind == K_NOTE) {
            gfx_text_n(s, x, y, itf, p, l->len, C_SYS, 255);
            continue;
        }
        if (l->prefix) {
            x += gfx_text_n(s, x, y, bf, p, l->prefix, l->kind == K_ME || l->kind == K_ME_AUTO ? C_ME : C_THEM, 255);
        }
        gfx_text_n(s, x, y, rf, p + l->prefix, l->len - l->prefix, UI_TEXT, 255);
    }
    if (!nlines)
        gfx_text_center(s, 0, area.y + 40, UI_W, ui_font(FONT_R13), "No messages yet.", 0xA0A0A0, 255);
    gfx_reset_clip(s);

    /* Status strip. */
    gfx_rect strip = gfx_r(0, UI_NAV_H, UI_W, STRIP_H);
    gfx_vgradient(s, strip, 0xF2F4F7, 0xE0E4EA, 255);
    gfx_fill(s, gfx_r(0, strip.y + strip.h - 1, UI_W, 1), 0xB8BEC8);
    if (cur->typing == 2)
        snprintf(t, sizeof(t), "%s is typing" GFX_CH_ELLIPSIS, cur->name);
    else if (cur->typing == 1)
        snprintf(t, sizeof(t), "%s has entered text", cur->name);
    else if (phase != P_ONLINE)
        strlcpy(t, "You are signed off", sizeof(t));
    else if (!b)
        snprintf(t, sizeof(t), "%s isn't on your Buddy List", cur->name);
    else if (!b->online)
        snprintf(t, sizeof(t), "%s is offline", cur->name);
    else if (b->away)
        snprintf(t, sizeof(t), "%s is away", cur->name);
    else if (oscar_idle_minutes(&o, b))
        snprintf(t, sizeof(t), "%s has been idle %d min", cur->name, oscar_idle_minutes(&o, b));
    else
        snprintf(t, sizeof(t), "%s is online", cur->name);
    status_dot(s, 12, strip.y + strip.h / 2, b);
    gfx_text_fit(s, 22, strip.y + (strip.h - ui_font(FONT_R11)->line_height) / 2, UI_W - 30, ui_font(FONT_R11), t,
                 C_HEADER_TEXT, 255);

    /* Message bar. */
    int y = bar_y();
    gfx_rect bar = gfx_r(0, y, UI_W, BAR_H);
    gfx_vgradient(s, bar, 0xDCE0E6, 0xB9C0CB, 255);
    gfx_fill(s, gfx_r(0, y, UI_W, 1), 0x8E96A3);
    gfx_rect box = gfx_r(6, y + 6, UI_W - 70, BAR_H - 12);
    gfx_round_rect(s, box, 11, 0x8E96A3, 255);
    gfx_round_rect(s, gfx_r(box.x + 1, box.y + 1, box.w - 2, box.h - 2), 10, 0xFFFFFF, 255);
    f_draft.placeholder = phase == P_ONLINE ? NULL : "Signed off";
    draw_field(s, &f_draft, gfx_r(box.x + 9, box.y + 2, box.w - 16, box.h - 4), ui_font(FONT_R13), UI_TEXT);
    add_hit(box, H_FIELD, 0, &f_draft);
    gfx_rect send = gfx_r(UI_W - 58, y + 5, 52, BAR_H - 10);
    bool can = cur->draft[0] && phase == P_ONLINE;
    gfx_round_rect_vgradient(s, send, 6, can ? 0x5E95F2 : 0xC4CAD3, can ? 0x2A63D4 : 0xA9B0BC,
                             is_pressed(H_SEND, 0) ? 170 : 255);
    gfx_text_center(s, send.x, send.y + (send.h - ui_font(FONT_B13)->line_height) / 2, send.w, ui_font(FONT_B13), "Send",
                    0xFFFFFF, 255);
    add_hit(send, H_SEND, 0, NULL);

    if (show_keyboard())
        ui_draw_keyboard(s, &kb, UI_H - UI_KB_H);

    ui_draw_navbar(s, cur->name, "Buddies", &back, 0);
    add_hit(back, H_BACK, 0, NULL);
    if (phase == P_ONLINE)
        add_hit(ui_draw_bar_button(s, "Info", true, is_pressed(H_RIGHT, 0), false, 0), H_RIGHT, 0, NULL);
}

/* ---- buddy info view ---------------------------------------------------- */

static int info_starts[400];
static int info_nlines;

static void info_layout(void)
{
    info_nlines = ui_wrap(ui_font(FONT_R13), info_text, UI_W - 2 * MARGIN - 8, info_starts, 400);
    info_sc.content_h = info_nlines * 16 + 16;
    info_sc.view_h = UI_H - UI_NAV_H;
}

static void set_info(const char *profile, const char *away)
{
    snprintf(info_text, sizeof(info_text), "%s%s%s%s%s", away[0] ? "Away message:\n" : "", away,
             away[0] ? "\n\n" : "", profile[0] ? "Profile:\n" : "", profile[0] ? profile : away[0] ? "" :
             "No profile.");
    info_loaded = true;
    info_layout();
}

static void draw_info(gfx_surface *s)
{
    gfx_rect area = gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H), back;
    const gfx_font *f = ui_font(FONT_R13), *bf = ui_font(FONT_B13);

    gfx_fill(s, area, 0xFFFFFF);
    gfx_set_clip(s, area);
    if (!info_loaded) {
        gfx_text_center(s, 0, area.y + 40, UI_W, f, "Loading" GFX_CH_ELLIPSIS, 0x8C8C8C, 255);
    } else {
        for (int i = 0; i < info_nlines; i++) {
            int y = area.y + 8 + i * 16 - info_sc.offset;
            const char *p = info_text + info_starts[i];
            int n = ui_line_len(info_text, info_starts, info_nlines, i);
            bool heading = (n == 13 && !strncmp(p, "Away message:", 13)) || (n == 8 && !strncmp(p, "Profile:", 8));
            gfx_text_n(s, MARGIN + 4, y, heading ? bf : f, p, n, heading ? C_HEADER_TEXT : UI_TEXT, 255);
        }
    }
    gfx_reset_clip(s);
    ui_draw_navbar(s, info_name, "Chat", &back, 0);
    add_hit(back, H_BACK, 0, NULL);
}

/* ---- my status view ----------------------------------------------------- */

static int naway(void)
{
    int n = 0;
    while (n < MAX_AWAY && acct.away[n][0])
        n++;
    return n;
}

static int status_content_h(void)
{
    return 8 + 22 + (1 + naway()) * CELL_H + 12 + 22 + CELL_H + 12 + 22 + CELL_H + 12 + CELL_H + 16 + kb_height();
}

static void draw_status(gfx_surface *s)
{
    gfx_rect area = gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H), back;
    const gfx_font *f = ui_font(FONT_R14);
    int n = naway(), y = area.y + 8 - status_sc.offset;

    status_sc.content_h = status_content_h();
    status_sc.view_h = area.h;
    gfx_set_clip(s, area);
    ui_draw_pinstripes(s, area);

    section_header(s, y, "Status");
    y += 22;
    for (int i = 0; i <= n; i++) {
        gfx_rect r = group_row(y, i);
        bool sel = is_pressed(H_STATUS_ROW, i);
        bool current = i == 0 ? !away_now[0] : !strcmp(away_now, acct.away[i - 1]);
        ui_draw_group_cell(s, r, ui_group_pos(i, n + 1), sel);
        const char *label = i == 0 ? "Available" : acct.away[i - 1];
        gfx_text_fit(s, r.x + 10, r.y + (r.h - f->line_height) / 2, r.w - 40, i == 0 ? ui_font(FONT_B14) : f, label,
                     sel ? 0xFFFFFF : current ? UI_TEXT_DETAIL : UI_TEXT, 255);
        if (current)
            gfx_text(s, r.x + r.w - 22, r.y + (r.h - f->line_height) / 2, ui_font(FONT_B14), GFX_CH_CHECK,
                     sel ? 0xFFFFFF : UI_TEXT_DETAIL, 255);
        add_hit(r, H_STATUS_ROW, i, NULL);
    }
    y += (n + 1) * CELL_H + 12;

    section_header(s, y, "New Away Message");
    y += 22;
    field_cell(s, group_row(y, 0), 0, NULL, &f_away);
    y += CELL_H + 12;

    section_header(s, y, "Profile");
    y += 22;
    field_cell(s, group_row(y, 0), 0, NULL, &f_profile);
    y += CELL_H + 12;

    gfx_rect r = group_row(y, 0);
    ui_draw_group_cell(s, r, 0, false);
    cell_label(s, r, "Sounds", UI_TEXT);
    ui_draw_switch(s, r.x + r.w - UI_SWITCH_W - 8, r.y + (r.h - UI_SWITCH_H) / 2, acct.sounds);
    add_hit(r, H_SOUNDS, 0, NULL);
    gfx_reset_clip(s);

    if (show_keyboard())
        ui_draw_keyboard(s, &kb, UI_H - UI_KB_H);
    ui_draw_navbar(s, "My Status", "Buddies", &back, 0);
    add_hit(gfx_r(0, 0, UI_W, UI_NAV_H), H_NONE, 0, NULL);     /* over rows scrolled beneath */
    add_hit(back, H_BACK, 0, NULL);
}

/* Scrolls the status view so the focused field sits above the keyboard. */
static void status_reveal(void)
{
    if (!focus)
        return;
    int view_h = UI_H - UI_NAV_H - kb_height();
    int y = focus->r.y - UI_NAV_H + status_sc.offset;
    status_sc.content_h = status_content_h();
    if (y + CELL_H + 8 - status_sc.offset > view_h)
        status_sc.offset = y + CELL_H + 8 - view_h;
    if (status_sc.offset > ui_scroll_max(&status_sc))
        status_sc.offset = ui_scroll_max(&status_sc);
}

/* ---- add buddy view ----------------------------------------------------- */

static void draw_add(gfx_surface *s)
{
    int y = UI_NAV_H + 14;
    const gfx_font *f = ui_font(FONT_R14);

    ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H));
    field_cell(s, group_row(y, 0), 1, "Screen Name", &f_add_name);
    field_cell(s, group_row(y, 1), 3, "Group", &f_add_group);
    y += 2 * CELL_H + 12;
    if (!show_keyboard() && o.ngroups) {
        section_header(s, y, "Groups");
        y += 22;
        int n = o.ngroups < 4 ? o.ngroups : 4;
        for (int i = 0; i < n; i++) {
            gfx_rect r = group_row(y, i);
            bool sel = is_pressed(H_GROUP_PICK, i);
            ui_draw_group_cell(s, r, ui_group_pos(i, n), sel);
            gfx_text_fit(s, r.x + 10, r.y + (r.h - f->line_height) / 2, r.w - 40, f, o.groups[i].name,
                         sel ? 0xFFFFFF : UI_TEXT, 255);
            if (!strcasecmp(add_group, o.groups[i].name))
                gfx_text(s, r.x + r.w - 22, r.y + (r.h - f->line_height) / 2, ui_font(FONT_B14), GFX_CH_CHECK,
                         sel ? 0xFFFFFF : UI_TEXT_DETAIL, 255);
            add_hit(r, H_GROUP_PICK, i, NULL);
        }
    }
    if (show_keyboard())
        ui_draw_keyboard(s, &kb, UI_H - UI_KB_H);
    ui_draw_navbar(s, "Add Buddy", NULL, NULL, 0);
    add_hit(ui_draw_bar_button(s, "Cancel", false, is_pressed(H_LEFT, 0), false, 0), H_LEFT, 0, NULL);
    add_hit(ui_draw_bar_button(s, "Add", true, is_pressed(H_RIGHT, 0), true, 0), H_RIGHT, 0, NULL);
}

/* ---- frame -------------------------------------------------------------- */

static void draw_view(gfx_surface *s)
{
    nhits = 0;
    switch (view) {
    case V_SIGNON: draw_signon(s); break;
    case V_SERVER: draw_server(s); break;
    case V_BUDDIES: draw_buddies(s); break;
    case V_CHAT: draw_chat(s); break;
    case V_INFO: draw_info(s); break;
    case V_STATUS: draw_status(s); break;
    case V_ADD: draw_add(s); break;
    }
    if (show_keyboard() && (view == V_SIGNON || view == V_SERVER))
        ui_draw_keyboard(s, &kb, UI_H - UI_KB_H);
    if (alert_open) {
        gfx_rect r1, r2;
        ui_draw_alert(s, alert_title, alert_msg, "OK", NULL, &r1, &r2, is_pressed(H_ALERT, 0) ? 0 : -1);
        nhits = 0;
        add_hit(r1, H_ALERT, 0, NULL);
    }
}

static void redraw(void)
{
    draw_view(&ui.surf);
    ui_invalidate_all();
    ui_present();
    dirty = false;
}

static void go(enum view v, bool forward)
{
    view = v;
    pressed.id = H_NONE;
    armed = -1;
    ui_transition(draw_view, forward);
    dirty = false;
}

/* ---- actions ------------------------------------------------------------ */

static void reset_session(void)
{
    conn_close();
    phase = P_IDLE;
    conn_up = redirect_pending = false;
    o.state = OSCAR_IDLE;
    away_now[0] = 0;
    nwas = 0;
}

static void sign_off(void)
{
    reset_session();
    nconvs = 0;
    cur = NULL;
    editing = false;
    sync_badge();
    set_focus(NULL);
    go(V_SIGNON, false);
}

static void on_event(void *ctx, const struct oscar_event *ev);
static int net_send_hook(void *ctx, const void *buf, size_t len);

static void sign_on(void)
{
    if (phase != P_IDLE) {
        reset_session();
        dirty = true;
        return;
    }
    set_focus(NULL);
    char norm[OSCAR_SN_MAX];
    oscar_norm(norm, sizeof(norm), acct.sn);
    if (!norm[0]) {
        show_alert("Sign On", "Enter your screen name.");
        return;
    }
    if (!password[0]) {
        show_alert("Sign On", "Enter your password.");
        return;
    }
    if (!acct.host[0]) {
        show_alert("No Server", "Choose a server first:\ntap Server and enter its address.");
        return;
    }
    save_account();
    oscar_init(&o, &(struct oscar_ops){ NULL, net_send_hook, on_event });
    oscar_tick(&o, gz_time_ms());
    oscar_set_profile(&o, acct.profile);
    strlcpy(progress, "Connecting" GFX_CH_ELLIPSIS, sizeof(progress));
    phase = P_LOGIN;
    conn_up = false;
    signon_ms = gz_time_ms();
    conn_start(acct.host, acct.port);
    dirty = true;
}

static void set_away(const char *text)
{
    strlcpy(away_now, text ? text : "", sizeof(away_now));
    oscar_set_away(&o, away_now);
    for (int i = 0; i < nconvs; i++)
        convs[i].autoreply_ms = 0;
}

/* Puts a new away message first in the saved list and goes away with it. */
static void add_away(const char *text)
{
    char copy[AWAY_LEN];
    int at = MAX_AWAY - 1;

    if (!text[0])
        return;
    strlcpy(copy, text, sizeof(copy));
    for (int i = 0; i < MAX_AWAY; i++)
        if (!strcmp(acct.away[i], copy)) {
            at = i;
            break;
        }
    memmove(acct.away[1], acct.away[0], (size_t)at * AWAY_LEN);
    strlcpy(acct.away[0], copy, AWAY_LEN);
    save_account();
    set_away(copy);
}

static void send_typing(struct conv *c, int state)
{
    if (c && c->sent_typing != state && phase == P_ONLINE) {
        oscar_send_typing(&o, c->name, state);
        c->sent_typing = state;
    }
}

static void send_draft(void)
{
    char *p = cur->draft;

    while (*p == ' ')
        p++;
    if (!*p)
        return;
    if (phase != P_ONLINE || oscar_send_im(&o, cur->name, p, false) < 0) {
        log_add(cur, K_NOTE, "Not sent: you are signed off.");
    } else {
        log_add(cur, K_ME, p);
        play(SND_IM_OUT);
        cur->sent_typing = 0;
    }
    cur->draft[0] = 0;
    f_draft.cursor = 0;
    kb.shift = true;
    chat_relayout(true);
}

static void open_chat(struct conv *c)
{
    cur = c;
    c->unread = 0;
    sync_badge();
    f_draft.buf = c->draft;
    ui_scroll_reset(&chat_sc, 0, 0);
    set_focus(ui_has_hardware_keyboard() ? &f_draft : NULL);
    chat_relayout(true);
    go(V_CHAT, true);
}

static void open_info(void)
{
    strlcpy(info_name, cur->name, sizeof(info_name));
    info_loaded = false;
    ui_scroll_reset(&info_sc, 0, UI_H - UI_NAV_H);
    oscar_get_info(&o, info_name);
    set_focus(NULL);
    go(V_INFO, true);
}

static void leave_chat(void)
{
    send_typing(cur, cur->draft[0] ? 1 : 0);
    set_focus(NULL);
    build_rows();
    go(V_BUDDIES, false);
}

static void delete_row(int i)
{
    const struct row *r = &rows[i];

    if (r->type == R_GROUP)
        oscar_remove_group(&o, o.groups[r->index].name);
    else if (r->type == R_BUDDY)
        oscar_remove_buddy(&o, o.buddies[r->index].name);
    armed = -1;
    build_rows();
}

static void add_buddy(void)
{
    char norm[OSCAR_SN_MAX];
    const char *group = add_group[0] ? add_group : "Buddies";

    oscar_norm(norm, sizeof(norm), add_name);
    if (!norm[0]) {
        show_alert("Add Buddy", "Enter a screen name.");
        return;
    }
    struct oscar_group *g = oscar_find_group(&o, group);
    for (int i = 0; g && i < o.nbuddies; i++)
        if (o.buddies[i].gid == g->gid && oscar_same_name(o.buddies[i].name, add_name)) {
            show_alert("Add Buddy", "That buddy is already in this group.");
            return;
        }
    if (oscar_add_buddy(&o, add_name, group) < 0) {
        show_alert("Add Buddy", "The buddy couldn't be added.");
        return;
    }
    set_focus(NULL);
    build_rows();
    go(V_BUDDIES, false);
}

static void open_add(void)
{
    add_name[0] = 0;
    strlcpy(add_group, o.ngroups ? o.groups[0].name : "Buddies", sizeof(add_group));
    set_focus(&f_add_name);
    go(V_ADD, true);
}

/* Return on the keyboard: the next field, or the view's action. */
static void field_return(struct field *f)
{
    if (f == &f_sn)
        set_focus(&f_pw);
    else if (f == &f_pw)
        sign_on();
    else if (f == &f_port || f == &f_host) {
        set_focus(f == &f_host ? &f_port : NULL);
        save_account();
    } else if (f == &f_draft)
        send_draft();
    else if (f == &f_away) {
        add_away(new_away);
        new_away[0] = 0;
        set_focus(NULL);
    } else if (f == &f_profile) {
        save_account();
        oscar_set_profile(&o, acct.profile);
        set_focus(NULL);
    } else if (f == &f_add_name)
        set_focus(&f_add_group);
    else if (f == &f_add_group)
        add_buddy();
}

static void field_changed(struct field *f)
{
    if (f == &f_draft)
        send_typing(cur, cur->draft[0] ? 2 : 0);
    else if (f == &f_sn || f == &f_host || f == &f_port)
        save_account();
}

/* Leaving a field without Return still keeps what was typed. */
static void blur(void)
{
    if (focus == &f_profile) {
        save_account();
        oscar_set_profile(&o, acct.profile);
    }
    set_focus(NULL);
    if (view == V_CHAT)
        chat_relayout(false);
}

static void field_key(int ch)
{
    struct field *f = focus;
    int n = (int)strlen(f->buf);

    if (ch == '\n') {
        field_return(f);
        return;
    }
    if (ch == '\b') {
        if (f->cursor > 0) {
            memmove(f->buf + f->cursor - 1, f->buf + f->cursor, (size_t)(n - f->cursor + 1));
            f->cursor--;
        }
    } else {
        const char *ins = ch == UI_KEY_DOTCOM ? ".com" : NULL;
        char one[2] = { (char)ch, 0 };
        if (!ins)
            ins = one;
        for (const char *p = ins; *p; p++) {
            if (f->digits && (*p < '0' || *p > '9'))
                continue;
            if (n + 1 >= f->cap)
                break;
            memmove(f->buf + f->cursor + 1, f->buf + f->cursor, (size_t)(n - f->cursor + 1));
            f->buf[f->cursor++] = *p;
            n++;
        }
    }
    field_changed(f);
}

/* ---- protocol events ---------------------------------------------------- */

static void on_event(void *ctx, const struct oscar_event *ev)
{
    struct conv *c;
    (void)ctx;

    switch (ev->type) {
    case OSCAR_EV_PROGRESS:
        strlcpy(progress, ev->text, sizeof(progress));
        break;
    case OSCAR_EV_REDIRECT:
        redirect_pending = true;
        break;
    case OSCAR_EV_ONLINE:
        phase = P_ONLINE;
        online_ms = gz_time_ms();
        went_online = true;
        break;
    case OSCAR_EV_FAILED:
    case OSCAR_EV_SIGNED_OFF:
        strlcpy(offline_msg, ev->text, sizeof(offline_msg));
        went_offline = true;
        break;
    case OSCAR_EV_BUDDIES:
        presence_changed();
        if (view == V_BUDDIES)
            build_rows();
        break;
    case OSCAR_EV_IM:
        c = conv_find(ev->name, true);
        if (!c)
            break;
        /* Use the sender's own spelling from now on. */
        strlcpy(c->name, ev->name, sizeof(c->name));
        log_add(c, ev->flag ? K_THEM_AUTO : K_THEM, ev->text);
        c->typing = 0;
        if (!ui_in_background() && view == V_CHAT && c == cur)
            chat_relayout(false);
        else if (ui_in_background() || !(view == V_INFO && c == cur)) {
            c->unread++;
            sync_badge();
        }
        if (ui_in_background()) {
            ui_notify_post((uint32_t)(c - convs) + 1u, 0, c->name, ev->text, 0);
            if (view == V_CHAT && c == cur)
                chat_relayout(false);
        }
        if (view == V_BUDDIES)
            build_rows();
        if (!ui_in_background())
            play(SND_IM_IN);            /* in the background the note sounds */
        if (away_now[0] && !ev->flag && (!c->autoreply_ms || gz_time_ms() - c->autoreply_ms >= AUTOREPLY_MS)) {
            if (oscar_send_im(&o, c->name, away_now, true) == 0) {
                log_add(c, K_ME_AUTO, away_now);
                c->autoreply_ms = gz_time_ms() | 1;
                if (view == V_CHAT && c == cur)
                    chat_relayout(false);
            }
        }
        break;
    case OSCAR_EV_TYPING:
        c = conv_find(ev->name, false);
        if (c) {
            c->typing = ev->flag;
            c->typing_ms = gz_time_ms();
        }
        break;
    case OSCAR_EV_INFO:
        if (ev->flag < 0 && view == V_INFO && !info_loaded)
            set_info("", "No information is available. The buddy may be offline.");
        else if (oscar_same_name(ev->name, info_name))
            set_info(ev->text, ev->text2);
        break;
    case OSCAR_EV_NOT_SENT:
        c = conv_find(ev->name, false);
        if (c) {
            char t[128];
            snprintf(t, sizeof(t), "%s", ev->text);
            log_add(c, K_SYS, t);
            if (view == V_CHAT && c == cur)
                chat_relayout(false);
        }
        break;
    case OSCAR_EV_EDITED:
        if (ev->flag < 0)
            show_alert("Buddy List", "The server didn't accept the change.");
        break;
    }
    dirty = true;
}

static int net_send_hook(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    return conn_send(buf, len);
}

/* Moves the connection along: connects, reads, follows the redirect. */
static void net_service(void)
{
    uint8_t buf[2048];
    int n = 0;

    if (phase == P_IDLE)
        return;
    int cs = conn_state();
    if (phase != P_ONLINE && gz_time_ms() - signon_ms > SIGNON_MS) {
        strlcpy(offline_msg, "The server stopped answering.", sizeof(offline_msg));
        went_offline = true;
    } else if (cs == CONN_FAILED) {
        strlcpy(offline_msg, conn_error(), sizeof(offline_msg));
        went_offline = true;
    } else if (cs == CONN_UP) {
        if (!conn_up) {
            conn_up = true;
            if (phase == P_LOGIN)
                oscar_login(&o, acct.sn, password);
            else
                oscar_bos_connected(&o);
            dirty = true;
        }
        for (int i = 0; i < 16 && !redirect_pending && !went_offline; i++) {
            n = conn_recv(buf, sizeof(buf));
            if (n <= 0)
                break;
            oscar_input(&o, buf, (size_t)n);
        }
        if (n < 0 && !redirect_pending && !went_offline) {
            conn_close();
            oscar_closed(&o);
        }
    }
    if (redirect_pending) {
        redirect_pending = false;
        conn_close();
        /* A server set up for itself often advertises a loopback address,
         * which is only right on its own host: use the login host then. */
        const char *host = o.bos_host;
        if (!strncmp(host, "127.", 4) || !strcmp(host, "localhost") || !strcmp(host, "0.0.0.0") || !host[0])
            host = acct.host;
        phase = P_BOS;
        conn_up = false;
        conn_start(host, o.bos_port);
    }
    if (went_offline) {
        went_offline = went_online = false;
        bool was_online = phase == P_ONLINE;
        reset_session();
        set_focus(NULL);
        if (was_online) {
            play(SND_DOOR_SLAM);
            nconvs = 0;
            cur = NULL;
            editing = false;
            sync_badge();
            go(V_SIGNON, false);
            show_alert("Signed Off", offline_msg);
        } else {
            show_alert("Sign On Failed", offline_msg);
        }
    }
    if (went_online) {
        went_online = false;
        play(SND_DOOR_OPEN);
        editing = false;
        armed = -1;
        ui_list_reset(&blist, 0);
        build_rows();
        set_focus(NULL);
        go(V_BUDDIES, true);
    }
}

/* ---- input -------------------------------------------------------------- */

static struct ui_scroll *view_scroll(gfx_rect *area)
{
    switch (view) {
    case V_CHAT:
        *area = transcript_area();
        return &chat_sc;
    case V_INFO:
        *area = gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H);
        return &info_sc;
    case V_STATUS:
        *area = gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H - (show_keyboard() ? UI_KB_H : 0));
        return &status_sc;
    default:
        return NULL;
    }
}

static void back(void)
{
    switch (view) {
    case V_SERVER:
        set_focus(NULL);
        save_account();
        go(V_SIGNON, false);
        break;
    case V_CHAT:
        leave_chat();
        break;
    case V_INFO:
        chat_relayout(true);
        go(V_CHAT, false);
        break;
    case V_STATUS:
        blur();
        build_rows();
        go(V_BUDDIES, false);
        break;
    case V_ADD:
        set_focus(NULL);
        go(V_BUDDIES, false);
        break;
    default:
        break;
    }
}

static void activate(const struct hit *h, int x)
{
    switch (h->id) {
    case H_ALERT:
        alert_open = false;
        break;
    case H_FIELD:
        if (h->f == &f_draft && phase != P_ONLINE)
            break;
        if (focus != h->f) {
            if (focus == &f_profile)
                blur();
            set_focus(h->f);
            if (view == V_STATUS)
                status_reveal();
            if (view == V_CHAT)
                chat_relayout(true);
        }
        break;
    case H_SIGNON:
        sign_on();
        break;
    case H_SERVER:
        set_focus(NULL);
        go(V_SERVER, true);
        break;
    case H_SAVEPW:
        acct.save_password = !acct.save_password;
        save_account();
        break;
    case H_SOUNDS:
        acct.sounds = !acct.sounds;
        save_account();
        break;
    case H_BACK:
        back();
        break;
    case H_LEFT:
        if (view == V_BUDDIES && editing)
            open_add();
        else if (view == V_BUDDIES)
            sign_off();
        else if (view == V_ADD)
            back();
        break;
    case H_RIGHT:
        if (view == V_BUDDIES) {
            editing = !editing;
            armed = -1;
            build_rows();
        } else if (view == V_CHAT) {
            open_info();
        } else if (view == V_ADD) {
            add_buddy();
        }
        break;
    case H_TOOLBAR:
        set_focus(NULL);
        new_away[0] = 0;
        ui_scroll_reset(&status_sc, status_content_h(), UI_H - UI_NAV_H);
        go(V_STATUS, true);
        break;
    case H_SEND:
        send_draft();
        break;
    case H_STATUS_ROW:
        if (focus)
            blur();
        set_away(h->arg ? acct.away[h->arg - 1] : NULL);
        break;
    case H_GROUP_PICK:
        strlcpy(add_group, o.groups[h->arg].name, sizeof(add_group));
        f_add_group.cursor = (int)strlen(add_group);
        break;
    case H_DELETE:
        delete_row(h->arg);
        break;
    }
    (void)x;
    dirty = true;
}

/* A tap on buddy list row i at x. */
static void row_tapped(int i, int x)
{
    const struct row *r = &rows[i];

    if (editing) {
        if (r->type != R_GROUP && r->type != R_BUDDY)
            return;
        armed = x < MARGIN + UI_DELETE_BADGE_W + 4 && armed != i ? i : -1;
        dirty = true;
        return;
    }
    switch (r->type) {
    case R_GROUP:
        toggle_collapsed(o.groups[r->index].gid);
        break;
    case R_OFFLINE:
        toggle_collapsed(GID_OFFLINE);
        break;
    case R_OTHERS:
        toggle_collapsed(GID_OTHERS);
        break;
    case R_BUDDY: {
        struct conv *c = conv_find(o.buddies[r->index].name, true);
        if (c)
            open_chat(c);
        return;
    }
    case R_OTHER:
        open_chat(&convs[r->index]);
        return;
    }
    build_rows();
    dirty = true;
}

static void touch(const struct gz_input_event *e)
{
    gfx_rect area;
    struct ui_scroll *sc = view_scroll(&area);
    int kb_y = UI_H - UI_KB_H;
    static bool in_scroll, in_list;

    if (e->type == GZ_EV_TOUCH_DOWN) {
        in_keyboard = !alert_open && show_keyboard() && e->y >= kb_y;
        in_scroll = in_list = false;
        pressed = (struct hit){ { 0, 0, 0, 0 }, H_NONE, 0, NULL };
        if (!in_keyboard) {
            pressed = hit_at(e->x, e->y);
            if (!alert_open && sc && gfx_rect_contains(area, e->x, e->y)) {
                in_scroll = true;
                ui_scroll_event(sc, e, e->y - area.y);
            }
            if (!alert_open && view == V_BUDDIES && pressed.id == H_NONE &&
                gfx_rect_contains(blist.area, e->x, e->y)) {
                bool d = false;
                in_list = true;
                ui_list_event(&blist, e, &d);
            }
        }
        dirty = true;
    }
    if (in_keyboard) {
        bool changed = false;
        int ch = ui_keyboard_event(&kb, e, kb_y, &changed);
        if (ch && focus)
            field_key(ch);
        if (ch || changed)
            dirty = true;
        if (e->type == GZ_EV_TOUCH_UP)
            in_keyboard = false;
        return;
    }
    if (in_list) {
        bool d = false;
        int r = ui_list_event(&blist, e, &d);
        if (d)
            dirty = true;
        if (r >= 0)
            row_tapped(r, e->x);
        else if (e->type == GZ_EV_TOUCH_UP && editing && armed >= 0)
            dirty = true;
        return;
    }
    if (e->type == GZ_EV_TOUCH_MOVE) {
        if (in_scroll && ui_scroll_event(sc, e, e->y - area.y))
            dirty = true;
        if (in_scroll && sc->moved && pressed.id != H_NONE) {
            pressed.id = H_NONE;
            dirty = true;
        }
        return;
    }
    if (e->type != GZ_EV_TOUCH_UP)
        return;
    if (in_scroll) {
        ui_scroll_event(sc, e, e->y - area.y);
        if (sc->moved) {
            pressed.id = H_NONE;
            dirty = true;
            return;
        }
    }
    struct hit h = pressed, now = hit_at(e->x, e->y);
    pressed.id = H_NONE;
    dirty = true;
    if (h.id != H_NONE && h.id == now.id && h.arg == now.arg && h.f == now.f) {
        activate(&h, e->x);
        return;
    }
    /* A tap on nothing in particular puts the keyboard away. */
    if (h.id == H_NONE && focus && !ui_has_hardware_keyboard())
        blur();
}

static void physical(const struct gz_input_event *e)
{
    if (e->type == GZ_EV_KEY_DOWN && e->code == GZ_KEY_ESC) {
        if (alert_open)
            alert_open = false;
        else if (focus && !ui_has_hardware_keyboard())
            blur();
        else
            back();
        dirty = true;
        return;
    }
    if (alert_open) {
        if (e->type == GZ_EV_KEY_DOWN && (e->code == GZ_KEY_ENTER || e->code == GZ_KEY_OK))
            alert_open = false;
        dirty = true;
        return;
    }
    if (!focus && view == V_CHAT && phase == P_ONLINE && ui_input_char(e))
        set_focus(&f_draft);
    if (!focus)
        return;
    int ch = ui_input_char(e);
    if (ch == '\n') {
        field_return(focus);
    } else {
        struct field *f = focus;
        if (f->digits && ch && ch != '\b' && (ch < '0' || ch > '9'))
            return;
        if (!ui_edit_input(f->buf, f->cap, &f->cursor, e, false))
            return;
        field_changed(f);
    }
    dirty = true;
}

/* Their typing notice lapses if they never send the end of it. */
static void expire_typing(void)
{
    for (int i = 0; i < nconvs; i++)
        if (convs[i].typing && gz_time_ms() - convs[i].typing_ms > 60000) {
            convs[i].typing = 0;
            if (view == V_CHAT && &convs[i] == cur)
                dirty = true;
        }
}

int main(void)
{
    uint32_t last_minute = 0;

    ui_init(SB_STATUS_GRAY);
    ui_set_background_running(true);    /* stay signed on after Home */
    load_account();
    ui_list_init(&blist, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H - TOOLBAR_H), ROW_H);
    oscar_init(&o, &(struct oscar_ops){ NULL, net_send_hook, on_event });
    view = V_SIGNON;
    redraw();

    for (;;) {
        gfx_rect area;
        struct ui_scroll *sc = view_scroll(&area);
        bool sounding = sound_pump();
        int timeout = -1;
        struct gz_input_event ev;

        if (phase != P_IDLE)
            timeout = 100;
        if (sounding)
            timeout = 50;
        if ((sc && sc->animating) || (view == V_BUDDIES && blist.sc.animating))
            timeout = 30;
        if (ui_next_event(&ev, timeout)) {
            if (ev.type == GZ_EV_APP_ACTIVATE) {
                if (view == V_CHAT && cur && cur->unread) {
                    cur->unread = 0;
                    sync_badge();
                }
                dirty = true;
            } else if (ev.type == GZ_EV_APP_DEACTIVATE) {
                pressed.id = H_NONE;
                in_keyboard = false;
            } else if (ev.type == GZ_EV_TEXT || ev.type == GZ_EV_KEY_DOWN || ev.type == GZ_EV_KEY_UP)
                physical(&ev);
            else
                touch(&ev);
        }
        if (sc && ui_scroll_step(sc))
            dirty = true;
        if (view == V_BUDDIES && ui_scroll_step(&blist.sc))
            dirty = true;
        net_service();
        oscar_tick(&o, gz_time_ms());
        expire_typing();
        /* Idle times shown on the list advance by the minute. */
        if (phase == P_ONLINE && gz_time_ms() - last_minute >= 60000) {
            last_minute = gz_time_ms();
            if (view == V_BUDDIES || view == V_CHAT)
                dirty = true;
        }
        if (dirty)
            redraw();
    }
}
