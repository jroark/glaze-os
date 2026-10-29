/*
 * Gmail: an IMAPS + SMTPS email client for Glaze OS.
 *
 * Connects to imap.gmail.com:993 and smtp.gmail.com:465 over wolfSSL using a
 * Gmail address and App Password (or any IMAP4rev1 / ESMTP server configured
 * in Server settings). Account settings, cached Inbox headers and bodies, and
 * any active compose draft persist in <bundle>/data/.
 */

#include <ctype.h>
#include <glaze/fs.h>
#include <glaze/notify.h>
#include <glaze/sys.h>
#include <glaze/tz.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "gmail.h"
#include "mail.h"

#define MARGIN          8
#define CELL_H          30
#define MBOX_ROW_H      36
#define MSG_ROW_H       50
#define TOOLBAR_H       34
#define SEARCH_BAR_H    32
#define LINE_H          16
#define COMP_HDR_H      84      /* To, Cc, Subject rows (28 px each) */
#define COMP_ROW_H      28
#define DRAFT_BODY_MAX  4096
#define SAVE_IDLE_MS    1500
#define CONNECT_MAX_MS  45000

#define C_GMAIL_RED     0xC5221F
#define C_GMAIL_DARK    0x8E1513
#define C_UNREAD_DOT    0x2068D8
#define C_STAR_GOLD     0xE6A800
#define C_STAR_OFF      0xB8C0CC
#define C_QUOTE_TEXT    0x385487
#define C_MUTED_TEXT    0x68707E
#define C_HEADER_BG     0xEEF2F7

enum view {
    V_SIGNON = 0,
    V_SERVER,
    V_MAILBOXES,
    V_LIST,
    V_MESSAGE,
    V_COMPOSE,
};

enum session_phase {
    P_OFFLINE = 0,
    P_CONNECTING,
    P_LOGGING_IN,
    P_ONLINE,
};

/* ---- Saved account, message cache, and compose draft -------------------- */

#define ACCOUNT_VERSION 1
#define CACHE_VERSION   2
#define DRAFT_VERSION   1
#define CACHE_BODIES    8

static struct account {
    uint32_t version;
    char email[MAIL_USER_MAX];
    char password[MAIL_PASS_MAX];
    char imap_host[MAIL_HOST_MAX];
    uint16_t imap_port;
    char smtp_host[MAIL_HOST_MAX];
    uint16_t smtp_port;
    uint8_t save_password;
    uint8_t use_tls;
    uint8_t sounds;
} acct;

/* The inbox index is one file; each cached body is its own "body<slot>" file,
 * written once when stored and read only when that message is opened. */
static struct mail_cache {
    uint32_t version;
    uint32_t inbox_unseen;
    int nmsgs;
    struct mail_summary msgs[MAIL_MAX_MSGS];
    int body_next;
    uint32_t body_uid[CACHE_BODIES];
} cache;
static struct mail_message cache_body;

static struct draft_state {
    uint32_t version;
    uint8_t active;
    char to[MAIL_ADDR_MAX];
    char cc[MAIL_ADDR_MAX];
    char subject[MAIL_SUBJ_MAX];
    char in_reply_to[MAIL_MSGID_MAX];
    char references[MAIL_MSGID_MAX * 2];
    char body[DRAFT_BODY_MAX];
} draft;

static char password[MAIL_PASS_MAX];
static char iport_text[8], sport_text[8];
static bool draft_dirty;
static uint32_t last_draft_edit;

static void data_path(const char *file, char *buf, size_t n)
{
    snprintf(buf, n, "%s/data/%s", res_bundle(), file);
}

static void write_bundle_file(const char *file, const void *data, uint32_t len)
{
    char path[FS_PATH_MAX], dir[FS_PATH_MAX];

    data_path(file, path, sizeof(path));
    if (fs_write_file(path, data, len) == GZ_ENOENT) {
        snprintf(dir, sizeof(dir), "%s/data", res_bundle());
        fs_mkdirs(dir);
        fs_write_file(path, data, len);
    }
}

static uint16_t parse_port(const char *s, uint16_t def)
{
    int v = atoi(s);
    return (v > 0 && v < 65536) ? (uint16_t)v : def;
}

static const char *eff_imap_host(void)
{
    return acct.imap_host[0] ? acct.imap_host : IMAP_DEFAULT_HOST;
}

static uint16_t eff_imap_port(void)
{
    return parse_port(iport_text, IMAP_DEFAULT_PORT);
}

static const char *eff_smtp_host(void)
{
    if (acct.smtp_host[0])
        return acct.smtp_host;
    if (acct.imap_host[0])
        return acct.imap_host;
    return SMTP_DEFAULT_HOST;
}

static uint16_t eff_smtp_port(void)
{
    return parse_port(sport_text, SMTP_DEFAULT_PORT);
}

static void save_account(void)
{
    struct account copy;

    acct.imap_port = parse_port(iport_text, 0);
    acct.smtp_port = parse_port(sport_text, 0);
    copy = acct;
    if (acct.save_password)
        strlcpy(copy.password, password, sizeof(copy.password));
    else
        memset(copy.password, 0, sizeof(copy.password));
    write_bundle_file("account", &copy, sizeof(copy));
}

static void reset_server_defaults(void)
{
    acct.imap_host[0] = 0;
    acct.imap_port = 0;
    acct.smtp_host[0] = 0;
    acct.smtp_port = 0;
    acct.use_tls = 1;
    iport_text[0] = 0;
    sport_text[0] = 0;
}

static void load_account(void)
{
    char path[FS_PATH_MAX];

    data_path("account", path, sizeof(path));
    if (fs_read_file(path, &acct, sizeof(acct)) != sizeof(acct) || acct.version != ACCOUNT_VERSION) {
        memset(&acct, 0, sizeof(acct));
        acct.version = ACCOUNT_VERSION;
        acct.save_password = 1;
        acct.sounds = 1;
        reset_server_defaults();
    }
    acct.email[sizeof(acct.email) - 1] = 0;
    acct.password[sizeof(acct.password) - 1] = 0;
    acct.imap_host[sizeof(acct.imap_host) - 1] = 0;
    acct.smtp_host[sizeof(acct.smtp_host) - 1] = 0;
    strlcpy(password, acct.password, sizeof(password));
    if (acct.imap_port)
        snprintf(iport_text, sizeof(iport_text), "%u", acct.imap_port);
    else
        iport_text[0] = 0;
    if (acct.smtp_port)
        snprintf(sport_text, sizeof(sport_text), "%u", acct.smtp_port);
    else
        sport_text[0] = 0;
}

static void save_cache(void)
{
    cache.version = CACHE_VERSION;
    write_bundle_file("cache", &cache, sizeof(cache));
}

static void load_cache(void)
{
    char path[FS_PATH_MAX];

    data_path("cache", path, sizeof(path));
    if (fs_read_file(path, &cache, sizeof(cache)) != sizeof(cache) || cache.version != CACHE_VERSION) {
        memset(&cache, 0, sizeof(cache));
        cache.version = CACHE_VERSION;
    }
    if (cache.nmsgs < 0 || cache.nmsgs > MAIL_MAX_MSGS)
        cache.nmsgs = 0;
    if (cache.body_next < 0 || cache.body_next >= CACHE_BODIES)
        cache.body_next = 0;
}

static const struct mail_message *cache_find_body(uint32_t uid)
{
    char file[16], path[FS_PATH_MAX];

    if (!uid)
        return NULL;
    for (int i = 0; i < CACHE_BODIES; i++) {
        if (cache.body_uid[i] != uid)
            continue;
        snprintf(file, sizeof(file), "body%d", i);
        data_path(file, path, sizeof(path));
        if (fs_read_file(path, &cache_body, sizeof(cache_body)) != sizeof(cache_body) ||
            cache_body.hdr.uid != uid)
            return NULL;
        cache_body.body[sizeof(cache_body.body) - 1] = 0;
        return &cache_body;
    }
    return NULL;
}

static void cache_store_body(const struct mail_message *m)
{
    char file[16];

    if (!m || !m->hdr.uid)
        return;
    int slot = -1;
    for (int i = 0; i < CACHE_BODIES; i++)
        if (cache.body_uid[i] == m->hdr.uid)
            slot = i;
    if (slot < 0) {
        slot = cache.body_next;
        cache.body_next = (cache.body_next + 1) % CACHE_BODIES;
    }
    snprintf(file, sizeof(file), "body%d", slot);
    write_bundle_file(file, m, sizeof(*m));
    cache.body_uid[slot] = m->hdr.uid;
    for (int i = 0; i < cache.nmsgs; i++) {
        if (cache.msgs[i].uid == m->hdr.uid) {
            cache.msgs[i].flags |= MAIL_F_SEEN | (m->hdr.flags & MAIL_F_HAS_ATT);
            if (m->hdr.snippet[0])
                strlcpy(cache.msgs[i].snippet, m->hdr.snippet, sizeof(cache.msgs[i].snippet));
            break;
        }
    }
    save_cache();
}

static void save_draft(void)
{
    draft.version = DRAFT_VERSION;
    draft.active = (draft.to[0] || draft.cc[0] || draft.subject[0] || draft.body[0]) ? 1 : 0;
    write_bundle_file("draft", &draft, sizeof(draft));
    draft_dirty = false;
}

static void load_draft(void)
{
    char path[FS_PATH_MAX];

    data_path("draft", path, sizeof(path));
    if (fs_read_file(path, &draft, sizeof(draft)) != sizeof(draft) || draft.version != DRAFT_VERSION) {
        memset(&draft, 0, sizeof(draft));
        draft.version = DRAFT_VERSION;
    }
    draft.to[sizeof(draft.to) - 1] = 0;
    draft.cc[sizeof(draft.cc) - 1] = 0;
    draft.subject[sizeof(draft.subject) - 1] = 0;
    draft.in_reply_to[sizeof(draft.in_reply_to) - 1] = 0;
    draft.references[sizeof(draft.references) - 1] = 0;
    draft.body[sizeof(draft.body) - 1] = 0;
}

static void clear_draft(void)
{
    memset(&draft, 0, sizeof(draft));
    draft.version = DRAFT_VERSION;
    save_draft();
}

/* ---- Session and UI state ----------------------------------------------- */

static struct imap im;
static struct smtp sm;
static enum session_phase phase = P_OFFLINE;
static bool imap_conn_up, smtp_conn_up, smtp_busy;
static uint32_t connect_t0;
static char status_text[80] = "Not signed in";

static enum view view = V_SIGNON;
static enum view compose_return_view = V_LIST;
static bool dirty;
static int cur_msg_idx = -1;
static bool msg_loading;
static uint32_t pending_fetch_uid;
static bool refresh_pending;            /* reload the inbox once the engine is idle */
static bool new_mail_note;              /* post a note when the inbox refresh lands */      /* body to fetch once the engine is idle */
static bool search_open;
static char search_buf[64];

/* Alert state. */
enum { ALERT_NONE = 0, ALERT_INFO, ALERT_REPLY_KIND, ALERT_DRAFT_CLOSE };
static int alert_kind = ALERT_NONE;
static char alert_title[48];
static char alert_msg[128];
static int alert_pressed = -1;
static gfx_rect alert_r1, alert_r2;

/* Input fields. */
struct field {
    char *buf;
    int cap, cursor;
    bool secret, url, digits, caps, multiline;
    const char *placeholder;
    gfx_rect r;
};

static unsigned field_flags(const struct field *f)
{
    return (f->secret ? UI_EDIT_SECRET : 0) | (f->digits ? UI_EDIT_DIGITS : 0) |
           (f->url ? UI_EDIT_URL : 0) | (f->multiline ? UI_EDIT_MULTILINE : 0);
}

static struct field f_email = { acct.email, MAIL_USER_MAX, 0, false, true, false, false, false, "user@gmail.com", { 0 } };
static struct field f_pass  = { password, MAIL_PASS_MAX, 0, true, false, false, false, false, "App Password", { 0 } };
static struct field f_ihost = { acct.imap_host, MAIL_HOST_MAX, 0, false, true, false, false, false, "imap.gmail.com", { 0 } };
static struct field f_iport = { iport_text, sizeof(iport_text), 0, false, false, true, false, false, "993", { 0 } };
static struct field f_shost = { acct.smtp_host, MAIL_HOST_MAX, 0, false, true, false, false, false, "smtp.gmail.com", { 0 } };
static struct field f_sport = { sport_text, sizeof(sport_text), 0, false, false, true, false, false, "465", { 0 } };
static struct field f_search = { search_buf, sizeof(search_buf), 0, false, false, false, false, false, "Search Mail", { 0 } };
static struct field f_to   = { draft.to, MAIL_ADDR_MAX, 0, false, true, false, false, false, "recipient@gmail.com", { 0 } };
static struct field f_cc   = { draft.cc, MAIL_ADDR_MAX, 0, false, true, false, false, false, "Optional", { 0 } };
static struct field f_subj = { draft.subject, MAIL_SUBJ_MAX, 0, false, false, false, true, false, "Subject", { 0 } };
static struct field f_body = { draft.body, DRAFT_BODY_MAX, 0, false, false, false, true, true, "Write your message...", { 0 } };

static struct field *focus;
static struct ui_keyboard kb;
static bool in_keyboard;

static struct ui_scroll signon_sc, server_sc, mbox_sc, msg_sc, comp_sc;
static struct ui_list mlist;

/* Wrapped lines for V_MESSAGE and V_COMPOSE body. */
static int msg_lines[512], nmsg_lines;
static int subj_lines[8], nsubj_lines;
static int comp_lines[400], ncomp_lines;

/* Touch hit targets. */
enum {
    H_NONE = 0,
    H_FIELD,
    H_SIGNIN,
    H_SERVER,
    H_SAVEPW,
    H_OFFLINE,
    H_TLS,
    H_SOUNDS,
    H_RESET_DEFAULTS,
    H_BACK,
    H_LEFT,
    H_RIGHT,
    H_MBOX_ROW,
    H_REFRESH,
    H_SEARCH_TOGGLE,
    H_SEARCH_GO,
    H_COMPOSE,
    H_PREV_MSG,
    H_NEXT_MSG,
    H_STAR_CUR,
    H_MSG_SEEN,
    H_MSG_STAR,
    H_MSG_REPLY,
    H_MSG_FORWARD,
    H_MSG_ARCHIVE,
    H_MSG_DELETE,
    H_SEND,
    H_SAVE_DRAFT,
    H_DISCARD_DRAFT,
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

static bool show_keyboard(void)
{
    return focus != NULL && !ui_has_hardware_keyboard();
}

static int active_kb_h(void)
{
    return show_keyboard() ? UI_KB_H : 0;
}

static void show_alert(int kind, const char *title, const char *msg)
{
    alert_kind = kind;
    alert_pressed = -1;
    strlcpy(alert_title, title ? title : "Gmail", sizeof(alert_title));
    strlcpy(alert_msg, msg ? msg : "", sizeof(alert_msg));
    dirty = true;
}

static void update_badge(void)
{
    uint32_t unseen = (phase == P_ONLINE) ? im.inbox_unseen : cache.inbox_unseen;
    ui_set_badge((int)unseen);
}

/* ---- Active message list helpers (live or offline cache) ---------------- */

static int current_msg_count(void)
{
    if (phase == P_ONLINE)
        return im.nmsgs;
    if (im.cur_mbox == MBOX_INBOX && !im.in_search)
        return cache.nmsgs;
    return 0;
}

static struct mail_summary *current_msg_at(int i)
{
    if (i < 0 || i >= current_msg_count())
        return NULL;
    return (phase == P_ONLINE) ? &im.msgs[i] : &cache.msgs[i];
}

static void sync_list_area(void)
{
    int top = UI_NAV_H + (search_open ? SEARCH_BAR_H : 0);
    int bot = UI_H - (show_keyboard() ? UI_KB_H : TOOLBAR_H);
    ui_list_set_area(&mlist, gfx_r(0, top, UI_W, bot - top));
    ui_list_set_count(&mlist, current_msg_count());
}

/* ---- Layout helpers for Compose and Message views ----------------------- */

static int comp_view_h(void)
{
    return UI_H - UI_NAV_H - (show_keyboard() ? UI_KB_H : TOOLBAR_H);
}

static void layout_compose(void)
{
    ncomp_lines = ui_wrap(ui_font(FONT_R13), draft.body, UI_W - MARGIN * 2, comp_lines, 400);
    comp_sc.content_h = COMP_HDR_H + 12 + ncomp_lines * LINE_H + 24;
    comp_sc.view_h = comp_view_h();
}

static int comp_cursor_line(void)
{
    int l = 0;
    while (l + 1 < ncomp_lines && comp_lines[l + 1] <= f_body.cursor)
        l++;
    return l;
}

static void keep_comp_cursor_visible(void)
{
    layout_compose();
    if (focus == &f_to) {
        comp_sc.offset = 0;
        return;
    }
    if (focus == &f_cc) {
        if (comp_sc.offset > COMP_ROW_H)
            comp_sc.offset = 0;
        return;
    }
    if (focus == &f_subj) {
        if (comp_sc.offset > COMP_ROW_H * 2)
            comp_sc.offset = COMP_ROW_H;
        return;
    }
    if (focus != &f_body)
        return;
    int y = COMP_HDR_H + 8 + comp_cursor_line() * LINE_H;
    int vh = comp_view_h();
    if (y - comp_sc.offset < 0)
        comp_sc.offset = y - 6;
    if (y + LINE_H - comp_sc.offset > vh)
        comp_sc.offset = y + LINE_H - vh + 6;
    if (comp_sc.offset < 0)
        comp_sc.offset = 0;
}

static void set_focus(struct field *f)
{
    focus = f;
    if (f) {
        int len = (int)strlen(f->buf);
        if (f->cursor > len || !f->multiline)
            f->cursor = len;
        ui_keyboard_edit_init(&kb, field_flags(f));
        kb.url = f->url;
        kb.numbers = f->digits;
        kb.shift = f->caps && (f->cursor == 0 || (f->multiline && f->buf[f->cursor - 1] == '\n'));
    }
    if (view == V_SERVER) {
        if (f == &f_shost || f == &f_sport)
            server_sc.offset = 86;
        else
            server_sc.offset = 0;
    }
    if (view == V_LIST)
        sync_list_area();
    if (view == V_COMPOSE)
        keep_comp_cursor_visible();
}

static void blur(void)
{
    if (view == V_SIGNON || view == V_SERVER)
        save_account();
    if (view == V_COMPOSE && draft_dirty)
        save_draft();
    set_focus(NULL);
}

static void draw_current(gfx_surface *s);

static void redraw(void)
{
    draw_current(&ui.surf);
    ui_invalidate_all();
    ui_present();
    dirty = false;
}

static void go(enum view next, bool forward)
{
    view = next;
    if (view == V_LIST)
        sync_list_area();
    ui_transition(draw_current, forward);
    dirty = false;
}

/* ---- Drawing primitives and custom icons -------------------------------- */

static void draw_star_glyph(gfx_surface *s, int cx, int cy, bool lit, bool sel)
{
    uint32_t col = sel ? 0xFFFFFF : (lit ? C_STAR_GOLD : C_STAR_OFF);
    /* Crisp 9x9 pixel star icon. */
    gfx_fill(s, gfx_r(cx, cy - 4, 1, 9), col);
    gfx_fill(s, gfx_r(cx - 4, cy - 1, 9, 2), col);
    gfx_fill(s, gfx_r(cx - 2, cy - 2, 5, 5), col);
    gfx_fill(s, gfx_r(cx - 3, cy + 2, 2, 2), col);
    gfx_fill(s, gfx_r(cx + 2, cy + 2, 2, 2), col);
    gfx_fill(s, gfx_r(cx - 4, cy + 3, 1, 2), col);
    gfx_fill(s, gfx_r(cx + 4, cy + 3, 1, 2), col);
}

static void draw_envelope_emblem(gfx_surface *s, int x, int y, int w, int h)
{
    gfx_round_rect(s, gfx_r(x, y + 1, w, h), 3, 0x202838, 120);
    gfx_round_rect_vgradient(s, gfx_r(x, y, w, h), 3, 0xFFFFFF, 0xEAE6DC, 255);
    /* Left and right Gmail crimson pillars. */
    gfx_fill(s, gfx_r(x, y + 1, 4, h - 2), C_GMAIL_RED);
    gfx_fill(s, gfx_r(x + w - 4, y + 1, 4, h - 2), C_GMAIL_RED);
    /* Diagonal M flap. */
    int mx = x + w / 2, my = y + h / 2 + 2;
    gfx_line_aa(s, (x + 2) << 4, (y + 2) << 4, mx << 4, my << 4, 40, C_GMAIL_RED, 255);
    gfx_line_aa(s, (x + w - 2) << 4, (y + 2) << 4, mx << 4, my << 4, 40, C_GMAIL_RED, 255);
}

static void draw_mbox_icon(gfx_surface *s, int mbox, int cx, int cy, bool sel)
{
    uint32_t fg = sel ? 0xFFFFFF : 0x35588A;

    switch (mbox) {
    case MBOX_INBOX:
        gfx_round_rect(s, gfx_r(cx - 7, cy - 5, 14, 11), 2, fg, 255);
        gfx_fill(s, gfx_r(cx - 5, cy - 3, 10, 4), sel ? UI_SELECT_TOP : 0xFFFFFF);
        gfx_fill(s, gfx_r(cx - 2, cy, 4, 2), sel ? UI_SELECT_TOP : 0xFFFFFF);
        break;
    case MBOX_STARRED:
        draw_star_glyph(s, cx, cy, true, sel);
        break;
    case MBOX_SENT:
        gfx_line_aa(s, (cx - 6) << 4, (cy + 1) << 4, (cx + 6) << 4, (cy - 5) << 4, 24, fg, 255);
        gfx_line_aa(s, (cx + 6) << 4, (cy - 5) << 4, (cx + 1) << 4, (cy + 5) << 4, 24, fg, 255);
        gfx_line_aa(s, (cx - 6) << 4, (cy + 1) << 4, (cx + 1) << 4, (cy + 5) << 4, 20, fg, 255);
        break;
    case MBOX_DRAFTS:
        gfx_fill(s, gfx_r(cx - 5, cy - 6, 10, 12), fg);
        gfx_fill(s, gfx_r(cx - 3, cy - 4, 6, 8), sel ? UI_SELECT_TOP : 0xFFFFFF);
        gfx_fill(s, gfx_r(cx - 2, cy - 2, 4, 1), fg);
        gfx_fill(s, gfx_r(cx - 2, cy + 1, 4, 1), fg);
        break;
    case MBOX_ALL:
        gfx_fill(s, gfx_r(cx - 7, cy - 5, 14, 3), fg);
        gfx_fill(s, gfx_r(cx - 6, cy - 1, 12, 7), fg);
        gfx_fill(s, gfx_r(cx - 2, cy + 1, 4, 2), sel ? UI_SELECT_TOP : 0xFFFFFF);
        break;
    case MBOX_SPAM:
        gfx_circle(s, cx, cy, 6, sel ? 0xFFFFFF : C_GMAIL_RED, 255);
        gfx_fill(s, gfx_r(cx, cy - 4, 1, 5), sel ? UI_SELECT_TOP : 0xFFFFFF);
        gfx_fill(s, gfx_r(cx, cy + 3, 1, 1), sel ? UI_SELECT_TOP : 0xFFFFFF);
        break;
    case MBOX_TRASH:
        gfx_fill(s, gfx_r(cx - 5, cy - 4, 11, 2), fg);
        gfx_fill(s, gfx_r(cx - 2, cy - 6, 5, 2), fg);
        gfx_fill(s, gfx_r(cx - 4, cy - 1, 9, 8), fg);
        break;
    }
}

static void draw_toolbar_bg(gfx_surface *s)
{
    gfx_rect r = gfx_r(0, UI_H - TOOLBAR_H, UI_W, TOOLBAR_H);
    gfx_vgradient(s, gfx_r(0, r.y, UI_W, r.h / 2), 0xB8C4D4, 0x899BB3, 255);
    gfx_vgradient(s, gfx_r(0, r.y + r.h / 2, UI_W, r.h - r.h / 2), 0x778AA4, 0x566982, 255);
    gfx_fill(s, gfx_r(0, r.y, UI_W, 1), 0x2D3642);
}

static void draw_toolbar_btn(gfx_surface *s, gfx_rect r, const char *label, bool blue, int hit_id, int hit_arg)
{
    bool down = is_pressed(hit_id, hit_arg);
    const gfx_font *f = ui_font(FONT_B11);

    gfx_round_rect(s, gfx_r(r.x, r.y + 1, r.w, r.h), 4, 0x26303D, 180);
    if (blue)
        gfx_round_rect_vgradient(s, r, 4, down ? 0x2050A8 : 0x5B93F0, down ? 0x103478 : 0x2460C8, 255);
    else
        gfx_round_rect_vgradient(s, r, 4, down ? 0x4A5B72 : 0x8FA1B8, down ? 0x344356 : 0x5C6F88, 255);
    gfx_text_shadow(s, r.x + (r.w - gfx_text_width(f, label)) / 2,
                    r.y + (r.h - f->line_height) / 2, f, label, 0xFFFFFF, 0x283240, -1, 255);
    add_hit(r, hit_id, hit_arg, NULL);
}

static void draw_field_value(gfx_surface *s, struct field *f, int x, int y, int w)
{
    const gfx_font *font = ui_font(FONT_R13);
    char stars[MAIL_PASS_MAX];
    const char *shown = f->buf;

    if (f->secret) {
        size_t n = strlen(f->buf);
        if (n >= sizeof(stars))
            n = sizeof(stars) - 1;
        memset(stars, 0x83, n); /* GFX_CH_BULLET */
        stars[n] = 0;
        shown = stars;
    }

    int ty = y + (CELL_H - font->line_height) / 2;
    if (!shown[0] && f != focus && f->placeholder) {
        gfx_text_fit(s, x, ty, w, font, f->placeholder, 0x99A0AC, 255);
        return;
    }

    int cur = f->cursor < (int)strlen(shown) ? f->cursor : (int)strlen(shown);
    int cur_x = gfx_text_width_n(font, shown, cur);
    int scroll_x = 0;
    if (f == focus && cur_x > w - 6)
        scroll_x = cur_x - (w - 6);

    gfx_rect prev_clip = s->clip;
    gfx_set_clip(s, gfx_r(x, y, w, CELL_H));
    if (f == focus && !f->secret) ui_draw_edit_selection(s, font, f->buf, f->cursor, 0, strlen(f->buf), x - scroll_x, ty, font->line_height);
    gfx_text(s, x - scroll_x, ty, font, shown, UI_TEXT_DETAIL, 255);
    if (f == focus)
        gfx_fill(s, gfx_r(x + cur_x - scroll_x, ty + 1, 2, font->line_height - 2), 0x2A6CD9);
    s->clip = prev_clip;
}

static void draw_form_cell(gfx_surface *s, int y, int pos, const char *label, struct field *f)
{
    const gfx_font *lf = ui_font(FONT_B13);
    gfx_rect r = gfx_r(MARGIN, y, UI_W - MARGIN * 2, CELL_H);

    ui_draw_group_cell(s, r, pos, false);
    gfx_text(s, r.x + 10, y + (CELL_H - lf->line_height) / 2, lf, label, UI_TEXT, 255);
    int vx = r.x + 88, vw = r.w - 96;
    f->r = gfx_r(vx, y, vw, CELL_H);
    draw_field_value(s, f, vx, y, vw);
    add_hit(r, H_FIELD, 0, f);
}

/* ---- View 1: Sign In (V_SIGNON) ----------------------------------------- */

static void draw_signon(gfx_surface *s)
{
    const gfx_font *tf = ui_font(FONT_B14);
    const gfx_font *sf = ui_font(FONT_R11), *bf = ui_font(FONT_B13);
    int view_h = UI_H - UI_NAV_H - active_kb_h();
    gfx_rect area = gfx_r(0, UI_NAV_H, UI_W, view_h);

    signon_sc.content_h = cache.nmsgs > 0 ? 276 : 242;
    signon_sc.view_h = view_h;
    int off = signon_sc.offset;

    gfx_set_clip(s, area);
    ui_draw_pinstripes(s, area);

    /* Gmail banner card. */
    int by = UI_NAV_H + 8 - off;
    draw_envelope_emblem(s, MARGIN + 6, by + 2, 34, 24);
    gfx_text_shadow(s, MARGIN + 48, by + 1, tf, "Google Mail", 0x243044, 0xFFFFFF, 1, 255);
    gfx_text(s, MARGIN + 48, by + 17, sf, "Sign in with an App Password", C_MUTED_TEXT, 255);

    /* Account credentials group. */
    int gy = UI_NAV_H + 42 - off;
    draw_form_cell(s, gy, 1, "Email", &f_email);
    draw_form_cell(s, gy + CELL_H, 2, "Password", &f_pass);
    gfx_rect sw_r = gfx_r(MARGIN, gy + CELL_H * 2, UI_W - MARGIN * 2, CELL_H);
    ui_draw_group_cell(s, sw_r, 3, false);
    gfx_text(s, sw_r.x + 10, sw_r.y + (CELL_H - bf->line_height) / 2, bf, "Save Password", UI_TEXT, 255);
    ui_draw_switch(s, sw_r.x + sw_r.w - UI_SWITCH_W - 8, sw_r.y + (CELL_H - UI_SWITCH_H) / 2, acct.save_password != 0);
    add_hit(sw_r, H_SAVEPW, 0, NULL);

    /* Sign In / Cancel button. */
    int btn_y = gy + CELL_H * 3 + 10;
    gfx_rect sign_r = gfx_r(MARGIN, btn_y, UI_W - MARGIN * 2, 32);
    bool busy = (phase == P_CONNECTING || phase == P_LOGGING_IN);
    bool down = is_pressed(H_SIGNIN, 0);
    gfx_round_rect(s, gfx_r(sign_r.x, sign_r.y + 1, sign_r.w, sign_r.h), 6, 0x3A4454, 180);
    if (busy)
        gfx_round_rect_vgradient(s, sign_r, 6, down ? 0x8E2020 : 0xD84848, down ? 0x621212 : 0xA82020, 255);
    else
        gfx_round_rect_vgradient(s, sign_r, 6, down ? 0x1B4EA0 : 0x4A85E8, down ? 0x123674 : 0x1F57B8, 255);
    const char *btn_lbl = busy ? status_text : "Sign In";
    gfx_text_shadow(s, sign_r.x + (sign_r.w - gfx_text_width(bf, btn_lbl)) / 2,
                    sign_r.y + (sign_r.h - bf->line_height) / 2, bf, btn_lbl, 0xFFFFFF, 0x182438, -1, 255);
    add_hit(sign_r, H_SIGNIN, 0, NULL);

    /* Server settings cell. */
    int sy = btn_y + 40;
    gfx_rect srv_r = gfx_r(MARGIN, sy, UI_W - MARGIN * 2, CELL_H);
    ui_draw_group_cell(s, srv_r, 0, is_pressed(H_SERVER, 0));
    uint32_t sfg = is_pressed(H_SERVER, 0) ? 0xFFFFFF : UI_TEXT;
    uint32_t sdg = is_pressed(H_SERVER, 0) ? 0xFFFFFF : UI_TEXT_DETAIL;
    gfx_text(s, srv_r.x + 10, sy + (CELL_H - bf->line_height) / 2, bf, "Server", sfg, 255);
    char host_sum[40];
    snprintf(host_sum, sizeof(host_sum), "%s:%u", eff_imap_host(), (unsigned)eff_imap_port());
    int hsw = gfx_text_width(sf, host_sum);
    if (hsw > 120)
        hsw = 120;
    gfx_text_fit(s, srv_r.x + srv_r.w - 24 - hsw, sy + (CELL_H - sf->line_height) / 2, 120, sf, host_sum, sdg, 255);
    ui_draw_chevron(s, srv_r.x + srv_r.w - 14, sy + CELL_H / 2 - 5, sdg);
    add_hit(srv_r, H_SERVER, 0, NULL);

    /* Offline cached mail cell when available. */
    if (cache.nmsgs > 0) {
        int oy = sy + CELL_H + 8;
        gfx_rect off_r = gfx_r(MARGIN, oy, UI_W - MARGIN * 2, CELL_H);
        bool odown = is_pressed(H_OFFLINE, 0);
        ui_draw_group_cell(s, off_r, 0, odown);
        char olbl[48];
        snprintf(olbl, sizeof(olbl), "Offline Inbox (%d cached)", cache.nmsgs);
        gfx_text(s, off_r.x + 10, oy + (CELL_H - bf->line_height) / 2, bf, olbl,
                 odown ? 0xFFFFFF : UI_TEXT_DETAIL, 255);
        ui_draw_chevron(s, off_r.x + off_r.w - 14, oy + CELL_H / 2 - 5, odown ? 0xFFFFFF : UI_TEXT_DETAIL);
        add_hit(off_r, H_OFFLINE, 0, NULL);
    }

    gfx_reset_clip(s);

    ui_draw_navbar(s, "Gmail", NULL, NULL, 0);
    gfx_rect rbtn = ui_draw_bar_button(s, "Server", true, is_pressed(H_SERVER, 1), false, 0);
    add_hit(rbtn, H_SERVER, 1, NULL);
}

/* ---- View 2: Server Settings (V_SERVER) --------------------------------- */

static void draw_server(gfx_surface *s)
{
    const gfx_font *hf = ui_font(FONT_B12), *bf = ui_font(FONT_B13);
    int view_h = UI_H - UI_NAV_H - active_kb_h();
    gfx_rect area = gfx_r(0, UI_NAV_H, UI_W, view_h);

    server_sc.content_h = 286;
    server_sc.view_h = view_h;
    int off = server_sc.offset;

    gfx_set_clip(s, area);
    ui_draw_pinstripes(s, area);

    int y = UI_NAV_H + 6 - off;
    gfx_text_shadow(s, MARGIN + 6, y, hf, "Incoming Mail (IMAP)", 0x4C566C, 0xFFFFFF, 1, 255);
    y += 18;
    draw_form_cell(s, y, 1, "Host", &f_ihost);
    draw_form_cell(s, y + CELL_H, 3, "Port", &f_iport);

    y += CELL_H * 2 + 8;
    gfx_text_shadow(s, MARGIN + 6, y, hf, "Outgoing Mail (SMTP)", 0x4C566C, 0xFFFFFF, 1, 255);
    y += 18;
    draw_form_cell(s, y, 1, "Host", &f_shost);
    draw_form_cell(s, y + CELL_H, 3, "Port", &f_sport);

    y += CELL_H * 2 + 8;
    gfx_rect tls_r = gfx_r(MARGIN, y, UI_W - MARGIN * 2, CELL_H);
    ui_draw_group_cell(s, tls_r, 1, false);
    gfx_text(s, tls_r.x + 10, y + (CELL_H - bf->line_height) / 2, bf, "Use SSL/TLS", UI_TEXT, 255);
    ui_draw_switch(s, tls_r.x + tls_r.w - UI_SWITCH_W - 8, y + (CELL_H - UI_SWITCH_H) / 2, acct.use_tls != 0);
    add_hit(tls_r, H_TLS, 0, NULL);

    gfx_rect snd_r = gfx_r(MARGIN, y + CELL_H, UI_W - MARGIN * 2, CELL_H);
    ui_draw_group_cell(s, snd_r, 3, false);
    gfx_text(s, snd_r.x + 10, y + CELL_H + (CELL_H - bf->line_height) / 2, bf, "Mail Sounds", UI_TEXT, 255);
    ui_draw_switch(s, snd_r.x + snd_r.w - UI_SWITCH_W - 8, y + CELL_H + (CELL_H - UI_SWITCH_H) / 2, acct.sounds != 0);
    add_hit(snd_r, H_SOUNDS, 0, NULL);

    y += CELL_H * 2 + 8;
    gfx_rect rst_r = gfx_r(MARGIN, y, UI_W - MARGIN * 2, CELL_H);
    bool rdown = is_pressed(H_RESET_DEFAULTS, 0);
    ui_draw_group_cell(s, rst_r, 0, rdown);
    gfx_text_center(s, rst_r.x, y + (CELL_H - bf->line_height) / 2, rst_r.w, bf,
                    "Reset to Gmail Defaults", rdown ? 0xFFFFFF : UI_TEXT_DETAIL, 255);
    add_hit(rst_r, H_RESET_DEFAULTS, 0, NULL);

    gfx_reset_clip(s);

    gfx_rect back_r;
    ui_draw_navbar(s, "Server", "Gmail", &back_r, 0);
    add_hit(back_r, H_BACK, 0, NULL);
}

/* ---- View 3: Mailboxes (V_MAILBOXES) ------------------------------------ */

static void draw_mailboxes(gfx_surface *s)
{
    const gfx_font *bf = ui_font(FONT_B13), *sf = ui_font(FONT_R11), *pf = ui_font(FONT_B11);
    int view_h = UI_H - UI_NAV_H - TOOLBAR_H;
    gfx_rect area = gfx_r(0, UI_NAV_H, UI_W, view_h);

    mbox_sc.content_h = 26 + MBOX_COUNT * MBOX_ROW_H + 12;
    mbox_sc.view_h = view_h;
    int off = mbox_sc.offset;

    gfx_set_clip(s, area);
    ui_draw_pinstripes(s, area);

    int y = UI_NAV_H + 6 - off;
    const char *sub = acct.email[0] ? acct.email : "Offline Cache";
    gfx_text_shadow(s, MARGIN + 6, y, sf, sub, 0x4C566C, 0xFFFFFF, 1, 255);
    y += 18;

    for (int i = 0; i < MBOX_COUNT; i++) {
        gfx_rect r = gfx_r(MARGIN, y + i * MBOX_ROW_H, UI_W - MARGIN * 2, MBOX_ROW_H);
        bool sel = is_pressed(H_MBOX_ROW, i);
        ui_draw_group_cell(s, r, ui_group_pos(i, MBOX_COUNT), sel);
        draw_mbox_icon(s, i, r.x + 16, r.y + MBOX_ROW_H / 2, sel);
        gfx_text(s, r.x + 32, r.y + (MBOX_ROW_H - bf->line_height) / 2, bf,
                 imap_mbox_title(i), sel ? 0xFFFFFF : UI_TEXT, 255);

        uint32_t unseen = (phase == P_ONLINE) ? im.inbox_unseen : cache.inbox_unseen;
        if (i == MBOX_INBOX && unseen > 0) {
            char ubuf[16];
            snprintf(ubuf, sizeof(ubuf), "%u", unseen);
            int pw = gfx_text_width(pf, ubuf) + 12;
            if (pw < 22)
                pw = 22;
            gfx_rect pill = gfx_r(r.x + r.w - 22 - pw, r.y + (MBOX_ROW_H - 18) / 2, pw, 18);
            gfx_round_rect(s, pill, 9, sel ? 0xFFFFFF : 0x8297B8, 255);
            gfx_text_center(s, pill.x, pill.y + (pill.h - pf->line_height) / 2, pill.w, pf,
                            ubuf, sel ? UI_SELECT_BOTTOM : 0xFFFFFF, 255);
        }
        ui_draw_chevron(s, r.x + r.w - 14, r.y + MBOX_ROW_H / 2 - 5, sel ? 0xFFFFFF : 0x7A8494);
        add_hit(r, H_MBOX_ROW, i, NULL);
    }

    gfx_reset_clip(s);

    ui_draw_navbar(s, "Mailboxes", NULL, NULL, 0);
    gfx_rect lbtn = ui_draw_bar_button(s, phase == P_ONLINE ? "Sign Out" : "Sign In",
                                       false, is_pressed(H_LEFT, 0), false, 0);
    add_hit(lbtn, H_LEFT, 0, NULL);
    gfx_rect rbtn = ui_draw_bar_button(s, "+", true, is_pressed(H_COMPOSE, 0), true, 0);
    add_hit(rbtn, H_COMPOSE, 0, NULL);

    draw_toolbar_bg(s);
    int ty = UI_H - TOOLBAR_H + 5;
    draw_toolbar_btn(s, gfx_r(6, ty, 58, 24), "Refresh", false, H_REFRESH, 0);
    draw_toolbar_btn(s, gfx_r(91, ty, 58, 24), "Search", false, H_SEARCH_TOGGLE, 0);
    draw_toolbar_btn(s, gfx_r(176, ty, 58, 24), "Compose", true, H_COMPOSE, 1);
}

/* ---- View 4: Message List & Search (V_LIST) ----------------------------- */

static void draw_msg_row(gfx_surface *s, int i, gfx_rect r, bool sel, void *ctx)
{
    const struct mail_summary *m = current_msg_at(i);
    const gfx_font *bf = ui_font(FONT_B13), *rf = ui_font(FONT_R12), *sf = ui_font(FONT_R11), *df = ui_font(FONT_R10);

    (void)ctx;
    ui_draw_list_row(s, r, sel);
    if (!m)
        return;

    bool unseen = !(m->flags & MAIL_F_SEEN);
    if (unseen)
        gfx_circle(s, 8, r.y + 12, 4, sel ? 0xFFFFFF : C_UNREAD_DOT, 255);
    if (m->flags & MAIL_F_FLAGGED)
        draw_star_glyph(s, 8, r.y + 30, true, sel);

    /* Sender (or recipient in Sent/Drafts) and date on line 1. */
    char who[80];
    if ((im.cur_mbox == MBOX_SENT || im.cur_mbox == MBOX_DRAFTS) && m->to[0])
        snprintf(who, sizeof(who), "To: %s", m->to);
    else
        strlcpy(who, m->from_name[0] ? m->from_name : m->from_addr, sizeof(who));

    int dw = gfx_text_width(df, m->date);
    gfx_text(s, UI_W - 18 - dw, r.y + 4, df, m->date, sel ? 0xFFFFFF : 0x2460B8, 255);
    gfx_text_fit(s, 18, r.y + 3, UI_W - 42 - dw, bf, who, sel ? 0xFFFFFF : UI_TEXT, 255);

    /* Subject on line 2. */
    char subj_line[MAIL_SUBJ_MAX + 8];
    if (m->flags & MAIL_F_HAS_ATT)
        snprintf(subj_line, sizeof(subj_line), "[Att] %s", m->subject);
    else
        strlcpy(subj_line, m->subject, sizeof(subj_line));
    gfx_text_fit(s, 18, r.y + 19, UI_W - 34, rf, subj_line, sel ? 0xFFFFFF : 0x1A1A1A, 255);

    /* Snippet on line 3. */
    const char *snip = m->snippet[0] ? m->snippet : m->from_addr;
    gfx_text_fit(s, 18, r.y + 34, UI_W - 34, sf, snip, sel ? 0xE6EEFA : C_MUTED_TEXT, 255);

    ui_draw_chevron(s, UI_W - 12, r.y + r.h / 2 - 5, sel ? 0xFFFFFF : 0x9AA0AA);
}

static void draw_list_view(gfx_surface *s)
{
    const gfx_font *sf = ui_font(FONT_R12), *bf = ui_font(FONT_B11);

    sync_list_area();
    gfx_fill(s, mlist.area, 0xFFFFFF);
    int n = current_msg_count();
    if (n > 0) {
        ui_list_draw(s, &mlist, draw_msg_row, NULL);
    } else {
        const char *empty = (phase == P_ONLINE && im.active_cmd != ICMD_NONE) ? status_text
                          : im.in_search ? "No matching messages."
                          : "No Messages";
        gfx_text_center(s, 0, mlist.area.y + mlist.area.h / 2 - 10, UI_W, sf, empty, C_MUTED_TEXT, 255);
    }

    if (search_open) {
        gfx_rect sb = gfx_r(0, UI_NAV_H, UI_W, SEARCH_BAR_H);
        gfx_vgradient(s, sb, 0xD2DAE4, 0xA8B5C6, 255);
        gfx_fill(s, gfx_r(0, sb.y + sb.h - 1, UI_W, 1), 0x6E7B8B);

        gfx_rect box = gfx_r(6, sb.y + 4, UI_W - 56, 24);
        gfx_round_rect(s, box, 10, 0x7A889B, 255);
        gfx_round_rect(s, gfx_r(box.x + 1, box.y + 1, box.w - 2, box.h - 2), 9, 0xFFFFFF, 255);
        f_search.r = gfx_r(box.x + 8, box.y - 3, box.w - 16, CELL_H);
        draw_field_value(s, &f_search, box.x + 8, box.y - 3, box.w - 16);
        add_hit(box, H_FIELD, 0, &f_search);

        gfx_rect go_r = gfx_r(UI_W - 46, sb.y + 4, 40, 24);
        bool gdown = is_pressed(H_SEARCH_GO, 0);
        gfx_round_rect_vgradient(s, go_r, 5, gdown ? 0x1E4C9A : 0x4E86E4, gdown ? 0x12326A : 0x2258B8, 255);
        gfx_text_center(s, go_r.x, go_r.y + (go_r.h - bf->line_height) / 2, go_r.w, bf, "Go", 0xFFFFFF, 255);
        add_hit(go_r, H_SEARCH_GO, 0, NULL);
    }

    char title[40];
    if (im.in_search) {
        snprintf(title, sizeof(title), "Search (%d)", n);
    } else if (im.cur_mbox == MBOX_INBOX) {
        uint32_t unseen = (phase == P_ONLINE) ? im.inbox_unseen : cache.inbox_unseen;
        if (unseen > 0)
            snprintf(title, sizeof(title), "Inbox (%u)", unseen);
        else
            strlcpy(title, "Inbox", sizeof(title));
    } else {
        strlcpy(title, imap_mbox_title(im.cur_mbox), sizeof(title));
    }

    gfx_rect back_r;
    ui_draw_navbar(s, title, "Mailboxes", &back_r, 0);
    add_hit(back_r, H_BACK, 0, NULL);
    gfx_rect rbtn = ui_draw_bar_button(s, "+", true, is_pressed(H_COMPOSE, 0), true, 0);
    add_hit(rbtn, H_COMPOSE, 0, NULL);

    if (!show_keyboard()) {
        draw_toolbar_bg(s);
        int ty = UI_H - TOOLBAR_H + 5;
        draw_toolbar_btn(s, gfx_r(5, ty, 50, 24), "Refresh", false, H_REFRESH, 0);
        draw_toolbar_btn(s, gfx_r(135, ty, 48, 24), search_open ? "Close" : "Search", false, H_SEARCH_TOGGLE, 0);
        draw_toolbar_btn(s, gfx_r(187, ty, 48, 24), "Write", true, H_COMPOSE, 1);

        const gfx_font *tf = ui_font(FONT_R10);
        const char *st = (phase == P_ONLINE && im.active_cmd != ICMD_NONE) ? status_text
                       : (phase != P_ONLINE) ? "Offline"
                       : status_text;
        gfx_text_fit(s, 58, UI_H - TOOLBAR_H + (TOOLBAR_H - tf->line_height) / 2, 74, tf, st, 0xFFFFFF, 255);
    }
}

/* ---- View 5: Message Viewer (V_MESSAGE) --------------------------------- */

static void draw_message_view(gfx_surface *s)
{
    const struct mail_message *m = &im.cur_msg;
    const gfx_font *bf = ui_font(FONT_B12), *rf = ui_font(FONT_R12), *sf = ui_font(FONT_R11);
    const gfx_font *subf = ui_font(FONT_B13), *bodyf = ui_font(FONT_R13);
    int view_h = UI_H - UI_NAV_H - TOOLBAR_H;
    gfx_rect area = gfx_r(0, UI_NAV_H, UI_W, view_h);

    nsubj_lines = ui_wrap(subf, m->hdr.subject, UI_W - MARGIN * 2 - 22, subj_lines, 8);
    if (nsubj_lines < 1)
        nsubj_lines = 1;
    nmsg_lines = ui_wrap(bodyf, m->body, UI_W - MARGIN * 2, msg_lines, 512);

    int hdr_h = 38 + (m->hdr.cc[0] ? 14 : 0) + nsubj_lines * 16 + (m->natts > 0 ? 20 : 0) + 8;
    msg_sc.content_h = hdr_h + 10 + nmsg_lines * LINE_H + 20;
    msg_sc.view_h = view_h;
    int off = msg_sc.offset;

    gfx_set_clip(s, area);
    gfx_fill(s, area, 0xFFFFFF);

    /* Header block. */
    int hy = UI_NAV_H - off;
    gfx_fill(s, gfx_r(0, hy, UI_W, hdr_h), C_HEADER_BG);
    gfx_fill(s, gfx_r(0, hy + hdr_h - 1, UI_W, 1), 0xB8C2D0);

    int y = hy + 5;
    gfx_text(s, MARGIN, y, bf, "From:", C_MUTED_TEXT, 255);
    char from_full[160];
    if (m->hdr.from_name[0] && strcmp(m->hdr.from_name, m->hdr.from_addr))
        snprintf(from_full, sizeof(from_full), "%s <%s>", m->hdr.from_name, m->hdr.from_addr);
    else
        strlcpy(from_full, m->hdr.from_addr[0] ? m->hdr.from_addr : m->hdr.from_name, sizeof(from_full));
    int dw = gfx_text_width(sf, m->hdr.date);
    gfx_text(s, UI_W - MARGIN - dw, y + 1, sf, m->hdr.date, C_MUTED_TEXT, 255);
    gfx_text_fit(s, MARGIN + 38, y, UI_W - MARGIN * 2 - 42 - dw, rf, from_full, UI_TEXT_DETAIL, 255);
    y += 15;

    gfx_text(s, MARGIN, y, bf, "To:", C_MUTED_TEXT, 255);
    gfx_text_fit(s, MARGIN + 38, y, UI_W - MARGIN * 2 - 38, sf,
                 m->hdr.to[0] ? m->hdr.to : acct.email, UI_TEXT, 255);
    y += 14;

    if (m->hdr.cc[0]) {
        gfx_text(s, MARGIN, y, bf, "Cc:", C_MUTED_TEXT, 255);
        gfx_text_fit(s, MARGIN + 38, y, UI_W - MARGIN * 2 - 38, sf, m->hdr.cc, UI_TEXT, 255);
        y += 14;
    }

    /* Subject + Star toggle. */
    gfx_rect star_r = gfx_r(UI_W - 28, y - 2, 24, 22);
    draw_star_glyph(s, star_r.x + 12, star_r.y + 10, (m->hdr.flags & MAIL_F_FLAGGED) != 0, false);
    add_hit(star_r, H_STAR_CUR, 0, NULL);

    for (int l = 0; l < nsubj_lines; l++) {
        gfx_text_n(s, MARGIN, y, subf, m->hdr.subject + subj_lines[l],
                   ui_line_len(m->hdr.subject, subj_lines, nsubj_lines, l), UI_TEXT, 255);
        y += 16;
    }

    if (m->natts > 0) {
        char att_txt[80];
        uint32_t kb_sz = (m->atts[0].size + 1023) / 1024;
        if (m->natts == 1)
            snprintf(att_txt, sizeof(att_txt), "Attachment: %s (%u KB)", m->atts[0].name, kb_sz ? kb_sz : 1);
        else
            snprintf(att_txt, sizeof(att_txt), "%d Attachments: %s...", m->natts, m->atts[0].name);
        gfx_round_rect(s, gfx_r(MARGIN, y + 1, UI_W - MARGIN * 2, 16), 3, 0xD8E2F0, 255);
        gfx_text_fit(s, MARGIN + 6, y + 2, UI_W - MARGIN * 2 - 12, sf, att_txt, UI_TEXT_DETAIL, 255);
    }

    /* Message body. */
    int body_top = hy + hdr_h + 8;
    if (msg_loading && !m->body[0]) {
        gfx_text_center(s, 0, body_top + 24, UI_W, rf, "Loading message...", C_MUTED_TEXT, 255);
    } else {
        for (int l = 0; l < nmsg_lines; l++) {
            int ly = body_top + l * LINE_H;
            if (ly + LINE_H < area.y || ly > area.y + area.h)
                continue;
            if (focus == &f_body) ui_draw_edit_selection(s, bf, draft.body, f_body.cursor, comp_lines[l],
                ui_line_len(draft.body, comp_lines, ncomp_lines, l), MARGIN, ly, LINE_H);
            const char *lp = m->body + msg_lines[l];
            int llen = ui_line_len(m->body, msg_lines, nmsg_lines, l);
            uint32_t col = (llen > 0 && lp[0] == '>') ? C_QUOTE_TEXT : UI_TEXT;
            gfx_text_n(s, MARGIN, ly, bodyf, lp, llen, col, 255);
        }
    }

    gfx_reset_clip(s);

    char nav_title[24];
    int total = current_msg_count();
    if (cur_msg_idx >= 0 && total > 0)
        snprintf(nav_title, sizeof(nav_title), "%d of %d", cur_msg_idx + 1, total);
    else
        strlcpy(nav_title, "Message", sizeof(nav_title));

    gfx_rect back_r;
    ui_draw_navbar(s, nav_title, im.in_search ? "Search" : imap_mbox_title(im.cur_mbox), &back_r, 0);
    add_hit(back_r, H_BACK, 0, NULL);

    /* Up/Down message navigation buttons on right of nav bar. */
    const gfx_font *nf = ui_font(FONT_B11);
    gfx_rect up_r = gfx_r(UI_W - 52, 4, 22, 22);
    gfx_rect dn_r = gfx_r(UI_W - 27, 4, 22, 22);
    gfx_round_rect_vgradient(s, up_r, 4, is_pressed(H_PREV_MSG, 0) ? 0x4A5B72 : 0x8FA1B8,
                             is_pressed(H_PREV_MSG, 0) ? 0x344356 : 0x5C6F88, 255);
    gfx_round_rect_vgradient(s, dn_r, 4, is_pressed(H_NEXT_MSG, 0) ? 0x4A5B72 : 0x8FA1B8,
                             is_pressed(H_NEXT_MSG, 0) ? 0x344356 : 0x5C6F88, 255);
    gfx_text_center(s, up_r.x, up_r.y + (up_r.h - nf->line_height) / 2, up_r.w, nf, "^",
                    cur_msg_idx > 0 ? 0xFFFFFF : 0xA8B4C4, 255);
    gfx_text_center(s, dn_r.x, dn_r.y + (dn_r.h - nf->line_height) / 2, dn_r.w, nf, "v",
                    (cur_msg_idx >= 0 && cur_msg_idx + 1 < total) ? 0xFFFFFF : 0xA8B4C4, 255);
    add_hit(up_r, H_PREV_MSG, 0, NULL);
    add_hit(dn_r, H_NEXT_MSG, 0, NULL);

    /* 5-action bottom toolbar. */
    draw_toolbar_bg(s);
    int ty = UI_H - TOOLBAR_H + 5;
    bool seen = (m->hdr.flags & MAIL_F_SEEN) != 0;
    bool starred = (m->hdr.flags & MAIL_F_FLAGGED) != 0;
    draw_toolbar_btn(s, gfx_r(3, ty, 44, 24), seen ? "Unread" : "Read", false, H_MSG_SEEN, 0);
    draw_toolbar_btn(s, gfx_r(50, ty, 42, 24), starred ? "Unstar" : "Star", false, H_MSG_STAR, 0);
    draw_toolbar_btn(s, gfx_r(95, ty, 46, 24), "Reply", true, H_MSG_REPLY, 0);
    draw_toolbar_btn(s, gfx_r(144, ty, 46, 24), "Archive", false, H_MSG_ARCHIVE, 0);
    draw_toolbar_btn(s, gfx_r(193, ty, 44, 24), "Delete", false, H_MSG_DELETE, 0);
}

/* ---- View 6: Compose / Reply / Forward (V_COMPOSE) ---------------------- */

static void draw_compose_hdr_row(gfx_surface *s, int y, const char *label, struct field *f)
{
    const gfx_font *lf = ui_font(FONT_B12);

    gfx_fill(s, gfx_r(0, y + COMP_ROW_H - 1, UI_W, 1), 0xD9D9D9);
    gfx_text(s, MARGIN, y + (COMP_ROW_H - lf->line_height) / 2, lf, label, C_MUTED_TEXT, 255);
    int vx = MARGIN + 54, vw = UI_W - vx - MARGIN;
    f->r = gfx_r(vx, y, vw, COMP_ROW_H);
    draw_field_value(s, f, vx, y - 1, vw);
    add_hit(gfx_r(0, y, UI_W, COMP_ROW_H), H_FIELD, 0, f);
}

static void draw_compose_view(gfx_surface *s)
{
    const gfx_font *bf = ui_font(FONT_R13);
    int vh = comp_view_h();
    gfx_rect area = gfx_r(0, UI_NAV_H, UI_W, vh);

    layout_compose();
    int off = comp_sc.offset;

    gfx_set_clip(s, area);
    gfx_fill(s, area, 0xFFFFFF);

    int hy = UI_NAV_H - off;
    draw_compose_hdr_row(s, hy, "To:", &f_to);
    draw_compose_hdr_row(s, hy + COMP_ROW_H, "Cc:", &f_cc);
    draw_compose_hdr_row(s, hy + COMP_ROW_H * 2, "Subject:", &f_subj);

    int body_top = hy + COMP_HDR_H + 6;
    f_body.r = gfx_r(0, body_top > UI_NAV_H ? body_top : UI_NAV_H, UI_W, area.y + area.h - (body_top > UI_NAV_H ? body_top : UI_NAV_H));
    add_hit(f_body.r, H_FIELD, 0, &f_body);

    if (!draft.body[0] && focus != &f_body) {
        gfx_text(s, MARGIN, body_top, bf, f_body.placeholder, 0x99A0AC, 255);
    } else {
        for (int l = 0; l < ncomp_lines; l++) {
            int ly = body_top + l * LINE_H;
            if (ly + LINE_H < area.y || ly > area.y + area.h)
                continue;
            gfx_text_n(s, MARGIN, ly, bf, draft.body + comp_lines[l],
                       ui_line_len(draft.body, comp_lines, ncomp_lines, l), UI_TEXT, 255);
        }
        if (focus == &f_body) {
            int cl = comp_cursor_line();
            int cx = MARGIN + gfx_text_width_n(bf, draft.body + comp_lines[cl], f_body.cursor - comp_lines[cl]);
            int cy = body_top + cl * LINE_H;
            gfx_fill(s, gfx_r(cx, cy + 1, 2, LINE_H - 2), 0x2A6CD9);
        }
    }

    gfx_reset_clip(s);

    const char *title = smtp_busy ? "Sending..." : (draft.in_reply_to[0] ? "Reply" : "New Message");
    ui_draw_navbar(s, title, NULL, NULL, 0);
    gfx_rect lbtn = ui_draw_bar_button(s, show_keyboard() ? "Done" : "Cancel",
                                       false, is_pressed(H_LEFT, 0), false, 0);
    add_hit(lbtn, H_LEFT, 0, NULL);
    gfx_rect sbtn = ui_draw_bar_button(s, "Send", true, is_pressed(H_SEND, 0), true, 0);
    add_hit(sbtn, H_SEND, 0, NULL);

    if (!show_keyboard()) {
        draw_toolbar_bg(s);
        int ty = UI_H - TOOLBAR_H + 5;
        draw_toolbar_btn(s, gfx_r(8, ty, 78, 24), "Save Draft", false, H_SAVE_DRAFT, 0);
        draw_toolbar_btn(s, gfx_r(154, ty, 78, 24), "Discard", false, H_DISCARD_DRAFT, 0);
    }
}

static void draw_current(gfx_surface *s)
{
    nhits = 0;
    switch (view) {
    case V_SIGNON:    draw_signon(s); break;
    case V_SERVER:    draw_server(s); break;
    case V_MAILBOXES: draw_mailboxes(s); break;
    case V_LIST:       draw_list_view(s); break;
    case V_MESSAGE:    draw_message_view(s); break;
    case V_COMPOSE:    draw_compose_view(s); break;
    }
    if (show_keyboard())
        ui_draw_keyboard(s, &kb, UI_H - UI_KB_H);
    if (alert_kind != ALERT_NONE) {
        const char *b1 = "OK", *b2 = NULL;
        if (alert_kind == ALERT_REPLY_KIND) {
            b1 = "Reply";
            b2 = "Forward";
        } else if (alert_kind == ALERT_DRAFT_CLOSE) {
            b1 = "Save Draft";
            b2 = "Discard";
        }
        ui_draw_alert(s, alert_title, alert_msg, b1, b2, &alert_r1, &alert_r2, alert_pressed);
    }
}

/* ---- Network & Protocol Integration ------------------------------------- */

static int imap_send_hook(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    return conn_send(CONN_CH_IMAP, buf, len);
}

static int smtp_send_hook(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    return conn_send(CONN_CH_SMTP, buf, len);
}

static void on_imap_event(void *ctx, const struct imap_event *ev)
{
    (void)ctx;
    switch (ev->type) {
    case IMAP_EV_PROGRESS:
        if (ev->text)
            strlcpy(status_text, ev->text, sizeof(status_text));
        dirty = true;
        break;

    case IMAP_EV_READY:
        phase = P_ONLINE;
        strlcpy(status_text, "Updated just now", sizeof(status_text));
        cache.inbox_unseen = im.inbox_unseen;
        cache.nmsgs = im.nmsgs;
        memcpy(cache.msgs, im.msgs, (size_t)im.nmsgs * sizeof(im.msgs[0]));
        save_cache();
        update_badge();
        set_focus(NULL);
        if (view == V_SIGNON)
            go(V_LIST, true);
        else
            dirty = true;
        break;

    case IMAP_EV_LIST_DONE:
        strlcpy(status_text, "Updated just now", sizeof(status_text));
        if (im.cur_mbox == MBOX_INBOX && !im.in_search) {
            cache.inbox_unseen = im.inbox_unseen;
            cache.nmsgs = im.nmsgs;
            memcpy(cache.msgs, im.msgs, (size_t)im.nmsgs * sizeof(im.msgs[0]));
            save_cache();
            if (new_mail_note && im.nmsgs > 0) {
                const struct mail_summary *m = &im.msgs[0];
                ui_notify_post(1, 0, m->from_name[0] ? m->from_name : m->from_addr, m->subject, 0);
            }
            new_mail_note = false;
        }
        update_badge();
        dirty = true;
        break;

    case IMAP_EV_MSG_DONE:
        msg_loading = false;
        cache_store_body(&im.cur_msg);
        update_badge();
        dirty = true;
        break;

    case IMAP_EV_ACTION_DONE:
        if (ev->text)
            strlcpy(status_text, ev->text, sizeof(status_text));
        if (im.cur_mbox == MBOX_INBOX && !im.in_search) {
            cache.inbox_unseen = im.inbox_unseen;
            cache.nmsgs = im.nmsgs;
            memcpy(cache.msgs, im.msgs, (size_t)im.nmsgs * sizeof(im.msgs[0]));
            save_cache();
        }
        update_badge();
        dirty = true;
        break;

    case IMAP_EV_NEW_MAIL:
        if (acct.sounds && !ui_in_background())
            sound_play(SND_NEW_MAIL);       /* in the background the note sounds */
        update_badge();
        if (ui_in_background())
            new_mail_note = true;
        /* This arrives during the NOOP; service_network refreshes once it ends. */
        if ((view == V_LIST || ui_in_background()) && im.cur_mbox == MBOX_INBOX && !im.in_search)
            refresh_pending = true;
        dirty = true;
        break;

    case IMAP_EV_ERROR:
        msg_loading = false;
        if (ev->fatal) {
            conn_close(CONN_CH_IMAP);
            imap_conn_up = false;
            phase = P_OFFLINE;
            if (view != V_SIGNON)
                go(V_SIGNON, false);
        }
        show_alert(ALERT_INFO, "Mail Error", ev->text ? ev->text : "An error occurred.");
        break;
    }
}

static void on_smtp_event(void *ctx, const struct smtp_event *ev)
{
    (void)ctx;
    switch (ev->type) {
    case SMTP_EV_PROGRESS:
        if (ev->text)
            strlcpy(status_text, ev->text, sizeof(status_text));
        dirty = true;
        break;

    case SMTP_EV_SENT:
        smtp_busy = false;
        smtp_conn_up = false;
        conn_close(CONN_CH_SMTP);
        if (acct.sounds)
            sound_play(SND_MAIL_SENT);
        clear_draft();
        set_focus(NULL);
        strlcpy(status_text, "Message sent", sizeof(status_text));
        if (view == V_COMPOSE)
            go(compose_return_view, false);
        if (phase == P_ONLINE && im.active_cmd == ICMD_NONE)
            imap_refresh(&im);
        break;

    case SMTP_EV_ERROR:
        smtp_busy = false;
        smtp_conn_up = false;
        conn_close(CONN_CH_SMTP);
        show_alert(ALERT_INFO, "Could Not Send Mail", ev->text ? ev->text : "Failed to send message.");
        break;
    }
}

static void start_signin(void)
{
    set_focus(NULL);
    save_account();
    if (!acct.email[0] || !password[0]) {
        show_alert(ALERT_INFO, "Sign In", "Please enter your Gmail address and App Password.");
        return;
    }
    if (phase == P_CONNECTING || phase == P_LOGGING_IN) {
        conn_close(CONN_CH_IMAP);
        imap_conn_up = false;
        phase = P_OFFLINE;
        strlcpy(status_text, "Cancelled", sizeof(status_text));
        dirty = true;
        return;
    }
    imap_reset(&im);
    imap_login(&im, acct.email, password);
    phase = P_CONNECTING;
    imap_conn_up = false;
    connect_t0 = gz_time_ms();
    strlcpy(status_text, "Connecting...", sizeof(status_text));
    conn_start(CONN_CH_IMAP, eff_imap_host(), eff_imap_port(), acct.use_tls != 0);
    dirty = true;
}

static void sign_out(void)
{
    if (phase == P_ONLINE)
        imap_logout(&im);
    conn_close(CONN_CH_IMAP);
    conn_close(CONN_CH_SMTP);
    imap_conn_up = smtp_conn_up = smtp_busy = false;
    phase = P_OFFLINE;
    strlcpy(status_text, "Signed out", sizeof(status_text));
    go(V_SIGNON, false);
}

static void service_network(void)
{
    /* IMAP channel. */
    if (phase != P_OFFLINE) {
        int st = conn_state(CONN_CH_IMAP);
        if (st == CONN_FAILED) {
            const char *why = conn_error(CONN_CH_IMAP);
            conn_close(CONN_CH_IMAP);
            imap_conn_up = false;
            phase = P_OFFLINE;
            show_alert(ALERT_INFO, "Connection Failed", why);
        } else if (st == CONN_UP) {
            if (!imap_conn_up) {
                imap_conn_up = true;
                phase = P_LOGGING_IN;
                strlcpy(status_text, "Waiting for greeting...", sizeof(status_text));
                dirty = true;
            }
            uint8_t buf[2048];
            for (int iter = 0; iter < 8; iter++) {
                int n = conn_recv(CONN_CH_IMAP, buf, sizeof(buf));
                if (n > 0) {
                    imap_feed(&im, buf, (size_t)n);
                    if (phase == P_OFFLINE)
                        break;
                } else if (n == 0) {
                    break;
                } else {
                    conn_close(CONN_CH_IMAP);
                    imap_conn_up = false;
                    phase = P_OFFLINE;
                    strlcpy(status_text, "Disconnected", sizeof(status_text));
                    dirty = true;
                    break;
                }
            }
        }
        if (pending_fetch_uid && phase == P_ONLINE && im.active_cmd == ICMD_NONE) {
            if (view == V_MESSAGE && msg_loading && im.cur_msg.hdr.uid == pending_fetch_uid)
                imap_fetch_message(&im, pending_fetch_uid);
            pending_fetch_uid = 0;
        }
        if (refresh_pending && phase == P_ONLINE && im.active_cmd == ICMD_NONE) {
            refresh_pending = false;
            imap_refresh(&im);
        }
        if ((phase == P_CONNECTING || phase == P_LOGGING_IN) && gz_time_ms() - connect_t0 > CONNECT_MAX_MS) {
            conn_close(CONN_CH_IMAP);
            imap_conn_up = false;
            phase = P_OFFLINE;
            show_alert(ALERT_INFO, "Sign In Timed Out", "The mail server took too long to respond.");
        }
    }

    /* SMTP channel. */
    if (smtp_busy) {
        int st = conn_state(CONN_CH_SMTP);
        if (st == CONN_FAILED) {
            const char *why = conn_error(CONN_CH_SMTP);
            smtp_busy = false;
            smtp_conn_up = false;
            conn_close(CONN_CH_SMTP);
            show_alert(ALERT_INFO, "Send Failed", why);
        } else if (st == CONN_UP) {
            smtp_conn_up = true;
            uint8_t buf[1024];
            for (int iter = 0; iter < 4 && smtp_busy; iter++) {
                int n = conn_recv(CONN_CH_SMTP, buf, sizeof(buf));
                if (n > 0) {
                    smtp_feed(&sm, buf, (size_t)n);
                } else if (n == 0) {
                    break;
                } else {
                    smtp_busy = false;
                    smtp_conn_up = false;
                    conn_close(CONN_CH_SMTP);
                    show_alert(ALERT_INFO, "Send Failed", "Connection to SMTP server was closed.");
                    break;
                }
            }
        }
    }
}

/* ---- Message & Compose Actions ------------------------------------------ */

static void open_message_at(int idx, bool transition)
{
    int total = current_msg_count();
    if (idx < 0 || idx >= total)
        return;

    cur_msg_idx = idx;
    struct mail_summary *s = current_msg_at(idx);
    ui_scroll_reset(&msg_sc, 0, UI_H - UI_NAV_H - TOOLBAR_H);

    pending_fetch_uid = 0;
    const struct mail_message *cached = cache_find_body(s->uid);
    if (cached) {
        im.cur_msg = *cached;
        im.cur_msg.hdr.flags = s->flags | MAIL_F_SEEN;
        msg_loading = false;
        if (!(s->flags & MAIL_F_SEEN)) {
            if (phase == P_ONLINE && im.active_cmd == ICMD_NONE) {
                imap_set_flag(&im, s->uid, MAIL_F_SEEN, true);
            } else {
                s->flags |= MAIL_F_SEEN;
                if (phase == P_ONLINE && im.cur_mbox == MBOX_INBOX && im.inbox_unseen > 0)
                    im.inbox_unseen--;
                if (cache.inbox_unseen > 0)
                    cache.inbox_unseen--;
            }
            update_badge();
        }
    } else {
        memset(&im.cur_msg, 0, sizeof(im.cur_msg));
        im.cur_msg.hdr = *s;
        im.cur_msg.hdr.flags |= MAIL_F_SEEN;
        if (phase == P_ONLINE) {
            /* The engine marks the summary \Seen and counts it when the body arrives. */
            msg_loading = true;
            if (imap_fetch_message(&im, s->uid) < 0)
                pending_fetch_uid = s->uid;
        } else {
            msg_loading = false;
            if (!(s->flags & MAIL_F_SEEN)) {
                s->flags |= MAIL_F_SEEN;
                if (cache.inbox_unseen > 0)
                    cache.inbox_unseen--;
                update_badge();
            }
            strlcpy(im.cur_msg.body, "(Message body is not cached for offline reading.)", sizeof(im.cur_msg.body));
        }
    }

    if (transition) {
        set_focus(NULL);
        go(V_MESSAGE, true);
    } else {
        dirty = true;
    }
}

static void start_compose(int mode)
{
    /* mode: 0 = new/resume draft, 1 = reply, 2 = forward */
    compose_return_view = view;
    if (mode == 1) {
        const struct mail_message *m = &im.cur_msg;
        memset(&draft, 0, sizeof(draft));
        draft.version = DRAFT_VERSION;
        strlcpy(draft.to, m->reply_to[0] ? m->reply_to : m->hdr.from_addr, sizeof(draft.to));
        mime_reply_subject(m->hdr.subject, false, draft.subject, sizeof(draft.subject));
        strlcpy(draft.in_reply_to, m->hdr.msgid, sizeof(draft.in_reply_to));
        if (m->references[0] && m->hdr.msgid[0])
            snprintf(draft.references, sizeof(draft.references), "%s %s", m->references, m->hdr.msgid);
        else
            strlcpy(draft.references, m->hdr.msgid, sizeof(draft.references));
        snprintf(draft.body, sizeof(draft.body), "\n\nOn %s, %s wrote:\n> ",
                 m->hdr.date[0] ? m->hdr.date : "earlier",
                 m->hdr.from_name[0] ? m->hdr.from_name : m->hdr.from_addr);
        size_t o = strlen(draft.body);
        for (const char *p = m->body; *p && o + 4 < sizeof(draft.body); p++) {
            draft.body[o++] = *p;
            if (*p == '\n' && p[1] && o + 3 < sizeof(draft.body)) {
                draft.body[o++] = '>';
                draft.body[o++] = ' ';
            }
        }
        draft.body[o] = 0;
        f_body.cursor = 0;
        save_draft();
    } else if (mode == 2) {
        const struct mail_message *m = &im.cur_msg;
        memset(&draft, 0, sizeof(draft));
        draft.version = DRAFT_VERSION;
        mime_reply_subject(m->hdr.subject, true, draft.subject, sizeof(draft.subject));
        snprintf(draft.body, sizeof(draft.body),
                 "\n\n---------- Forwarded message ---------\nFrom: %s <%s>\nDate: %s\nSubject: %s\n\n%s",
                 m->hdr.from_name, m->hdr.from_addr, m->hdr.date, m->hdr.subject, m->body);
        f_body.cursor = 0;
        save_draft();
    }

    ui_scroll_reset(&comp_sc, 0, comp_view_h());
    if (!draft.to[0])
        set_focus(&f_to);
    else if (!draft.subject[0])
        set_focus(&f_subj);
    else {
        f_body.cursor = 0;
        set_focus(&f_body);
    }
    go(V_COMPOSE, true);
}

/* RFC 5322 Date for now: the RTC holds local time in the Settings zone. */
static void rfc_date_now(char *buf, size_t n)
{
    static const char wday[7][4] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char mon[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    struct gz_datetime dt;

    buf[0] = 0;
    if (gz_rtc_get(&dt) < 0 || dt.month < 1 || dt.month > 12 || dt.weekday > 6)
        return;
    int zone = tz_current();
    int off = tz_offset_min(zone, tz_local_to_utc(zone, gz_dt_to_secs(&dt)));
    int aoff = off < 0 ? -off : off;
    snprintf(buf, n, "%s, %u %s %u %02u:%02u:%02u %c%02d%02d", wday[dt.weekday], dt.day,
             mon[dt.month - 1], dt.year, dt.hour, dt.minute, dt.second,
             off < 0 ? '-' : '+', aoff / 60, aoff % 60);
}

static void send_current_draft(void)
{
    if (smtp_busy)
        return;
    set_focus(NULL);
    save_draft();
    if (!draft.to[0]) {
        show_alert(ALERT_INFO, "Missing Recipient", "Please enter an email address in the To: field.");
        return;
    }
    if (!acct.email[0] || !password[0]) {
        show_alert(ALERT_INFO, "Not Signed In", "Please enter your account credentials first.");
        return;
    }

    char *raw = malloc(MAIL_RAW_OUT_MAX);
    if (!raw) {
        show_alert(ALERT_INFO, "Mail Error", "Out of memory.");
        return;
    }
    char msgid[96], date[40];
    snprintf(msgid, sizeof(msgid), "<%u.%u@glaze.local>", (unsigned)gz_time_ms(), (unsigned)ui_now_secs());
    rfc_date_now(date, sizeof(date));
    size_t rlen = mime_build_message(raw, MAIL_RAW_OUT_MAX, NULL, acct.email,
                                     draft.to, draft.cc, draft.subject,
                                     draft.in_reply_to, draft.references, date, msgid, draft.body);
    if (smtp_start(&sm, acct.email, password, acct.email, draft.to, draft.cc, raw, rlen) == 0) {
        smtp_busy = true;
        smtp_conn_up = false;
        conn_start(CONN_CH_SMTP, eff_smtp_host(), eff_smtp_port(), acct.use_tls != 0);
        dirty = true;
    }
    free(raw);
}

static void save_draft_to_server(void)
{
    set_focus(NULL);
    save_draft();
    if (phase == P_ONLINE && im.active_cmd == ICMD_NONE && draft.active) {
        char *raw = malloc(MAIL_RAW_OUT_MAX);
        if (raw) {
            char date[40];
            rfc_date_now(date, sizeof(date));
            size_t rlen = mime_build_message(raw, MAIL_RAW_OUT_MAX, NULL, acct.email,
                                             draft.to, draft.cc, draft.subject,
                                             draft.in_reply_to, draft.references, date, NULL, draft.body);
            imap_append_draft(&im, raw, rlen);
            free(raw);
        }
    }
    go(compose_return_view, false);
}

/* ---- Touch and Physical Keyboard Handling ------------------------------- */

static struct ui_scroll *active_scroll(gfx_rect *area_out)
{
    int vh = UI_H - UI_NAV_H - active_kb_h();
    switch (view) {
    case V_SIGNON:
        *area_out = gfx_r(0, UI_NAV_H, UI_W, vh);
        return &signon_sc;
    case V_SERVER:
        *area_out = gfx_r(0, UI_NAV_H, UI_W, vh);
        return &server_sc;
    case V_MAILBOXES:
        *area_out = gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H - TOOLBAR_H);
        return &mbox_sc;
    case V_MESSAGE:
        *area_out = gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H - TOOLBAR_H);
        return &msg_sc;
    case V_COMPOSE:
        *area_out = gfx_r(0, UI_NAV_H, UI_W, comp_view_h());
        return &comp_sc;
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
    case V_LIST:
        set_focus(NULL);
        search_open = false;
        go(V_MAILBOXES, false);
        break;
    case V_MESSAGE:
        go(V_LIST, false);
        break;
    case V_COMPOSE:
        if (show_keyboard()) {
            blur();
        } else if (draft.to[0] || draft.subject[0] || draft.body[0]) {
            show_alert(ALERT_DRAFT_CLOSE, "Save Draft?", "Save this message as a draft\nbefore closing?");
        } else {
            go(compose_return_view, false);
        }
        break;
    default:
        break;
    }
}

static void activate(const struct hit *h, int x, int y)
{
    switch (h->id) {
    case H_FIELD:
        if (h->f == &f_body) {
            int body_top = UI_NAV_H - comp_sc.offset + COMP_HDR_H + 6;
            int line = (y - body_top) / LINE_H;
            layout_compose();
            if (line < 0)
                line = 0;
            f_body.cursor = ui_text_hit(ui_font(FONT_R13), draft.body, comp_lines, ncomp_lines, line, x - MARGIN);
        }
        set_focus(h->f);
        break;

    case H_SIGNIN:
        start_signin();
        break;

    case H_SERVER:
        set_focus(NULL);
        ui_scroll_reset(&server_sc, 286, UI_H - UI_NAV_H);
        go(V_SERVER, true);
        break;

    case H_SAVEPW:
        acct.save_password = !acct.save_password;
        save_account();
        break;

    case H_OFFLINE:
        set_focus(NULL);
        im.cur_mbox = MBOX_INBOX;
        im.in_search = false;
        go(V_LIST, true);
        break;

    case H_TLS:
        acct.use_tls = !acct.use_tls;
        save_account();
        break;

    case H_SOUNDS:
        acct.sounds = !acct.sounds;
        save_account();
        break;

    case H_RESET_DEFAULTS:
        set_focus(NULL);
        reset_server_defaults();
        save_account();
        break;

    case H_BACK:
        back();
        break;

    case H_LEFT:
        if (view == V_MAILBOXES) {
            sign_out();
        } else if (view == V_COMPOSE) {
            back();
        }
        break;

    case H_MBOX_ROW:
        search_open = false;
        im.in_search = false;
        im.cur_mbox = h->arg;
        if (phase == P_ONLINE)
            imap_select(&im, h->arg);
        ui_list_reset(&mlist, current_msg_count());
        go(V_LIST, true);
        break;

    case H_REFRESH:
        if (phase == P_ONLINE) {
            imap_refresh(&im);
        } else if (acct.email[0] && password[0]) {
            start_signin();
        } else {
            go(V_SIGNON, false);
        }
        break;

    case H_SEARCH_TOGGLE:
        if (view == V_MAILBOXES) {
            search_open = true;
            set_focus(&f_search);
            go(V_LIST, true);
        } else {
            search_open = !search_open;
            if (search_open) {
                set_focus(&f_search);
            } else {
                set_focus(NULL);
                if (im.in_search) {
                    im.in_search = false;
                    if (phase == P_ONLINE)
                        imap_select(&im, im.cur_mbox);
                }
            }
            sync_list_area();
        }
        break;

    case H_SEARCH_GO:
        set_focus(NULL);
        if (search_buf[0] && phase == P_ONLINE)
            imap_search(&im, search_buf);
        break;

    case H_COMPOSE:
        start_compose(0);
        break;

    case H_PREV_MSG:
        if (cur_msg_idx > 0)
            open_message_at(cur_msg_idx - 1, false);
        break;

    case H_NEXT_MSG:
        if (cur_msg_idx + 1 < current_msg_count())
            open_message_at(cur_msg_idx + 1, false);
        break;

    case H_STAR_CUR:
    case H_MSG_STAR: {
        bool on = !(im.cur_msg.hdr.flags & MAIL_F_FLAGGED);
        if (phase == P_ONLINE) {
            imap_set_flag(&im, im.cur_msg.hdr.uid, MAIL_F_FLAGGED, on);
        } else {
            if (on) im.cur_msg.hdr.flags |= MAIL_F_FLAGGED;
            else im.cur_msg.hdr.flags &= (uint8_t)~MAIL_F_FLAGGED;
            struct mail_summary *s = current_msg_at(cur_msg_idx);
            if (s) s->flags = im.cur_msg.hdr.flags;
            save_cache();
        }
        break;
    }

    case H_MSG_SEEN: {
        bool on = !(im.cur_msg.hdr.flags & MAIL_F_SEEN);
        if (phase == P_ONLINE) {
            imap_set_flag(&im, im.cur_msg.hdr.uid, MAIL_F_SEEN, on);
        } else {
            if (on) im.cur_msg.hdr.flags |= MAIL_F_SEEN;
            else im.cur_msg.hdr.flags &= (uint8_t)~MAIL_F_SEEN;
            struct mail_summary *s = current_msg_at(cur_msg_idx);
            if (s) s->flags = im.cur_msg.hdr.flags;
            save_cache();
        }
        update_badge();
        break;
    }

    case H_MSG_REPLY:
        show_alert(ALERT_REPLY_KIND, "Respond", "Reply to sender or forward\nthis message?");
        break;

    case H_MSG_ARCHIVE:
        if (phase == P_ONLINE) {
            imap_archive(&im, im.cur_msg.hdr.uid);
            go(V_LIST, false);
        } else {
            show_alert(ALERT_INFO, "Offline", "Sign in to archive messages on the server.");
        }
        break;

    case H_MSG_DELETE:
        if (phase == P_ONLINE) {
            imap_delete(&im, im.cur_msg.hdr.uid);
            go(V_LIST, false);
        } else {
            show_alert(ALERT_INFO, "Offline", "Sign in to delete messages on the server.");
        }
        break;

    case H_SEND:
        send_current_draft();
        break;

    case H_SAVE_DRAFT:
        save_draft_to_server();
        break;

    case H_DISCARD_DRAFT:
        clear_draft();
        set_focus(NULL);
        go(compose_return_view, false);
        break;
    }
    dirty = true;
}

static void field_changed(struct field *f)
{
    if (f == &f_email || f == &f_pass || f == &f_ihost || f == &f_iport || f == &f_shost || f == &f_sport)
        save_account();
    if (f == &f_to || f == &f_cc || f == &f_subj || f == &f_body) {
        draft_dirty = true;
        last_draft_edit = gz_time_ms();
        if (view == V_COMPOSE)
            keep_comp_cursor_visible();
    }
}

static void field_return(struct field *f)
{
    if (f == &f_email)
        set_focus(&f_pass);
    else if (f == &f_pass)
        start_signin();
    else if (f == &f_ihost)
        set_focus(&f_iport);
    else if (f == &f_iport)
        set_focus(&f_shost);
    else if (f == &f_shost)
        set_focus(&f_sport);
    else if (f == &f_sport)
        blur();
    else if (f == &f_search) {
        set_focus(NULL);
        if (search_buf[0] && phase == P_ONLINE)
            imap_search(&im, search_buf);
    } else if (f == &f_to)
        set_focus(&f_subj);
    else if (f == &f_cc)
        set_focus(&f_subj);
    else if (f == &f_subj) {
        f_body.cursor = (int)strlen(draft.body);
        set_focus(&f_body);
    }
}


static void field_key(int ch)
{
    struct field *f = focus;
    if (!f) return;
    if (ch == '\n' && !f->multiline) { field_return(f); return; }
    if (ui_edit_key(f->buf, f->cap, &f->cursor, ch, field_flags(f)) && ui_edit_modified())
        field_changed(f);
}

static void handle_alert_touch(const struct gz_input_event *e)
{
    int b = gfx_rect_contains(alert_r1, e->x, e->y) ? 0
          : (alert_r2.w > 0 && gfx_rect_contains(alert_r2, e->x, e->y)) ? 1 : -1;

    if (e->type != GZ_EV_TOUCH_UP) {
        alert_pressed = b;
        dirty = true;
        return;
    }
    int kind = alert_kind;
    alert_kind = ALERT_NONE;
    alert_pressed = -1;
    dirty = true;
    if (b < 0)
        return;
    if (kind == ALERT_REPLY_KIND) {
        start_compose(b == 0 ? 1 : 2);
    } else if (kind == ALERT_DRAFT_CLOSE) {
        if (b == 0) {
            save_draft_to_server();
        } else {
            clear_draft();
            set_focus(NULL);
            go(compose_return_view, false);
        }
    }
}

static void touch_event(const struct gz_input_event *e)
{
    if (alert_kind != ALERT_NONE) {
        handle_alert_touch(e);
        return;
    }

    gfx_rect area;
    struct ui_scroll *sc = active_scroll(&area);
    int kb_y = UI_H - UI_KB_H;
    static bool in_scroll, in_list;

    if (e->type == GZ_EV_TOUCH_CANCEL) {
        /* Home mid-touch: drop the touch without acting on it. */
        bool d = false;
        if (in_keyboard)
            ui_keyboard_event(&kb, e, kb_y, &d);
        if (in_list)
            ui_list_event(&mlist, e, &d);
        if (in_scroll && sc)
            ui_scroll_event(sc, e, 0);
        in_keyboard = in_scroll = in_list = false;
        pressed = (struct hit){ { 0, 0, 0, 0 }, H_NONE, 0, NULL };
        dirty = true;
        return;
    }
    if (e->type == GZ_EV_TOUCH_DOWN) {
        in_keyboard = show_keyboard() && e->y >= kb_y;
        in_scroll = in_list = false;
        pressed = (struct hit){ { 0, 0, 0, 0 }, H_NONE, 0, NULL };
        if (!in_keyboard) {
            pressed = hit_at(e->x, e->y);
            if (sc && gfx_rect_contains(area, e->x, e->y)) {
                in_scroll = true;
                ui_scroll_event(sc, e, e->y - area.y);
            }
            if (view == V_LIST && pressed.id == H_NONE && gfx_rect_contains(mlist.area, e->x, e->y)) {
                bool d = false;
                in_list = true;
                ui_list_event(&mlist, e, &d);
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
        int r = ui_list_event(&mlist, e, &d);
        if (d)
            dirty = true;
        if (r >= 0)
            open_message_at(r, true);
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
        activate(&h, e->x, e->y);
        return;
    }
    if (h.id == H_NONE && focus && !ui_has_hardware_keyboard())
        blur();
}

static void physical_event(const struct gz_input_event *e)
{
    if (e->type == GZ_EV_KEY_DOWN && e->code == GZ_KEY_ESC) {
        if (alert_kind != ALERT_NONE)
            alert_kind = ALERT_NONE;
        else if (focus && !ui_has_hardware_keyboard())
            blur();
        else
            back();
        dirty = true;
        return;
    }
    if (alert_kind != ALERT_NONE) {
        if (e->type == GZ_EV_KEY_DOWN && (e->code == GZ_KEY_ENTER || e->code == GZ_KEY_OK)) {
            int kind = alert_kind;
            alert_kind = ALERT_NONE;
            if (kind == ALERT_REPLY_KIND)
                start_compose(1);
            else if (kind == ALERT_DRAFT_CLOSE)
                save_draft_to_server();
            dirty = true;
        }
        return;
    }
    if (!focus)
        return;

    int ch = ui_input_char(e);
    if (ch == '\n' && !focus->multiline) {
        field_return(focus);
        dirty = true;
        return;
    }
    if (focus->digits && ch && ch != '\b' && (ch < '0' || ch > '9'))
        return;
    if (ui_edit_event(focus->buf, focus->cap, &focus->cursor, e, field_flags(focus))) {
        if (ui_edit_modified()) field_changed(focus);
        dirty = true;
    }
}

int main(void)
{
    ui_init(SB_STATUS_GRAY);
    ui_set_background_running(true);    /* keep checking for new mail after Home */
    load_account();
    load_cache();
    load_draft();
    update_badge();

    ui_list_init(&mlist, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H - TOOLBAR_H), MSG_ROW_H);
    imap_init(&im, &(struct imap_ops){ NULL, imap_send_hook, on_imap_event });
    smtp_init(&sm, &(struct smtp_ops){ NULL, smtp_send_hook, on_smtp_event });

    view = V_SIGNON;
    redraw();

    for (;;) {
        gfx_rect area;
        struct ui_scroll *sc = active_scroll(&area);
        bool sounding = sound_pump();
        int timeout = -1;
        struct gz_input_event ev;

        if (phase != P_OFFLINE || smtp_busy)
            timeout = 100;
        if (draft_dirty)
            timeout = timeout < 0 || SAVE_IDLE_MS < timeout ? SAVE_IDLE_MS : timeout;
        if (sounding)
            timeout = 50;
        if ((sc && sc->animating) || (view == V_LIST && mlist.sc.animating))
            timeout = 30;

        if (ui_next_event(&ev, timeout)) {
            if (ev.type == GZ_EV_APP_ACTIVATE) {
                dirty = true;
            } else if (ev.type == GZ_EV_APP_DEACTIVATE) {
                /* Home: a background app may be ended, so save now. */
                pressed.id = H_NONE;
                in_keyboard = false;
                if (draft_dirty)
                    save_draft();
                if (view == V_SIGNON || view == V_SERVER)
                    save_account();
            } else if (ev.type == GZ_EV_TEXT || ev.type == GZ_EV_KEY_DOWN || ev.type == GZ_EV_KEY_UP)
                physical_event(&ev);
            else
                touch_event(&ev);
        }

        if (sc && ui_scroll_step(sc))
            dirty = true;
        if (view == V_LIST && ui_scroll_step(&mlist.sc))
            dirty = true;

        if (draft_dirty && gz_time_ms() - last_draft_edit >= SAVE_IDLE_MS)
            save_draft();

        service_network();
        imap_tick(&im, gz_time_ms());

        if (dirty)
            redraw();
    }
}
