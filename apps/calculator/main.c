/*
 * Calculator. Values are decimal (mantissa x 10^exp) so 0.1 + 0.2 shows
 * 0.3, with nine significant digits on screen like the original.
 */

#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <string.h>

/* ---- decimal arithmetic ------------------------------------------------- */

typedef struct {
    int64_t m;
    int e;
} dec;

#define LIMIT       100000000000000000LL    /* 10^17: mantissa bound */

static int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

static dec dnorm(dec d)
{
    while (iabs64(d.m) >= LIMIT) {
        d.m = (d.m + (d.m < 0 ? -5 : 5)) / 10;
        d.e++;
    }
    while (d.m && d.m % 10 == 0) {
        d.m /= 10;
        d.e++;
    }
    if (!d.m)
        d.e = 0;
    return d;
}

/* Reduce the mantissa to at most `digits` significant digits (rounded). */
static dec dround(dec d, int digits)
{
    int64_t lim = 1;

    for (int i = 0; i < digits; i++)
        lim *= 10;
    while (iabs64(d.m) >= lim) {
        d.m = (d.m + (d.m < 0 ? -5 : 5)) / 10;
        d.e++;
    }
    return d;
}

static dec dadd(dec a, dec b)
{
    if (!a.m)
        return b;
    if (!b.m)
        return a;
    while (a.e > b.e) {
        if (iabs64(a.m) < LIMIT / 10) {
            a.m *= 10;
            a.e--;
        } else {
            b.m /= 10;
            b.e++;
        }
    }
    while (b.e > a.e) {
        if (iabs64(b.m) < LIMIT / 10) {
            b.m *= 10;
            b.e--;
        } else {
            a.m /= 10;
            a.e++;
        }
    }
    return dnorm((dec){ a.m + b.m, a.e });
}

static dec dneg(dec a) { return (dec){ -a.m, a.e }; }

static dec dmul(dec a, dec b)
{
    a = dround(a, 9);
    b = dround(b, 9);
    return dnorm((dec){ a.m * b.m, a.e + b.e });
}

static bool ddiv(dec a, dec b, dec *out)
{
    if (!b.m)
        return false;
    b = dround(b, 9);
    while (a.m && iabs64(a.m) < LIMIT / 10) {
        a.m *= 10;
        a.e--;
    }
    *out = dnorm((dec){ a.m / b.m, a.e - b.e });
    return true;
}

static dec dparse(const char *s)
{
    dec d = { 0, 0 };
    bool neg = false, frac = false;

    if (*s == '-') {
        neg = true;
        s++;
    }
    for (; *s; s++) {
        if (*s == '.') {
            frac = true;
        } else if (*s >= '0' && *s <= '9') {
            d.m = d.m * 10 + (*s - '0');
            if (frac)
                d.e--;
        }
    }
    if (neg)
        d.m = -d.m;
    return dnorm(d);
}

/* Format with up to nine significant digits and thousands separators. */
static void dformat(dec d, char *out, size_t n)
{
    char digits[24], plain[48];
    bool neg = d.m < 0;

    d = dnorm(dround(d, 9));
    {
        int64_t m = iabs64(d.m);
        int len = 0;
        char rev[24];
        do {
            rev[len++] = (char)('0' + (int)(m % 10));
            m /= 10;
        } while (m);
        for (int i = 0; i < len; i++)
            digits[i] = rev[len - 1 - i];
        digits[len] = 0;
    }
    int nd = (int)strlen(digits);
    int point = nd + d.e;              /* digits before the decimal point */

    if (d.m == 0) {
        snprintf(out, n, "0");
        return;
    }
    if (point > 9 || point < -7) {
        /* Scientific: d.ddddde+x */
        int exp10 = point - 1;
        snprintf(plain, sizeof(plain), "%s%c%s%se%d", neg ? "-" : "", digits[0], nd > 1 ? "." : "",
                 nd > 1 ? digits + 1 : "", exp10);
        snprintf(out, n, "%s", plain);
        return;
    }
    int p = 0;
    if (neg)
        plain[p++] = '-';
    if (point <= 0) {
        plain[p++] = '0';
        plain[p++] = '.';
        for (int i = 0; i < -point; i++)
            plain[p++] = '0';
        for (int i = 0; i < nd; i++)
            plain[p++] = digits[i];
    } else {
        for (int i = 0; i < point; i++) {
            plain[p++] = i < nd ? digits[i] : '0';
            int left = point - 1 - i;
            if (left > 0 && left % 3 == 0)
                plain[p++] = ',';
        }
        if (nd > point) {
            plain[p++] = '.';
            for (int i = point; i < nd; i++)
                plain[p++] = digits[i];
        }
    }
    plain[p] = 0;
    snprintf(out, n, "%s", plain);
}

/* ---- calculator state --------------------------------------------------- */

enum { OP_NONE, OP_ADD, OP_SUB, OP_MUL, OP_DIV };

static dec acc, memory, last_operand;
static int op = OP_NONE, last_op = OP_NONE;
static char entry[16];          /* digits being typed, "" when showing acc */
static bool error;
static bool has_memory;

static dec current(void)
{
    return entry[0] ? dparse(entry) : acc;
}

static bool apply(int o, dec a, dec b, dec *out)
{
    switch (o) {
    case OP_ADD: *out = dadd(a, b); return true;
    case OP_SUB: *out = dadd(a, dneg(b)); return true;
    case OP_MUL: *out = dmul(a, b); return true;
    case OP_DIV: return ddiv(a, b, out);
    default: *out = b; return true;
    }
}

static void press_digit(char c)
{
    size_t len = strlen(entry);
    int digits = 0;

    for (const char *p = entry; *p; p++)
        digits += *p >= '0' && *p <= '9';
    if (error) {
        error = false;
        acc = (dec){ 0, 0 };
    }
    if (c == '.' && strchr(entry, '.'))
        return;
    if (c != '.' && digits >= 9)
        return;
    if (!len && c == '.')
        strcpy(entry, "0");
    if (!strcmp(entry, "0") && c != '.')
        entry[0] = 0;
    len = strlen(entry);
    entry[len] = c;
    entry[len + 1] = 0;
}

static void press_op(int o)
{
    if (error)
        return;
    if (entry[0]) {
        dec v = dparse(entry);
        if (op != OP_NONE) {
            if (!apply(op, acc, v, &acc))
                error = true;
        } else {
            acc = v;
        }
        entry[0] = 0;
    }
    op = o;
    last_op = OP_NONE;
}

static void press_equals(void)
{
    if (error)
        return;
    if (op != OP_NONE) {
        dec v = current();
        if (!entry[0])
            v = acc;
        last_op = op;
        last_operand = v;
        if (!apply(op, acc, v, &acc))
            error = true;
        op = OP_NONE;
    } else if (last_op != OP_NONE) {
        if (!apply(last_op, current(), last_operand, &acc))
            error = true;
    } else {
        acc = current();
    }
    entry[0] = 0;
}

static void press_clear(void)
{
    if (entry[0]) {
        entry[0] = 0;           /* C clears the entry; a second C clears all */
        return;
    }
    acc = (dec){ 0, 0 };
    op = last_op = OP_NONE;
    error = false;
}

static void press_sign(void)
{
    if (entry[0]) {
        if (entry[0] == '-')
            memmove(entry, entry + 1, strlen(entry));
        else if (strlen(entry) < sizeof(entry) - 1) {
            memmove(entry + 1, entry, strlen(entry) + 1);
            entry[0] = '-';
        }
    } else {
        acc = dneg(acc);
    }
}

static void press_memory(int which)
{
    switch (which) {
    case 0: memory = (dec){ 0, 0 }; has_memory = false; break;                 /* mc */
    case 1: memory = dadd(memory, current()); has_memory = true; entry[0] = 0; acc = current(); break; /* m+ */
    case 2: memory = dadd(memory, dneg(current())); has_memory = true; entry[0] = 0; break;           /* m- */
    case 3: acc = memory; entry[0] = 0; break;                                  /* mr */
    }
    if (which == 1 || which == 2)
        has_memory = memory.m != 0;
}

/* ---- layout ------------------------------------------------------------- */

enum kind { K_FUNC, K_OP, K_DIGIT, K_EQUALS };

struct key {
    const char *label;
    int row, col, rows, cols;
    enum kind kind;
    int code;
};

static const struct key keys[] = {
    { "mc", 0, 0, 1, 1, K_FUNC, 100 }, { "m+", 0, 1, 1, 1, K_FUNC, 101 },
    { "m" GFX_CH_MINUS, 0, 2, 1, 1, K_FUNC, 102 }, { "mr", 0, 3, 1, 1, K_FUNC, 103 },
    { "C", 1, 0, 1, 1, K_FUNC, 'C' }, { "+/" GFX_CH_MINUS, 1, 1, 1, 1, K_FUNC, 'S' },
    { GFX_CH_DIVIDE, 1, 2, 1, 1, K_OP, OP_DIV }, { GFX_CH_TIMES, 1, 3, 1, 1, K_OP, OP_MUL },
    { "7", 2, 0, 1, 1, K_DIGIT, '7' }, { "8", 2, 1, 1, 1, K_DIGIT, '8' }, { "9", 2, 2, 1, 1, K_DIGIT, '9' },
    { GFX_CH_MINUS, 2, 3, 1, 1, K_OP, OP_SUB },
    { "4", 3, 0, 1, 1, K_DIGIT, '4' }, { "5", 3, 1, 1, 1, K_DIGIT, '5' }, { "6", 3, 2, 1, 1, K_DIGIT, '6' },
    { "+", 3, 3, 1, 1, K_OP, OP_ADD },
    { "1", 4, 0, 1, 1, K_DIGIT, '1' }, { "2", 4, 1, 1, 1, K_DIGIT, '2' }, { "3", 4, 2, 1, 1, K_DIGIT, '3' },
    { "=", 4, 3, 2, 1, K_EQUALS, '=' },
    { "0", 5, 0, 1, 2, K_DIGIT, '0' }, { ".", 5, 2, 1, 1, K_DIGIT, '.' },
};
#define NKEYS ((int)(sizeof(keys) / sizeof(keys[0])))

#define DISPLAY_H   78
#define PAD_TOP     (DISPLAY_H + 4)
#define ROW_H       37
#define COL_W       59

static gfx_rect key_rect(const struct key *k)
{
    return gfx_r(2 + k->col * COL_W, PAD_TOP + k->row * ROW_H, k->cols * COL_W - 3, k->rows * ROW_H - 3);
}

static int pressed = -1;

static void draw_display(gfx_surface *s)
{
    char text[48];
    const int fonts[] = { FONT_R44, FONT_R24, FONT_R17 };

    gfx_vgradient(s, gfx_r(0, 0, UI_W, DISPLAY_H), 0x3B3B3B, 0x0D0D0D, 255);
    gfx_vgradient(s, gfx_r(0, 0, UI_W, DISPLAY_H / 2), 0xFFFFFF, 0xFFFFFF, 12);
    if (error)
        strcpy(text, "Error");
    else if (entry[0]) {
        /* Show the entry with separators by round-tripping through dec,
         * keeping a trailing "." or zeros the user typed. */
        const char *dot = strchr(entry, '.');
        if (dot) {
            char intpart[16];
            size_t n = (size_t)(dot - entry);
            memcpy(intpart, entry, n);
            intpart[n] = 0;
            dformat(dparse(intpart[0] && strcmp(intpart, "-") ? intpart : "0"), text, sizeof(text));
            if (entry[0] == '-' && text[0] != '-') {
                memmove(text + 1, text, strlen(text) + 1);
                text[0] = '-';
            }
            strncat(text, dot, sizeof(text) - strlen(text) - 1);
        } else {
            dformat(dparse(entry), text, sizeof(text));
        }
    } else {
        dformat(acc, text, sizeof(text));
    }
    const gfx_font *f = ui_font(FONT_R44);
    for (unsigned i = 0; i < 3; i++) {
        f = ui_font(fonts[i]);
        if (gfx_text_width(f, text) <= UI_W - 20)
            break;
    }
    gfx_text(s, UI_W - 12 - gfx_text_width(f, text), (DISPLAY_H - f->line_height) / 2 + 2, f, text, 0xFFFFFF, 255);
    if (has_memory)
        gfx_text(s, 10, 6, ui_font(FONT_B11), "M", 0xBBBBBB, 255);
}

static void draw_key(gfx_surface *s, int i)
{
    const struct key *k = &keys[i];
    gfx_rect r = key_rect(k);
    uint32_t top, bottom, text = 0xFFFFFF;
    bool active_op = k->kind == K_OP && op == k->code && !entry[0];

    switch (k->kind) {
    case K_DIGIT: top = 0x6E6E6E; bottom = 0x3A3A3A; break;
    case K_OP: top = 0x575757; bottom = 0x262626; break;
    case K_EQUALS: top = 0xFFA63D; bottom = 0xE0680A; break;
    default: top = 0x464646; bottom = 0x1C1C1C; text = 0xD8D8D8; break;
    }
    gfx_round_rect(s, r, 6, 0x000000, 255);
    gfx_round_rect_vgradient(s, gfx_r(r.x + 1, r.y + 1, r.w - 2, r.h - 2), 5, top, bottom, 255);
    gfx_round_rect(s, gfx_r(r.x + 1, r.y + 1, r.w - 2, (r.h - 2) / 2), 5, 0xFFFFFF, 28);
    if (active_op)
        gfx_round_rect(s, gfx_r(r.x + 1, r.y + 1, r.w - 2, r.h - 2), 5, 0xFFFFFF, 60);
    if (pressed == i)
        gfx_round_rect(s, r, 6, 0xFFFFFF, 90);
    const gfx_font *f = k->kind == K_FUNC ? ui_font(FONT_B14) : ui_font(FONT_B20);
    gfx_text_center(s, r.x, r.y + (r.h - f->line_height) / 2, r.w, f, k->label, text, 255);
}

static void draw(void)
{
    gfx_surface *s = &ui.surf;

    gfx_fill(s, gfx_r(0, DISPLAY_H, UI_W, UI_H - DISPLAY_H), 0x000000);
    draw_display(s);
    for (int i = 0; i < NKEYS; i++)
        draw_key(s, i);
    ui_invalidate_all();
}

static int hit(int x, int y)
{
    for (int i = 0; i < NKEYS; i++) {
        if (gfx_rect_contains(key_rect(&keys[i]), x, y))
            return i;
    }
    return -1;
}

static void activate(const struct key *k)
{
    switch (k->kind) {
    case K_DIGIT: press_digit((char)k->code); break;
    case K_OP: press_op(k->code); break;
    case K_EQUALS: press_equals(); break;
    case K_FUNC:
        if (k->code == 'C')
            press_clear();
        else if (k->code == 'S')
            press_sign();
        else
            press_memory(k->code - 100);
        break;
    }
}

int main(void)
{
    ui_init(SB_STATUS_BLACK);
    draw();
    ui_present();

    for (;;) {
        struct gz_input_event ev;
        if (!ui_next_event(&ev, -1))
            continue;
        bool redraw = false;
        if (ev.type == GZ_EV_TOUCH_DOWN || ev.type == GZ_EV_TOUCH_MOVE) {
            int h = hit(ev.x, ev.y);
            if (ev.type == GZ_EV_TOUCH_MOVE && h != pressed)
                h = -1;
            if (h != pressed) {
                pressed = h;
                redraw = true;
            }
        } else if (ev.type == GZ_EV_TOUCH_UP || ev.type == GZ_EV_TOUCH_CANCEL) {
            if (pressed >= 0 && ev.type == GZ_EV_TOUCH_UP)
                activate(&keys[pressed]);
            pressed = -1;
            redraw = true;
        }
        if (redraw) {
            draw();
            ui_present();
        }
    }
}
