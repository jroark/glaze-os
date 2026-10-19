/*
 * Web: fetches a page over HTTP/1.0 and shows its text. Markup is reduced
 * to paragraphs and line breaks; scripts, styles and images are dropped.
 * There is no TLS, so https:// addresses are refused. The last address is
 * kept in the key/value store under "web.url".
 */

#include <glaze/net.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <string.h>

#define BAR_H       40
#define MARGIN      10
#define LINE_H      17
#define URL_MAX     200
#define RAW_MAX     (96 * 1024)
#define TEXT_MAX    (48 * 1024)
#define MAX_LINES   3000
#define REDIRECTS   3

static char url[URL_MAX + 1] = "http://example.com/";
static char title[96];
static char text[TEXT_MAX + 1];
static char raw[RAW_MAX + 1];
static int line_starts[MAX_LINES];
static int nlines;
static bool editing;
static bool pressed_go, pressed_clear, in_keyboard;
static struct ui_keyboard kb;
static struct ui_scroll scroll;
static gfx_rect field_rect, go_rect, clear_rect;

/* ---- HTTP ---------------------------------------------------------------- */

struct target {
    char host[NET_HOST_MAX];
    uint16_t port;
    const char *path;
};

static int parse_url(const char *u, struct target *t)
{
    if (!strncmp(u, "https://", 8))
        return -2;
    if (!strncmp(u, "http://", 7))
        u += 7;
    size_t n = strcspn(u, ":/");
    if (!n || n >= sizeof(t->host))
        return -1;
    memcpy(t->host, u, n);
    t->host[n] = 0;
    u += n;
    t->port = 80;
    if (*u == ':') {
        int p = 0;
        for (u++; *u >= '0' && *u <= '9'; u++)
            p = p * 10 + (*u - '0');
        if (p <= 0 || p > 65535)
            return -1;
        t->port = (uint16_t)p;
    }
    t->path = *u ? u : "/";
    return *t->path == '/' ? 0 : -1;
}

/* The response, headers and all, into raw[]; its length or an error. */
static int http_get(const struct target *t)
{
    char req[URL_MAX + NET_HOST_MAX + 96];
    uint32_t addr;
    int rv = net_resolve(t->host, &addr, 8000);

    if (rv < 0)
        return rv;
    int s = net_socket(NET_TCP);
    if (s < 0)
        return s;
    rv = net_connect(s, addr, t->port, 10000);
    if (rv == 0) {
        snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Glaze/1.0 (BE-300)\r\n"
                 "Accept: text/html, text/plain\r\nConnection: close\r\n\r\n", t->path, t->host);
        rv = net_send(s, req, (uint32_t)strlen(req));
    }
    int n = 0;
    while (rv >= 0 && n < RAW_MAX) {
        rv = net_recv(s, raw + n, (uint32_t)(RAW_MAX - n), 15000);
        if (rv <= 0)
            break;
        n += rv;
    }
    net_close(s);
    raw[n] = 0;
    return rv < 0 && !n ? rv : n;
}

/* A header's value (case-insensitive name) copied into out, or NULL. */
static char *header(const char *head, const char *name, char *out, size_t max)
{
    size_t nl = strlen(name);

    for (const char *p = strchr(head, '\n'); p && p[1]; p = strchr(p + 1, '\n')) {
        p++;
        if (*p == '\r' || *p == '\n')
            break;
        size_t i = 0;
        while (i < nl && p[i] && (p[i] | 0x20) == (name[i] | 0x20))
            i++;
        if (i == nl && p[i] == ':') {
            p += i + 1;
            while (*p == ' ')
                p++;
            size_t n = strcspn(p, "\r\n");
            n = MIN(n, max - 1);
            memcpy(out, p, n);
            out[n] = 0;
            return out;
        }
    }
    return NULL;
}

/* ---- markup to text ------------------------------------------------------- */

static int out_len;
static bool out_space;      /* a pending collapsed space */

static void emit(char c)
{
    if (out_len >= TEXT_MAX)
        return;
    if (c == '\n') {
        /* At most one blank line in a row. */
        while (out_len && text[out_len - 1] == ' ')
            out_len--;
        if (out_len >= 2 && text[out_len - 1] == '\n' && text[out_len - 2] == '\n')
            return;
        if (out_len)
            text[out_len++] = '\n';
        out_space = false;
        return;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
        out_space = out_len && text[out_len - 1] != '\n';
        return;
    }
    if (out_space && out_len < TEXT_MAX)
        text[out_len++] = ' ';
    out_space = false;
    text[out_len++] = c;
}

/* UTF-8 sequences become the nearest ASCII the fonts can show. */
static const char *utf8(const char *p)
{
    unsigned c = (unsigned char)*p;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    unsigned cp = c & (0x3F >> extra);

    for (int i = 0; i < extra && (p[1] & 0xC0) == 0x80; i++)
        cp = cp << 6 | (*++p & 0x3F);
    if (cp == 0x2018 || cp == 0x2019)
        emit('\'');
    else if (cp == 0x201C || cp == 0x201D)
        emit('"');
    else if (cp == 0x2013 || cp == 0x2014)
        emit('-');
    else if (cp == 0x2026)
        emit('.'), emit('.'), emit('.');
    else if (cp == 0xA0)
        emit(' ');
    else
        emit('?');
    return p + 1;
}

static const char *entity(const char *p)
{
    static const struct { const char *name; char c; } names[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
        { "nbsp", ' ' }, { "copy", 'c' }, { "mdash", '-' }, { "ndash", '-' }, { "ldquo", '"' },
        { "rdquo", '"' }, { "lsquo", '\'' }, { "rsquo", '\'' }, { "bull", '*' }, { "middot", '.' },
    };
    static const char *const accents[] = { "acute", "grave", "circ", "uml", "tilde", "cedil", "ring", "slash" };
    const char *end = strchr(p, ';');

    if (!end || end - p > 10) {
        emit('&');
        return p + 1;
    }
    if (p[1] == '#') {
        unsigned v = 0;
        bool hex = p[2] == 'x' || p[2] == 'X';
        for (const char *q = p + 2 + hex; q < end; q++)
            v = v * (hex ? 16 : 10) + (unsigned)(*q <= '9' ? *q - '0' : (*q | 0x20) - 'a' + 10);
        emit(v >= 32 && v < 127 ? (char)v : v == 160 ? ' ' : '?');
        return end + 1;
    }
    size_t len = (size_t)(end - p - 1);
    for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
        size_t n = strlen(names[i].name);
        if (len == n && !strncmp(p + 1, names[i].name, n)) {
            emit(names[i].c);
            return end + 1;
        }
    }
    /* &eacute; and friends: the bare letter. */
    for (size_t i = 0; i < ARRAY_SIZE(accents); i++) {
        size_t n = strlen(accents[i]);
        if (len == n + 1 && !strncmp(p + 2, accents[i], n)) {
            emit(p[1]);
            return end + 1;
        }
    }
    emit('?');
    return end + 1;
}

static bool tag_is(const char *tag, const char *name)
{
    size_t n = strlen(name);

    for (size_t i = 0; i < n; i++)
        if ((tag[i] | 0x20) != name[i])
            return false;
    return !((tag[n] | 0x20) >= 'a' && (tag[n] | 0x20) <= 'z') && !(tag[n] >= '0' && tag[n] <= '9');
}

static void html_to_text(const char *p)
{
    static const char *const breaks[] = {
        "p", "div", "br", "tr", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol",
        "table", "blockquote", "pre", "hr", "section", "article", "header", "footer", "dd", "dt",
    };

    while (*p) {
        if (*p == '<') {
            const char *t = p + 1;
            bool closing = *t == '/';
            t += closing;
            if (!strncmp(p, "<!--", 4)) {
                const char *e = strstr(p, "-->");
                p = e ? e + 3 : p + strlen(p);
                continue;
            }
            if (!closing && (tag_is(t, "script") || tag_is(t, "style"))) {
                const char *e = strstr(t, tag_is(t, "script") ? "</script" : "</style");
                if (!e)
                    e = strstr(t, tag_is(t, "script") ? "</SCRIPT" : "</STYLE");
                p = e ? e : p + strlen(p);
                const char *gt = strchr(p, '>');
                p = gt ? gt + 1 : p + strlen(p);
                continue;
            }
            if (!closing && tag_is(t, "title")) {
                const char *s = strchr(t, '>'), *e = s ? strchr(s, '<') : NULL;
                if (s && e) {
                    size_t n = MIN((size_t)(e - s - 1), sizeof(title) - 1);
                    memcpy(title, s + 1, n);
                    title[n] = 0;
                    p = e;
                    continue;
                }
            }
            if (!closing && tag_is(t, "li")) {
                emit('\n');
                emit('*');
                emit(' ');
            } else {
                for (size_t i = 0; i < ARRAY_SIZE(breaks); i++) {
                    if (tag_is(t, breaks[i])) {
                        emit('\n');
                        if (breaks[i][0] != 'b' && breaks[i][0] != 't')
                            emit('\n');
                        break;
                    }
                }
            }
            const char *gt = strchr(p, '>');
            p = gt ? gt + 1 : p + strlen(p);
        } else if (*p == '&') {
            p = entity(p);
        } else if ((unsigned char)*p >= 0x80) {
            p = utf8(p);
        } else {
            emit(*p == '\n' ? ' ' : *p);
            p++;
        }
    }
}

static void plain_to_text(const char *p)
{
    while (*p && out_len < TEXT_MAX) {
        if ((unsigned char)*p >= 0x80) {
            p = utf8(p);
            continue;
        }
        if (*p != '\r')
            text[out_len++] = *p == '\t' ? ' ' : *p;
        p++;
    }
}

/* ---- loading ------------------------------------------------------------- */

static void show_message(const char *head, const char *msg)
{
    strlcpy(title, head, sizeof(title));
    strlcpy(text, msg, sizeof(text));
}

static void layout(void);
static void redraw(void);

static void load(void)
{
    char next[URL_MAX + 1], value[URL_MAX + 1];
    struct target t;

    strlcpy(next, url, sizeof(next));
    show_message("Loading" GFX_CH_ELLIPSIS, url);
    layout();
    redraw();

    for (int hop = 0;; hop++) {
        int rv = parse_url(next, &t);
        if (rv == -2) {
            show_message("Can't Open Page", "Secure (https) pages aren't supported.");
            break;
        }
        if (rv < 0) {
            show_message("Can't Open Page", "The address isn't valid.");
            break;
        }
        rv = http_get(&t);
        if (rv < 0) {
            show_message("Can't Open Page", net_strerror(rv));
            break;
        }
        char *body = strstr(raw, "\r\n\r\n");
        body = body ? body + 4 : raw + rv;
        int status = 0;
        if (!strncmp(raw, "HTTP/1.", 7))
            status = (raw[9] - '0') * 100 + (raw[10] - '0') * 10 + (raw[11] - '0');
        if (status >= 300 && status < 400 && hop < REDIRECTS && header(raw, "location", value, sizeof(value))) {
            if (value[0] == '/')
                snprintf(next, sizeof(next), "http://%s:%u%s", t.host, t.port, value);
            else
                strlcpy(next, value, sizeof(next));
            strlcpy(url, next, sizeof(url));
            continue;
        }
        title[0] = 0;
        text[0] = 0;
        out_len = 0;
        out_space = false;
        bool html = !header(raw, "content-type", value, sizeof(value)) || strstr(value, "html");
        if (html)
            html_to_text(body);
        else
            plain_to_text(body);
        text[out_len] = 0;
        if (!title[0])
            strlcpy(title, t.host, sizeof(title));
        if (status != 200) {
            char line[40];
            snprintf(line, sizeof(line), "HTTP %d", status);
            strlcpy(title, line, sizeof(title));
        }
        break;
    }
    gz_kv_put("web.url", url, (uint32_t)strlen(url));
    ui_scroll_reset(&scroll, 0, 0);
    layout();
}

/* ---- drawing ------------------------------------------------------------- */

static int content_h(void)
{
    return UI_H - BAR_H - (editing ? UI_KB_H : 0);
}

static void layout(void)
{
    nlines = ui_wrap(ui_font(FONT_R13), text, UI_W - MARGIN * 2, line_starts, MAX_LINES);
    int h = 34 + nlines * LINE_H + MARGIN;
    int off = scroll.offset;
    ui_scroll_reset(&scroll, h, content_h());
    scroll.offset = MIN(off, ui_scroll_max(&scroll));
}

static void draw(gfx_surface *s)
{
    const gfx_font *uf = ui_font(FONT_R13), *bf = ui_font(FONT_B14), *tf = ui_font(FONT_R13);

    /* Address bar. */
    gfx_rect bar = gfx_r(0, 0, UI_W, BAR_H);
    gfx_vgradient(s, bar, UI_NAV_TOP, UI_NAV_BOTTOM, 255);
    gfx_fill(s, gfx_r(0, BAR_H - 1, UI_W, 1), UI_NAV_LINE);
    field_rect = gfx_r(6, 7, UI_W - 62, 26);
    go_rect = gfx_r(UI_W - 50, 7, 44, 26);
    gfx_round_rect(s, field_rect, 6, 0x4E5F78, 255);
    gfx_round_rect(s, gfx_r(field_rect.x + 1, field_rect.y + 1, field_rect.w - 2, field_rect.h - 2), 5, 0xFFFFFF, 255);
    clear_rect = gfx_r(field_rect.x + field_rect.w - 24, field_rect.y, 24, field_rect.h);
    int fw = field_rect.w - 12 - (editing ? 20 : 0);
    gfx_rect saved = s->clip;
    gfx_set_clip(s, gfx_r(field_rect.x + 6, field_rect.y, fw, field_rect.h));
    int tw = gfx_text_width(uf, url);
    int tx = field_rect.x + 6 - (editing && tw > fw - 3 ? tw - (fw - 3) : 0);
    gfx_text(s, tx, field_rect.y + (field_rect.h - uf->line_height) / 2, uf, url, UI_TEXT, 255);
    if (editing)
        gfx_fill(s, gfx_r(tx + tw + 1, field_rect.y + 5, 2, field_rect.h - 10), 0x3B6FD8);
    s->clip = saved;
    if (editing) {
        int cx = clear_rect.x + 12, cy = clear_rect.y + clear_rect.h / 2;
        gfx_round_rect(s, gfx_r(cx - 7, cy - 7, 14, 14), 7, pressed_clear ? 0x707070 : 0xA8A8A8, 255);
        gfx_text_center(s, cx - 7, cy - 7, 14, ui_font(FONT_B10), GFX_CH_TIMES, 0xFFFFFF, 255);
    }
    gfx_round_rect(s, go_rect, 5, 0x2D3E5C, 255);
    gfx_round_rect_vgradient(s, gfx_r(go_rect.x + 1, go_rect.y + 1, go_rect.w - 2, go_rect.h - 2), 4,
                             pressed_go ? 0x1E4FB0 : 0x5E95F2, pressed_go ? 0x123A8A : 0x2A63D4, 255);
    gfx_text_center(s, go_rect.x, go_rect.y + (go_rect.h - bf->line_height) / 2, go_rect.w, bf, "Go", 0xFFFFFF, 255);

    /* Page. */
    gfx_rect page = gfx_r(0, BAR_H, UI_W, content_h());
    gfx_fill(s, page, 0xFFFFFF);
    gfx_set_clip(s, page);
    int y = page.y + 8 - scroll.offset;
    gfx_text_n(s, MARGIN, y, bf, title, (int)strlen(title), UI_TEXT, 255);
    y += 26;
    for (int l = 0; l < nlines; l++, y += LINE_H) {
        if (y + LINE_H < page.y || y > page.y + page.h)
            continue;
        gfx_text_n(s, MARGIN, y, tf, text + line_starts[l], ui_line_len(text, line_starts, nlines, l),
                   0x202020, 255);
    }
    s->clip = saved;

    if (editing)
        ui_draw_keyboard(s, &kb, UI_H - UI_KB_H);
}

static void redraw(void)
{
    draw(&ui.surf);
    ui_invalidate_all();
    ui_present();
}

/* ---- events -------------------------------------------------------------- */

static void go(void)
{
    editing = false;
    if (!strstr(url, "://")) {
        char full[URL_MAX + 1];
        snprintf(full, sizeof(full), "http://%s", url);
        strlcpy(url, full, sizeof(url));
    }
    load();
}

static void type_char(int ch)
{
    size_t n = strlen(url);

    if (ch == '\n') {
        go();
    } else if (ch == '\b') {
        if (n)
            url[n - 1] = 0;
    } else if (n < URL_MAX) {
        url[n] = (char)ch;
        url[n + 1] = 0;
    }
}

static void event(const struct gz_input_event *e)
{
    int kb_y = UI_H - UI_KB_H;

    if (e->type == GZ_EV_KEY_DOWN) {
        if (e->code == GZ_KEY_ESC && editing) {
            editing = false;
            layout();
        } else if (e->code == GZ_KEY_OK) {
            go();
        } else if (e->code == GZ_KEY_UP || e->code == GZ_KEY_DOWN) {
            int step = (content_h() - LINE_H) * (e->code == GZ_KEY_UP ? -1 : 1);
            scroll.offset = MAX(0, MIN(scroll.offset + step, ui_scroll_max(&scroll)));
        }
        redraw();
        return;
    }
    if (e->type == GZ_EV_TOUCH_DOWN) {
        in_keyboard = editing && e->y >= kb_y;
        pressed_go = gfx_rect_contains(go_rect, e->x, e->y);
        pressed_clear = editing && gfx_rect_contains(clear_rect, e->x, e->y);
    }
    if (in_keyboard) {
        bool changed;
        int ch = ui_keyboard_event(&kb, e, kb_y, &changed);
        if (ch)
            type_char(ch);
        if (ch || changed)
            redraw();
        return;
    }
    if (e->y < BAR_H) {
        if (e->type == GZ_EV_TOUCH_UP) {
            if (pressed_go && gfx_rect_contains(go_rect, e->x, e->y)) {
                pressed_go = false;
                go();
            } else if (pressed_clear && gfx_rect_contains(clear_rect, e->x, e->y)) {
                url[0] = 0;
            } else if (gfx_rect_contains(field_rect, e->x, e->y) && !editing) {
                editing = true;
                ui_keyboard_init(&kb);
                kb.shift = false;           /* addresses are lower case */
                layout();
            }
            pressed_go = pressed_clear = false;
        }
        redraw();
        return;
    }
    if (e->type == GZ_EV_TOUCH_UP && (pressed_go || pressed_clear)) {
        pressed_go = pressed_clear = false;
        redraw();
    }
    if (ui_scroll_event(&scroll, e, e->y - BAR_H))
        redraw();
}

int main(void)
{
    ui_init(SB_STATUS_GRAY);
    int n = gz_kv_get("web.url", url, URL_MAX);
    if (n > 0)
        url[n] = 0;
    show_message("Web", "Tap the address bar to enter a page, then Go.\n\n"
                 "Pages load over plain HTTP; only their text is shown.");
    layout();
    redraw();

    for (;;) {
        struct gz_input_event ev;
        if (ui_next_event(&ev, scroll.animating ? 30 : -1))
            event(&ev);
        if (ui_scroll_step(&scroll))
            redraw();
    }
}
