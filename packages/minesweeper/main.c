/*
 * Minesweeper: an installable game package (glaze/pkg.h). Tap to dig, hold
 * (or use the flag mode switch) to flag; a tap on an opened number whose
 * flags are all placed digs around it. The game, its level and the best
 * times live in <bundle>/data/state, saved after every move since the Home
 * button kills the app.
 */

#include <glaze/fs.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <string.h>

#define MAX_W       16
#define MAX_H       16
#define STRIP_Y     UI_NAV_H
#define STRIP_H     32
#define BOARD_Y     (STRIP_Y + STRIP_H)
#define HOLD_MS     400

enum { C_MINE = 1, C_OPEN = 2, C_FLAG = 4, C_BOOM = 8 };
enum { G_READY, G_PLAYING, G_WON, G_LOST };

static const struct level {
    const char *name;
    int w, h, mines, cell;
} levels[] = {
    { "Beginner",     9,  9, 10, 26 },
    { "Intermediate", 16, 16, 40, 15 },
};
#define NLEVELS ((int)(sizeof(levels) / sizeof(levels[0])))

#define STATE_VERSION 1

static struct state {
    uint32_t version;
    uint8_t level, status, flag_mode, pad;
    uint32_t elapsed_ms;    /* time played up to the last save */
    uint32_t best_s[NLEVELS];
    uint8_t cell[MAX_W * MAX_H];
} st;

static const struct level *lv;
static uint32_t start_ms;   /* gz_time_ms() at elapsed 0 while playing */
static uint32_t rng;

/* Touch tracking. */
enum { P_NONE, P_LEVEL, P_FACE, P_MODE, P_CELL };
static int pressed = P_NONE, press_cell = -1;
static uint32_t press_ms;
static bool hold_done;
static gfx_rect level_rect, face_rect, mode_rect;

static bool alert_open, alert_choice;
static char alert_title[24], alert_msg[64];
static int alert_pressed = -1;
static gfx_rect alert_r1, alert_r2;

/* ---- persistence -------------------------------------------------------- */

static void data_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/data/state", res_bundle());
}

static uint32_t elapsed_ms(void)
{
    return st.status == G_PLAYING ? gz_time_ms() - start_ms : st.elapsed_ms;
}

static void save(void)
{
    char path[FS_PATH_MAX], dir[FS_PATH_MAX];

    st.elapsed_ms = elapsed_ms();
    data_path(path, sizeof(path));
    if (fs_write_file(path, &st, sizeof(st)) == GZ_ENOENT) {
        snprintf(dir, sizeof(dir), "%s/data", res_bundle());
        fs_mkdirs(dir);
        fs_write_file(path, &st, sizeof(st));
    }
}

static void new_game(int level)
{
    st.level = (uint8_t)level;
    st.status = G_READY;
    st.elapsed_ms = 0;
    memset(st.cell, 0, sizeof(st.cell));
    lv = &levels[level];
}

static void load(void)
{
    char path[FS_PATH_MAX];

    data_path(path, sizeof(path));
    if (fs_read_file(path, &st, sizeof(st)) != sizeof(st) || st.version != STATE_VERSION ||
        st.level >= NLEVELS || st.status > G_LOST) {
        memset(&st, 0, sizeof(st));
        st.version = STATE_VERSION;
        new_game(0);
        return;
    }
    lv = &levels[st.level];
    start_ms = gz_time_ms() - st.elapsed_ms;
}

/* ---- rules -------------------------------------------------------------- */

static uint32_t rand32(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static bool near(int a, int b)
{
    int dx = a % lv->w - b % lv->w, dy = a / lv->w - b / lv->w;
    return dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1;
}

/* Fills nb[] with the indices of the cells around i; returns how many. */
static int neighbours(int i, int nb[8])
{
    int x = i % lv->w, y = i / lv->w, n = 0;

    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if ((dx || dy) && nx >= 0 && nx < lv->w && ny >= 0 && ny < lv->h)
                nb[n++] = ny * lv->w + nx;
        }
    return n;
}

static int count_around(int i, uint8_t bit)
{
    int nb[8], n = neighbours(i, nb), c = 0;

    for (int k = 0; k < n; k++)
        c += (st.cell[nb[k]] & bit) != 0;
    return c;
}

/* Mines go in on the first dig, never on or next to the dug cell. */
static void place_mines(int first)
{
    int cells = lv->w * lv->h, placed = 0;

    while (placed < lv->mines) {
        int i = (int)(rand32() % (uint32_t)cells);
        if (!(st.cell[i] & C_MINE) && !near(i, first)) {
            st.cell[i] |= C_MINE;
            placed++;
        }
    }
    st.status = G_PLAYING;
    start_ms = gz_time_ms();
}

static int flags_placed(void)
{
    int n = 0;

    for (int i = 0; i < lv->w * lv->h; i++)
        n += (st.cell[i] & C_FLAG) != 0;
    return n;
}

static void show_alert(const char *title, const char *msg, bool choice)
{
    strlcpy(alert_title, title, sizeof(alert_title));
    strlcpy(alert_msg, msg, sizeof(alert_msg));
    alert_open = true;
    alert_choice = choice;
    alert_pressed = -1;
}

static void check_won(void)
{
    int cells = lv->w * lv->h, open = 0;
    char msg[64];

    for (int i = 0; i < cells; i++)
        open += (st.cell[i] & C_OPEN) != 0;
    if (open != cells - lv->mines)
        return;
    st.elapsed_ms = elapsed_ms();
    st.status = G_WON;
    for (int i = 0; i < cells; i++)
        if (st.cell[i] & C_MINE)
            st.cell[i] |= C_FLAG;
    uint32_t secs = (st.elapsed_ms + 999) / 1000;
    bool best = !st.best_s[st.level] || secs < st.best_s[st.level];
    if (best)
        st.best_s[st.level] = secs;
    snprintf(msg, sizeof(msg), best ? "%u seconds, a new best!" : "%u seconds.\nYour best is %u.", secs,
             st.best_s[st.level]);
    show_alert("You Win!", msg, false);
}

/* Opens cell i, flooding out from cells with no mines around them. */
static void dig(int i)
{
    static int stack[MAX_W * MAX_H];
    int sp = 0;

    if (st.cell[i] & (C_OPEN | C_FLAG))
        return;
    if (st.cell[i] & C_MINE) {
        st.elapsed_ms = elapsed_ms();
        st.status = G_LOST;
        st.cell[i] |= C_BOOM;
        for (int k = 0; k < lv->w * lv->h; k++)
            if ((st.cell[k] & C_MINE) && !(st.cell[k] & C_FLAG))
                st.cell[k] |= C_OPEN;
        return;
    }
    st.cell[i] |= C_OPEN;
    stack[sp++] = i;
    while (sp) {
        int c = stack[--sp], nb[8], n;
        if (count_around(c, C_MINE))
            continue;
        n = neighbours(c, nb);
        for (int k = 0; k < n; k++)
            if (!(st.cell[nb[k]] & (C_OPEN | C_FLAG | C_MINE))) {
                st.cell[nb[k]] |= C_OPEN;
                stack[sp++] = nb[k];
            }
    }
}

static void tap_cell(int i)
{
    if (st.status == G_WON || st.status == G_LOST)
        return;
    if (st.cell[i] & C_OPEN) {
        /* Chord: dig around a number whose flags are all placed. */
        int nb[8], n = neighbours(i, nb), mines = count_around(i, C_MINE);
        if (!mines || count_around(i, C_FLAG) != mines)
            return;
        for (int k = 0; k < n && st.status == G_PLAYING; k++)
            dig(nb[k]);
    } else if (st.flag_mode) {
        st.cell[i] ^= C_FLAG;
    } else {
        if (st.cell[i] & C_FLAG)
            return;
        if (st.status == G_READY)
            place_mines(i);
        dig(i);
    }
    if (st.status == G_PLAYING)
        check_won();
    save();
}

static void hold_cell(int i)
{
    if (st.status == G_WON || st.status == G_LOST || (st.cell[i] & C_OPEN))
        return;
    st.cell[i] ^= C_FLAG;
    save();
}

/* ---- drawing ------------------------------------------------------------ */

static gfx_rect cell_rect(int i)
{
    int bw = lv->w * lv->cell, x0 = (UI_W - bw) / 2;
    int y0 = BOARD_Y + (UI_H - BOARD_Y - lv->h * lv->cell) / 2;

    return gfx_r(x0 + i % lv->w * lv->cell, y0 + i / lv->w * lv->cell, lv->cell, lv->cell);
}

static int cell_at(int x, int y)
{
    gfx_rect b = cell_rect(0);

    x -= b.x;
    y -= b.y;
    if (x < 0 || y < 0 || x >= lv->w * lv->cell || y >= lv->h * lv->cell)
        return -1;
    return y / lv->cell * lv->w + x / lv->cell;
}

static void draw_mine(gfx_surface *s, int cx, int cy, int r)
{
    gfx_line(s, cx - r - 2, cy, cx + r + 2, cy, 0x000000, 255);
    gfx_line(s, cx, cy - r - 2, cx, cy + r + 2, 0x000000, 255);
    gfx_line(s, cx - r, cy - r, cx + r, cy + r, 0x000000, 255);
    gfx_line(s, cx - r, cy + r, cx + r, cy - r, 0x000000, 255);
    gfx_circle(s, cx, cy, r, 0x000000, 255);
    gfx_circle(s, cx - r / 3, cy - r / 3, r / 3 > 0 ? r / 3 : 1, 0xFFFFFF, 255);
}

/* A flag of height h whose pole stands at x, from y to y + h. */
static void draw_flag(gfx_surface *s, int x, int y, int h)
{
    int fh = h / 2;

    for (int k = 0; k < fh; k++) {
        int w = (k <= fh / 2 ? k : fh - k) * 2 + 1;
        gfx_fill(s, gfx_r(x - w, y + k, w, 1), 0xE01010);
    }
    gfx_fill(s, gfx_r(x, y, 1 + h / 12, h), 0x000000);
    gfx_fill(s, gfx_r(x - h / 4, y + h - 2, h / 2 + 1, 2), 0x000000);
}

static void draw_cell(gfx_surface *s, int i)
{
    static const uint32_t num_rgb[9] = {
        0, 0x1030E0, 0x108010, 0xE01010, 0x101080, 0x801010, 0x108080, 0x000000, 0x707070,
    };
    gfx_rect r = cell_rect(i);
    uint8_t c = st.cell[i];
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    bool down = pressed == P_CELL && press_cell == i && !hold_done && !(c & (C_OPEN | C_FLAG)) && !st.flag_mode;

    if (c & C_OPEN || down) {
        gfx_fill(s, r, c & C_BOOM ? 0xE02020 : 0xC6C6C6);
        gfx_fill(s, gfx_r(r.x, r.y, r.w, 1), 0x8A8A8A);
        gfx_fill(s, gfx_r(r.x, r.y, 1, r.h), 0x8A8A8A);
        if (down)
            return;
        if (c & C_MINE) {
            draw_mine(s, cx, cy, r.w / 4);
            return;
        }
        int n = count_around(i, C_MINE);
        if (n) {
            char t[2] = { (char)('0' + n), 0 };
            const gfx_font *f = ui_font(lv->cell >= 20 ? FONT_B17 : FONT_B11);
            gfx_text_center(s, r.x, cy - f->line_height / 2, r.w, f, t, num_rgb[n], 255);
        }
        return;
    }
    /* Covered: a raised tile. */
    int b = lv->cell >= 20 ? 3 : 2;
    gfx_fill(s, r, 0x808080);
    gfx_fill(s, gfx_r(r.x, r.y, r.w - 1, r.h - 1), 0xFFFFFF);
    gfx_fill(s, gfx_r(r.x + b, r.y + b, r.w - 2 * b, r.h - 2 * b), 0xBDBDBD);
    if (c & C_FLAG) {
        draw_flag(s, cx + 1, r.y + r.h / 5, r.h * 3 / 5);
        if (st.status == G_LOST && !(c & C_MINE)) {
            gfx_line(s, r.x + 3, r.y + 3, r.x + r.w - 4, r.y + r.h - 4, 0xE01010, 255);
            gfx_line(s, r.x + 3, r.y + r.h - 4, r.x + r.w - 4, r.y + 3, 0xE01010, 255);
        }
    }
}

static void draw_counter(gfx_surface *s, gfx_rect r, int value)
{
    const gfx_font *f = ui_font(FONT_B17);
    char t[8];

    if (value > 999)
        value = 999;
    if (value < -99)
        value = -99;
    snprintf(t, sizeof(t), value < 0 ? "-%02d" : "%03d", value < 0 ? -value : value);
    gfx_round_rect(s, r, 4, 0x200000, 255);
    gfx_text_center(s, r.x, r.y + (r.h - f->line_height) / 2, r.w, f, t, 0xFF3020, 255);
}

static void draw_face(gfx_surface *s)
{
    gfx_rect r = face_rect;
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;

    gfx_round_rect_vgradient(s, r, 5, pressed == P_FACE ? 0x9AA4B4 : 0xE4E8EE, pressed == P_FACE ? 0x7A8494 : 0xB4BCC8,
                             255);
    gfx_circle(s, cx, cy, 11, 0x000000, 255);
    gfx_circle(s, cx, cy, 10, 0xFFD820, 255);
    if (st.status == G_LOST) {
        for (int e = -1; e <= 1; e += 2) {
            int ex = cx + e * 4;
            gfx_line(s, ex - 2, cy - 5, ex + 2, cy - 1, 0x000000, 255);
            gfx_line(s, ex - 2, cy - 1, ex + 2, cy - 5, 0x000000, 255);
        }
        gfx_line(s, cx - 4, cy + 5, cx + 4, cy + 5, 0x000000, 255);
        gfx_line(s, cx - 4, cy + 5, cx - 5, cy + 6, 0x000000, 255);
        gfx_line(s, cx + 4, cy + 5, cx + 5, cy + 6, 0x000000, 255);
        return;
    }
    if (st.status == G_WON) {
        gfx_fill(s, gfx_r(cx - 8, cy - 4, 16, 2), 0x000000);
        gfx_fill(s, gfx_r(cx - 7, cy - 4, 6, 4), 0x000000);
        gfx_fill(s, gfx_r(cx + 1, cy - 4, 6, 4), 0x000000);
    } else if (pressed == P_CELL && !hold_done) {
        gfx_circle(s, cx - 4, cy - 3, 2, 0x000000, 255);
        gfx_circle(s, cx + 4, cy - 3, 2, 0x000000, 255);
        gfx_circle(s, cx, cy + 5, 2, 0x000000, 255);
        return;
    } else {
        gfx_fill(s, gfx_r(cx - 5, cy - 4, 2, 3), 0x000000);
        gfx_fill(s, gfx_r(cx + 3, cy - 4, 2, 3), 0x000000);
    }
    gfx_line(s, cx - 5, cy + 3, cx - 2, cy + 6, 0x000000, 255);
    gfx_line(s, cx - 2, cy + 6, cx + 2, cy + 6, 0x000000, 255);
    gfx_line(s, cx + 2, cy + 6, cx + 5, cy + 3, 0x000000, 255);
}

static void draw_strip(gfx_surface *s)
{
    gfx_rect strip = gfx_r(0, STRIP_Y, UI_W, STRIP_H);
    uint32_t secs = st.status == G_PLAYING ? elapsed_ms() / 1000 : (st.elapsed_ms + 999) / 1000;

    gfx_vgradient(s, strip, 0x9AA6B6, 0x6E7C90, 255);
    gfx_fill(s, gfx_r(0, STRIP_Y + STRIP_H - 1, UI_W, 1), 0x2D3642);
    draw_counter(s, gfx_r(6, STRIP_Y + 4, 48, 24), lv->mines - flags_placed());
    draw_counter(s, gfx_r(UI_W - 54, STRIP_Y + 4, 48, 24), (int)secs);

    /* Dig/flag mode: the tool a tap uses. */
    mode_rect = gfx_r(62, STRIP_Y + 3, 40, 26);
    gfx_round_rect_vgradient(s, mode_rect, 5, st.flag_mode ? 0x5E95F2 : 0xE4E8EE, st.flag_mode ? 0x2A63D4 : 0xB4BCC8,
                             pressed == P_MODE ? 160 : 255);
    if (st.flag_mode)
        draw_flag(s, mode_rect.x + 21, mode_rect.y + 5, 16);
    else
        draw_mine(s, mode_rect.x + 20, mode_rect.y + 13, 5);

    face_rect = gfx_r((UI_W - 30) / 2 + 12, STRIP_Y + 2, 30, 28);
    draw_face(s);
}

static void draw(void)
{
    gfx_surface *s = &ui.surf;

    ui_draw_navbar(s, "Minesweeper", NULL, NULL, 0);
    level_rect = ui_draw_bar_button(s, "Level", true, pressed == P_LEVEL, false, 0);
    draw_strip(s);
    gfx_fill(s, gfx_r(0, BOARD_Y, UI_W, UI_H - BOARD_Y), 0x7B8796);
    for (int i = 0; i < lv->w * lv->h; i++)
        draw_cell(s, i);
    if (alert_open)
        ui_draw_alert(s, alert_title, alert_msg, alert_choice ? levels[0].name : "OK",
                      alert_choice ? levels[1].name : NULL, &alert_r1, &alert_r2, alert_pressed);
    ui_invalidate_all();
    ui_present();
}

/* Only the timer changes while nobody touches the screen. */
static void draw_timer(void)
{
    draw_strip(&ui.surf);
    ui_invalidate(gfx_r(0, STRIP_Y, UI_W, STRIP_H));
    ui_present();
}

/* ---- input -------------------------------------------------------------- */

static void alert_event(const struct gz_input_event *e)
{
    int h = gfx_rect_contains(alert_r1, e->x, e->y) ? 0
          : alert_choice && gfx_rect_contains(alert_r2, e->x, e->y) ? 1 : -1;

    if (e->type != GZ_EV_TOUCH_UP) {
        alert_pressed = h;
        return;
    }
    alert_pressed = -1;
    if (h < 0)
        return;
    alert_open = false;
    if (alert_choice) {
        new_game(h);
        save();
    }
}

static int hit(int x, int y)
{
    if (gfx_rect_contains(level_rect, x, y))
        return P_LEVEL;
    if (gfx_rect_contains(face_rect, x, y))
        return P_FACE;
    if (gfx_rect_contains(mode_rect, x, y))
        return P_MODE;
    return cell_at(x, y) >= 0 ? P_CELL : P_NONE;
}

static void touch(const struct gz_input_event *e)
{
    if (e->type == GZ_EV_TOUCH_DOWN) {
        pressed = hit(e->x, e->y);
        press_cell = pressed == P_CELL ? cell_at(e->x, e->y) : -1;
        press_ms = gz_time_ms();
        hold_done = false;
        return;
    }
    if (e->type == GZ_EV_TOUCH_MOVE) {
        if (pressed == P_CELL) {
            int c = cell_at(e->x, e->y);
            if (c != press_cell && !hold_done) {
                press_cell = c;
                press_ms = gz_time_ms();
                if (c < 0)
                    pressed = P_NONE;
            }
        }
        return;
    }
    if (e->type != GZ_EV_TOUCH_UP)
        return;
    int p = pressed;
    pressed = P_NONE;
    if (p != hit(e->x, e->y))
        return;
    switch (p) {
    case P_LEVEL:
        show_alert("New Game", "Choose a level.", true);
        break;
    case P_FACE:
        new_game(st.level);
        save();
        break;
    case P_MODE:
        st.flag_mode = !st.flag_mode;
        save();
        break;
    case P_CELL:
        if (!hold_done && press_cell >= 0)
            tap_cell(press_cell);
        break;
    }
}

int main(void)
{
    uint32_t shown_secs = 0;

    ui_init(SB_STATUS_GRAY);
    rng = (uint32_t)gz_time_us() | 1;
    load();
    draw();
    for (;;) {
        struct gz_input_event ev;
        int timeout = st.status == G_PLAYING ? 1000 - (int)(elapsed_ms() % 1000) : -1;

        /* A held cell flags once HOLD_MS have passed. */
        if (pressed == P_CELL && !hold_done && press_cell >= 0 && !st.flag_mode) {
            int left = HOLD_MS - (int)(gz_time_ms() - press_ms);
            if (left <= 0) {
                hold_done = true;
                hold_cell(press_cell);
                draw();
                continue;
            }
            if (timeout < 0 || left < timeout)
                timeout = left;
        }
        if (ui_next_event(&ev, timeout)) {
            if (alert_open)
                alert_event(&ev);
            else
                touch(&ev);
            draw();
            continue;
        }
        uint32_t secs = elapsed_ms() / 1000;
        if (secs != shown_secs) {
            shown_secs = secs;
            draw_timer();
        }
    }
}
