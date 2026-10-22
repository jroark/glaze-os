/* On-screen QWERTY keyboard (iPhone OS 1-3 style), 240 x 150. */

#include "sound.h"
#include <glaze/audio.h>
#include <glaze/ui.h>
#include <string.h>

#define KEY_SHIFT   1
#define KEY_MODE    2
#define KEY_DOTCOM  3

struct key {
    int16_t x, y, w, h;
    char ch;                /* character, '\b', '\n', ' ', KEY_SHIFT, KEY_MODE */
    const char *label;      /* for special keys */
};

#define MAX_KEYS 40
static struct key keys[MAX_KEYS];
static int nkeys;
static bool built_numbers = true, built_shift = true, built_url;
static char letter_labels[MAX_KEYS][2];

static void add(int x, int y, int w, char ch, const char *label)
{
    struct key *k = &keys[nkeys];
    k->x = (int16_t)x;
    k->y = (int16_t)y;
    k->w = (int16_t)w;
    k->h = 30;
    k->ch = ch;
    if (label) {
        k->label = label;
    } else {
        letter_labels[nkeys][0] = ch;
        letter_labels[nkeys][1] = 0;
        k->label = letter_labels[nkeys];
    }
    nkeys++;
}

static void row(const char *chars, int x, int y, bool upper)
{
    for (; *chars; chars++, x += 23) {
        char c = *chars;
        if (upper && c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        add(x, y, 21, c, NULL);
    }
}

static void build(const struct ui_keyboard *kb)
{
    if (built_numbers == kb->numbers && built_shift == kb->shift && built_url == kb->url && nkeys)
        return;
    built_numbers = kb->numbers;
    built_shift = kb->shift;
    built_url = kb->url;
    nkeys = 0;
    if (kb->numbers) {
        row("1234567890", 6, 6, false);
        row(kb->url ? "-/:;()_&@" : "-/:;()$&@", 17, 42, false);
        row(kb->url ? ".=?#%~" : ".,?!'", 63, 78, false);
        add(6, 78, 50, KEY_MODE, "#+=");
    } else {
        row("qwertyuiop", 6, 6, kb->shift);
        row("asdfghjkl", 17, 42, kb->shift);
        row("zxcvbnm", 40, 78, kb->shift);
        add(6, 78, 28, KEY_SHIFT, "^");
    }
    add(206, 78, 28, '\b', GFX_CH_LARROW);
    add(6, 114, 52, KEY_MODE, kb->numbers ? "ABC" : ".?123");
    if (kb->url) {
        add(62, 114, 36, '/', NULL);
        add(102, 114, 36, '.', NULL);
        add(142, 114, 36, KEY_DOTCOM, ".com");
        add(182, 114, 52, '\n', "Go");
    } else {
        add(62, 114, 116, ' ', "space");
        add(182, 114, 52, '\n', "return");
    }
}

void ui_keyboard_init(struct ui_keyboard *kb)
{
    memset(kb, 0, sizeof(*kb));
    kb->pressed = -1;
    kb->shift = true;           /* sentence starts capitalized */
    nkeys = 0;
}

static bool special(const struct key *k)
{
    return k->ch == KEY_SHIFT || k->ch == KEY_MODE || k->ch == '\b' || k->ch == '\n' || k->ch == KEY_DOTCOM;
}

void ui_draw_keyboard(gfx_surface *s, const struct ui_keyboard *kb, int y)
{
    const gfx_font *lf = ui_font(FONT_R17), *sf = ui_font(FONT_B11);

    build(kb);
    gfx_vgradient(s, gfx_r(0, y, UI_W, UI_KB_H), 0xB7BDC7, 0x7E8696, 255);
    gfx_fill(s, gfx_r(0, y, UI_W, 1), 0x5A606C);
    for (int i = 0; i < nkeys; i++) {
        const struct key *k = &keys[i];
        gfx_rect r = gfx_r(k->x, y + k->y, k->w, k->h);
        bool down = kb->pressed == i;
        bool sp = special(k);
        bool lit = sp && k->ch == KEY_SHIFT && kb->shift;
        gfx_round_rect(s, gfx_r(r.x, r.y + 1, r.w, r.h), 5, 0x3C424D, 200);
        if (sp)
            gfx_round_rect_vgradient(s, r, 5, lit ? 0xFFFFFF : down ? 0x7C8597 : 0xA8B0BE,
                                     lit ? 0xE4E4E4 : down ? 0x5B6374 : 0x87909F, 255);
        else
            gfx_round_rect_vgradient(s, r, 5, down ? 0xC8CDD6 : 0xFFFFFF, down ? 0xA9AFBA : 0xEAEAEA, 255);
        const gfx_font *f = (sp || k->ch == ' ') ? sf : lf;
        uint32_t col = (sp && !lit) ? 0xFFFFFF : 0x111111;
        gfx_text_center(s, r.x, r.y + (r.h - f->line_height) / 2, r.w, f, k->label, col, 255);
    }
    /* Magnified preview of the pressed character key. */
    if (kb->pressed >= 0 && kb->pressed < nkeys) {
        const struct key *k = &keys[kb->pressed];
        if (!special(k) && k->ch != ' ') {
            const gfx_font *bf = ui_font(FONT_R24);
            gfx_rect b = gfx_r(k->x - 6, y + k->y - 40, k->w + 12, 44);
            if (b.x < 1)
                b.x = 1;
            if (b.x + b.w > UI_W - 1)
                b.x = UI_W - 1 - b.w;
            gfx_round_rect(s, gfx_r(b.x - 1, b.y - 1, b.w + 2, b.h + 2), 8, 0x3C424D, 230);
            gfx_round_rect_vgradient(s, b, 7, 0xFFFFFF, 0xDADDE3, 255);
            gfx_text_center(s, b.x, b.y + 4, b.w, bf, k->label, 0x111111, 255);
        }
    }
}

static int hit(int x, int y)
{
    int best = -1, best_d = 1 << 30;

    for (int i = 0; i < nkeys; i++) {
        const struct key *k = &keys[i];
        /* Nearest key centre, restricted to the touched row, so gaps count. */
        if (y < k->y - 3 || y >= k->y + k->h + 3)
            continue;
        int cx = k->x + k->w / 2, d = x > cx ? x - cx : cx - x;
        if (x >= k->x && x < k->x + k->w)
            d = 0;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

int ui_keyboard_event(struct ui_keyboard *kb, const struct gz_input_event *e, int y, bool *changed)
{
    int ky = e->y - y;
    int old = kb->pressed;

    build(kb);
    *changed = false;
    switch (e->type) {
    case GZ_EV_TOUCH_DOWN:
    case GZ_EV_TOUCH_MOVE:
        kb->pressed = (ky >= 0 && ky < UI_KB_H) ? hit(e->x, ky) : -1;
        *changed = kb->pressed != old;
        if (e->type == GZ_EV_TOUCH_DOWN && kb->pressed >= 0) {
            audio_sound(AUDIO_SND_KEY);
            uisnd_sounded();
        }
        return 0;
    case GZ_EV_TOUCH_CANCEL:
        kb->pressed = -1;
        *changed = old != -1;
        return 0;
    case GZ_EV_TOUCH_UP: {
        int k = kb->pressed;
        kb->pressed = -1;
        *changed = old != -1;
        if (k < 0)
            return 0;
        char ch = keys[k].ch;
        if (ch == KEY_SHIFT) {
            kb->shift = !kb->shift;
            return 0;
        }
        if (ch == KEY_MODE) {
            kb->numbers = !kb->numbers;
            kb->shift = false;
            return 0;
        }
        if (kb->shift && ch != '\b') {
            kb->shift = false;
            *changed = true;
        }
        if (ch == '\n')
            kb->shift = !kb->url;
        if (ch == KEY_DOTCOM)
            return UI_KEY_DOTCOM;
        return (unsigned char)ch;
    }
    }
    return 0;
}
