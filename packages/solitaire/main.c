/*
 * Solitaire: Klondike as an installable game package (glaze/pkg.h). Drag a
 * card or a face-up run onto another pile, or tap it to send it to the
 * first pile that takes it; tap the stock to turn one card (or three) and
 * to turn the waste back over. The deal lives in <bundle>/data/state, saved
 * after every move since the Home button kills the app.
 */

#include <glaze/fs.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CW          32
#define CH          44
#define COL_X(c)    (2 + (c) * (CW + 2))
#define TOP_Y       (UI_NAV_H + 5)
#define TAB_Y       (TOP_Y + CH + 6)
#define DOWN_OFF    5       /* overlap of face-down tableau cards */
#define UP_OFF      13      /* and of face-up ones, unless the column is long */
#define FAN_OFF     7       /* waste fan in draw-3 */
#define DRAG_SLOP   5
#define FELT_TOP    0x2E8B3E
#define FELT_BOTTOM 0x17602A

enum { P_STOCK, P_WASTE, P_FOUND, P_TAB = P_FOUND + 4, NPILES = P_TAB + 7 };

/* A card is suit * 13 + rank (rank 0 = ace), with UP set when face up.
 * Suits: spades, hearts, diamonds, clubs. */
#define UP          0x80
#define RANK(c)     (((c) & 0x3F) % 13)
#define SUIT(c)     (((c) & 0x3F) / 13)
#define RED(c)      (SUIT(c) == 1 || SUIT(c) == 2)

#define STATE_VERSION 1

static struct state {
    uint32_t version;
    uint8_t draw3, won, pad[2];
    uint32_t moves;
    uint32_t elapsed_ms;
    uint8_t n[NPILES];
    uint8_t card[NPILES][52];
} st;

static uint32_t start_ms;
static uint32_t rng;

/* Undo: the last moves, in a ring. */
enum { M_MOVE, M_DRAW, M_REDEAL };
static struct move {
    uint8_t kind, from, to, count, flipped;
} undo_ring[64];
static int undo_n, undo_head;

/* Touch: a press on a bar button, the stock, or a card that may be dragged. */
enum { T_NONE, T_NEW, T_UNDO, T_STOCK, T_CARD };
static int touch_kind = T_NONE;
static int drag_pile = -1, drag_count;     /* the run being touched */
static bool dragging;
static int down_x, down_y, grab_dx, grab_dy, drag_x, drag_y;
static gfx_rect new_rect, undo_rect;
static gfx_surface bg;      /* the table without the dragged run */
static uint16_t *bg_px;

enum { A_NONE, A_CONFIRM, A_DRAW, A_WON };
static int alert = A_NONE, alert_pressed = -1;
static char alert_msg[64];
static gfx_rect alert_r1, alert_r2;

/* ---- persistence -------------------------------------------------------- */

static void data_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/data/state", res_bundle());
}

static uint32_t elapsed_ms(void)
{
    return st.won ? st.elapsed_ms : gz_time_ms() - start_ms;
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

static uint32_t rand32(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void deal(void)
{
    uint8_t deck[52];
    int k = 0;

    for (int i = 0; i < 52; i++)
        deck[i] = (uint8_t)i;
    for (int i = 51; i > 0; i--) {
        int j = (int)(rand32() % (uint32_t)(i + 1));
        uint8_t t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
    memset(st.n, 0, sizeof(st.n));
    for (int t = 0; t < 7; t++)
        for (int i = 0; i <= t; i++)
            st.card[P_TAB + t][st.n[P_TAB + t]++] = deck[k++] | (i == t ? UP : 0);
    while (k < 52)
        st.card[P_STOCK][st.n[P_STOCK]++] = deck[k++];
    st.moves = 0;
    st.won = 0;
    st.elapsed_ms = 0;
    start_ms = gz_time_ms();
    undo_n = 0;
}

static bool state_ok(void)
{
    uint8_t seen[52] = { 0 };
    int total = 0;

    if (st.version != STATE_VERSION)
        return false;
    for (int p = 0; p < NPILES; p++) {
        if (st.n[p] > 52)
            return false;
        for (int i = 0; i < st.n[p]; i++) {
            int id = st.card[p][i] & 0x3F;
            if (id >= 52 || seen[id]++)
                return false;
        }
        total += st.n[p];
    }
    return total == 52;
}

static void load(void)
{
    char path[FS_PATH_MAX];

    data_path(path, sizeof(path));
    if (fs_read_file(path, &st, sizeof(st)) != sizeof(st) || !state_ok()) {
        memset(&st, 0, sizeof(st));
        st.version = STATE_VERSION;
        deal();
        return;
    }
    start_ms = gz_time_ms() - st.elapsed_ms;
}

/* ---- rules -------------------------------------------------------------- */

static uint8_t top(int p)
{
    return st.n[p] ? st.card[p][st.n[p] - 1] : 0;
}

static bool fits(int p, uint8_t c)
{
    uint8_t t = top(p);

    if (p >= P_FOUND && p < P_TAB) {
        if (!st.n[p])
            return RANK(c) == 0;
        return SUIT(t) == SUIT(c) && RANK(c) == RANK(t) + 1;
    }
    if (p >= P_TAB) {
        if (!st.n[p])
            return RANK(c) == 12;
        return (t & UP) && RED(t) != RED(c) && RANK(t) == RANK(c) + 1;
    }
    return false;
}

static void push_undo(int kind, int from, int to, int count, bool flipped)
{
    undo_ring[undo_head] = (struct move){ (uint8_t)kind, (uint8_t)from, (uint8_t)to, (uint8_t)count, flipped };
    undo_head = (undo_head + 1) % 64;
    if (undo_n < 64)
        undo_n++;
}

static void move_cards(int from, int to, int count)
{
    bool flipped = false;

    memcpy(&st.card[to][st.n[to]], &st.card[from][st.n[from] - count], (size_t)count);
    st.n[to] += count;
    st.n[from] -= count;
    if (from >= P_TAB && st.n[from] && !(top(from) & UP)) {
        st.card[from][st.n[from] - 1] |= UP;
        flipped = true;
    }
    push_undo(M_MOVE, from, to, count, flipped);
    st.moves++;
}

static void turn_stock(void)
{
    if (st.n[P_STOCK]) {
        int k = st.draw3 && st.n[P_STOCK] >= 3 ? 3 : 1;
        for (int i = 0; i < k; i++)
            st.card[P_WASTE][st.n[P_WASTE]++] = st.card[P_STOCK][--st.n[P_STOCK]] | UP;
        push_undo(M_DRAW, P_STOCK, P_WASTE, k, false);
    } else if (st.n[P_WASTE]) {
        while (st.n[P_WASTE])
            st.card[P_STOCK][st.n[P_STOCK]++] = st.card[P_WASTE][--st.n[P_WASTE]] & ~UP;
        push_undo(M_REDEAL, P_WASTE, P_STOCK, st.n[P_STOCK], false);
    } else {
        return;
    }
    st.moves++;
}

static void undo(void)
{
    if (!undo_n || st.won)
        return;
    undo_head = (undo_head + 63) % 64;
    undo_n--;
    struct move m = undo_ring[undo_head];
    switch (m.kind) {
    case M_MOVE:
        if (m.flipped)
            st.card[m.from][st.n[m.from] - 1] &= ~UP;
        memcpy(&st.card[m.from][st.n[m.from]], &st.card[m.to][st.n[m.to] - m.count], m.count);
        st.n[m.from] += m.count;
        st.n[m.to] -= m.count;
        break;
    case M_DRAW:
        for (int i = 0; i < m.count; i++)
            st.card[P_STOCK][st.n[P_STOCK]++] = st.card[P_WASTE][--st.n[P_WASTE]] & ~UP;
        break;
    case M_REDEAL:
        while (st.n[P_STOCK])
            st.card[P_WASTE][st.n[P_WASTE]++] = st.card[P_STOCK][--st.n[P_STOCK]] | UP;
        break;
    }
    st.moves++;
}

/* Moves a tapped run to the first pile that takes it: a foundation for a
 * single card, else a tableau column that isn't empty, else an empty one
 * (unless the run already starts its column). */
static bool auto_move(int from, int count)
{
    uint8_t c = st.card[from][st.n[from] - count];

    if (count == 1)
        for (int p = P_FOUND; p < P_TAB; p++)
            if (fits(p, c)) {
                move_cards(from, p, 1);
                return true;
            }
    for (int pass = 0; pass < 2; pass++)
        for (int p = P_TAB; p < NPILES; p++) {
            if (p == from || (st.n[p] == 0) != pass)
                continue;
            if (pass && from >= P_TAB && count == st.n[from])
                continue;
            if (fits(p, c)) {
                move_cards(from, p, count);
                return true;
            }
        }
    return false;
}

static bool all_home(void)
{
    int n = 0;

    for (int p = P_FOUND; p < P_TAB; p++)
        n += st.n[p];
    return n == 52;
}

/* Once nothing is hidden, the rest of the game plays itself. */
static bool can_finish(void)
{
    if (st.n[P_STOCK] || st.n[P_WASTE] || all_home())
        return false;
    for (int p = P_TAB; p < NPILES; p++)
        for (int i = 0; i < st.n[p]; i++)
            if (!(st.card[p][i] & UP))
                return false;
    return true;
}

/* ---- drawing ------------------------------------------------------------ */

/* 7x7 suit shapes, one byte per row, bit 6 leftmost. */
static const uint8_t suit_bits[4][7] = {
    { 0x08, 0x1C, 0x3E, 0x7F, 0x7F, 0x08, 0x1C },     /* spades */
    { 0x36, 0x7F, 0x7F, 0x3E, 0x1C, 0x08, 0x00 },     /* hearts */
    { 0x08, 0x1C, 0x3E, 0x7F, 0x3E, 0x1C, 0x08 },     /* diamonds */
    { 0x1C, 0x1C, 0x6B, 0x7F, 0x6B, 0x08, 0x1C },     /* clubs */
};
static const char *const rank_names[13] = {
    "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K",
};

static void draw_suit(gfx_surface *s, int x, int y, int suit, int scale, uint32_t rgb)
{
    for (int r = 0; r < 7; r++)
        for (int b = 0; b < 7; b++)
            if (suit_bits[suit][r] & (0x40 >> b))
                gfx_fill(s, gfx_r(x + b * scale, y + r * scale, scale, scale), rgb);
}

static void draw_back(gfx_surface *s, int x, int y)
{
    gfx_rect in = gfx_r(x + 2, y + 2, CW - 4, CH - 4);

    gfx_round_rect(s, gfx_r(x, y, CW, CH), 3, 0x404040, 255);
    gfx_round_rect(s, gfx_r(x + 1, y + 1, CW - 2, CH - 2), 2, 0xFFFFFF, 255);
    gfx_fill(s, in, 0x2858B8);
    gfx_set_clip(s, in);
    for (int d = -CH; d < CW; d += 5) {
        gfx_line(s, in.x + d, in.y, in.x + d + CH, in.y + CH, 0x4A7CDA, 255);
        gfx_line(s, in.x + d + CH, in.y, in.x + d, in.y + CH, 0x4A7CDA, 255);
    }
    gfx_reset_clip(s);
}

static void draw_card(gfx_surface *s, int x, int y, uint8_t c)
{
    if (!(c & UP)) {
        draw_back(s, x, y);
        return;
    }
    const gfx_font *f = ui_font(FONT_B11);
    uint32_t ink = RED(c) ? 0xD01818 : 0x101010;
    int rank = RANK(c);

    gfx_round_rect(s, gfx_r(x, y, CW, CH), 3, 0x404040, 255);
    gfx_round_rect(s, gfx_r(x + 1, y + 1, CW - 2, CH - 2), 2, 0xFFFFFF, 255);
    gfx_text(s, x + 2, y + 1, f, rank_names[rank], ink, 255);
    draw_suit(s, x + CW - 10, y + 3, SUIT(c), 1, ink);
    if (rank >= 10) {
        gfx_rect fr = gfx_r(x + 5, y + 15, CW - 10, CH - 19);
        const gfx_font *bf = ui_font(FONT_B17);
        gfx_fill(s, fr, ink);
        gfx_fill(s, gfx_r(fr.x + 1, fr.y + 1, fr.w - 2, fr.h - 2), 0xF6E7B4);
        gfx_text_center(s, fr.x, fr.y + (fr.h - bf->line_height) / 2, fr.w, bf, rank_names[rank], ink, 255);
    } else {
        draw_suit(s, x + (CW - 14) / 2, y + 20, SUIT(c), 2, ink);
    }
}

static void draw_slot(gfx_surface *s, int x, int y)
{
    gfx_round_rect(s, gfx_r(x, y, CW, CH), 3, 0x000000, 60);
    gfx_round_rect(s, gfx_r(x + 1, y + 1, CW - 2, CH - 2), 2, FELT_TOP, 200);
}

static void pile_xy(int p, int *x, int *y)
{
    if (p >= P_TAB) {
        *x = COL_X(p - P_TAB);
        *y = TAB_Y;
    } else {
        *x = COL_X(p == P_STOCK ? 0 : p == P_WASTE ? 1 : 3 + p - P_FOUND);
        *y = TOP_Y;
    }
}

/* Overlap of face-up cards in tableau pile p, shrunk to fit the screen. */
static int up_off(int p)
{
    int down = 0, up;

    while (down < st.n[p] && !(st.card[p][down] & UP))
        down++;
    up = st.n[p] - down;
    if (up < 2)
        return UP_OFF;
    int room = UI_H - 2 - TAB_Y - CH - down * DOWN_OFF;
    int off = room / (up - 1);
    return off < UP_OFF ? (off < 4 ? 4 : off) : UP_OFF;
}

/* Where card i of pile p is drawn. */
static void card_xy(int p, int i, int *x, int *y)
{
    pile_xy(p, x, y);
    if (p >= P_TAB) {
        int uo = up_off(p);
        for (int k = 0; k < i; k++)
            *y += st.card[p][k] & UP ? uo : DOWN_OFF;
    } else if (p == P_WASTE && st.draw3) {
        int shown = st.n[p] < 3 ? st.n[p] : 3;
        int first = st.n[p] - shown;
        if (i > first)
            *x += (i - first) * FAN_OFF;
    }
}

/* The table, leaving out the top `hide` cards of pile `hide_pile`. */
static void draw_table(gfx_surface *s, int hide_pile, int hide)
{
    gfx_vgradient(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H), FELT_TOP, FELT_BOTTOM, 255);
    ui_draw_navbar(s, "Solitaire", NULL, NULL, 0);
    new_rect = ui_draw_bar_button(s, "New", false, touch_kind == T_NEW, false, 0);
    undo_rect = ui_draw_bar_button(s, "Undo", true, touch_kind == T_UNDO, false, 0);

    for (int p = 0; p < NPILES; p++) {
        int x, y, n = st.n[p] - (p == hide_pile ? hide : 0), from = 0;
        pile_xy(p, &x, &y);
        draw_slot(s, x, y);
        if (p == P_STOCK && !n && st.n[P_WASTE]) {
            /* Tap to turn the waste over: a ring. */
            gfx_circle(s, x + CW / 2, y + CH / 2, 9, 0xFFFFFF, 90);
            gfx_circle(s, x + CW / 2, y + CH / 2, 6, FELT_TOP, 255);
        }
        if (p >= P_FOUND && p < P_TAB && !n)
            gfx_text_center(s, x, y + (CH - 14) / 2, CW, ui_font(FONT_B12), "A", 0xFFFFFF, 70);
        /* Only the top cards of the stock, waste and foundations show. */
        if (p < P_TAB)
            from = n - (p == P_WASTE && st.draw3 ? 3 : 1);
        for (int i = from < 0 ? 0 : from; i < n; i++) {
            int cx, cy;
            card_xy(p, i, &cx, &cy);
            draw_card(s, cx, cy, st.card[p][i]);
        }
    }
}

static gfx_rect drag_rect(void)
{
    return gfx_r(drag_x, drag_y, CW, CH + (drag_count - 1) * UP_OFF);
}

static void draw_dragged(gfx_surface *s)
{
    for (int i = 0; i < drag_count; i++)
        draw_card(s, drag_x, drag_y + i * UP_OFF, st.card[drag_pile][st.n[drag_pile] - drag_count + i]);
}

static void draw_alert(gfx_surface *s)
{
    switch (alert) {
    case A_CONFIRM:
        ui_draw_alert(s, "New Game", "Deal again? This game\nwill be lost.", "Cancel", "Deal", &alert_r1,
                      &alert_r2, alert_pressed);
        break;
    case A_DRAW:
        ui_draw_alert(s, "New Game", "Turn cards from the stock\none or three at a time.", "Draw 1", "Draw 3",
                      &alert_r1, &alert_r2, alert_pressed);
        break;
    case A_WON:
        ui_draw_alert(s, "You Win!", alert_msg, "OK", NULL, &alert_r1, &alert_r2, alert_pressed);
        break;
    }
}

static void draw(void)
{
    gfx_surface *s = &ui.surf;

    draw_table(s, -1, 0);
    draw_alert(s);
    ui_invalidate_all();
    ui_present();
}

/* While dragging, the table without the run is kept in bg; each move
 * restores the run's old place from it and draws the run at its new one. */
static void drag_begin(void)
{
    if (!bg_px) {
        bg_px = malloc(UI_W * UI_H * sizeof(uint16_t));
        if (!bg_px)
            return;
        gfx_surface_init(&bg, bg_px, UI_W, UI_H, UI_W);
    }
    dragging = true;
    draw_table(&bg, drag_pile, drag_count);
    gfx_blit(&ui.surf, 0, 0, &bg, gfx_r(0, 0, UI_W, UI_H));
    draw_dragged(&ui.surf);
    ui_invalidate_all();
    ui_present();
}

static void drag_to(int x, int y)
{
    gfx_rect old = drag_rect();

    drag_x = x - grab_dx;
    drag_y = y - grab_dy;
    gfx_blit(&ui.surf, old.x, old.y, &bg, old);
    draw_dragged(&ui.surf);
    ui_invalidate(gfx_rect_union(old, drag_rect()));
    ui_present();
}

/* ---- play --------------------------------------------------------------- */

static void after_move(void)
{
    while (can_finish()) {
        /* Lowest card first, one at a time, so it can be watched. */
        int best = -1, bf = 0;
        for (int p = P_TAB; p < NPILES; p++)
            for (int f = P_FOUND; f < P_TAB && st.n[p]; f++)
                if (fits(f, top(p)) && (best < 0 || RANK(top(p)) < RANK(top(best)))) {
                    best = p;
                    bf = f;
                }
        if (best < 0)
            break;
        move_cards(best, bf, 1);
        draw();
        gz_sleep_ms(60);
    }
    if (all_home() && !st.won) {
        uint32_t secs = (elapsed_ms() + 999) / 1000;
        st.elapsed_ms = elapsed_ms();
        st.won = 1;
        snprintf(alert_msg, sizeof(alert_msg), "%u moves in %u:%02u.", st.moves, secs / 60, secs % 60);
        alert = A_WON;
        alert_pressed = -1;
    }
    save();
}

/* The pile and card index under (x, y), or -1. */
static int card_at(int x, int y, int *index)
{
    for (int p = 0; p < NPILES; p++) {
        int px, py;
        pile_xy(p, &px, &py);
        if (p < P_TAB) {
            if (p == P_WASTE && st.n[p])
                card_xy(p, st.n[p] - 1, &px, &py);
            if (gfx_rect_contains(gfx_r(px, py, CW, CH), x, y)) {
                *index = st.n[p] - 1;
                return p;
            }
            continue;
        }
        if (x < px || x >= px + CW)
            continue;
        for (int i = st.n[p] - 1; i >= 0; i--) {
            int cx, cy;
            card_xy(p, i, &cx, &cy);
            if (y >= cy && y < cy + CH) {
                *index = i;
                return p;
            }
        }
        *index = -1;
        return p;
    }
    return -1;
}

/* Drops the dragged run on the legal pile it overlaps most. */
static bool drop(void)
{
    gfx_rect r = gfx_r(drag_x, drag_y, CW, CH);
    uint8_t c = st.card[drag_pile][st.n[drag_pile] - drag_count];
    int best = -1, best_area = 0;

    for (int p = P_FOUND; p < NPILES; p++) {
        int x, y;
        gfx_rect hit;
        if (p == drag_pile || (p < P_TAB && drag_count != 1) || !fits(p, c))
            continue;
        if (st.n[p])
            card_xy(p, st.n[p] - 1, &x, &y);
        else
            pile_xy(p, &x, &y);
        if (gfx_rect_intersect(r, gfx_r(x, y, CW, CH), &hit) && hit.w * hit.h > best_area) {
            best = p;
            best_area = hit.w * hit.h;
        }
    }
    if (best < 0)
        return false;
    move_cards(drag_pile, best, drag_count);
    return true;
}

static void alert_event(const struct gz_input_event *e)
{
    int h = gfx_rect_contains(alert_r1, e->x, e->y) ? 0
          : alert != A_WON && gfx_rect_contains(alert_r2, e->x, e->y) ? 1 : -1;
    int which = alert;

    if (e->type != GZ_EV_TOUCH_UP) {
        alert_pressed = h;
        return;
    }
    alert_pressed = -1;
    if (h < 0)
        return;
    alert = A_NONE;
    if (which == A_CONFIRM && h == 1) {
        alert = A_DRAW;
    } else if (which == A_DRAW) {
        st.draw3 = (uint8_t)h;
        deal();
        save();
    }
}

static void touch(const struct gz_input_event *e)
{
    if (e->type == GZ_EV_TOUCH_DOWN) {
        int i = -1;
        int p = card_at(e->x, e->y, &i);
        touch_kind = T_NONE;
        dragging = false;
        if (gfx_rect_contains(new_rect, e->x, e->y)) {
            touch_kind = T_NEW;
        } else if (gfx_rect_contains(undo_rect, e->x, e->y)) {
            touch_kind = T_UNDO;
        } else if (p == P_STOCK) {
            touch_kind = T_STOCK;
        } else if (p >= 0 && !st.won && i >= 0 && (st.card[p][i] & UP)) {
            int cx, cy;
            card_xy(p, i, &cx, &cy);
            touch_kind = T_CARD;
            drag_pile = p;
            drag_count = st.n[p] - i;
            down_x = e->x;
            down_y = e->y;
            grab_dx = e->x - cx;
            grab_dy = e->y - cy;
            drag_x = cx;
            drag_y = cy;
        }
        if (touch_kind == T_NEW || touch_kind == T_UNDO)
            draw();
        return;
    }
    if (e->type == GZ_EV_TOUCH_MOVE) {
        if (touch_kind == T_CARD) {
            if (!dragging && abs(e->x - down_x) + abs(e->y - down_y) > DRAG_SLOP)
                drag_begin();
            if (dragging)
                drag_to(e->x, e->y);
        }
        return;
    }
    if (e->type != GZ_EV_TOUCH_UP)
        return;
    int kind = touch_kind;
    touch_kind = T_NONE;
    switch (kind) {
    case T_NEW:
        if (gfx_rect_contains(new_rect, e->x, e->y))
            alert = st.moves && !st.won ? A_CONFIRM : A_DRAW;
        break;
    case T_UNDO:
        if (gfx_rect_contains(undo_rect, e->x, e->y)) {
            undo();
            save();
        }
        break;
    case T_STOCK:
        if (!st.won) {
            turn_stock();
            save();
        }
        break;
    case T_CARD:
        if (dragging ? drop() : auto_move(drag_pile, drag_count))
            after_move();
        dragging = false;
        break;
    }
    draw();
}

int main(void)
{
    ui_init(SB_STATUS_GRAY);
    rng = (uint32_t)gz_time_us() | 1;
    load();
    draw();
    for (;;) {
        struct gz_input_event ev;
        if (!ui_next_event(&ev, -1))
            continue;
        if (ev.type == GZ_EV_TOUCH_CANCEL) {
            /* Home mid-touch: a dragged run goes back where it was. */
            touch_kind = T_NONE;
            dragging = false;
            alert_pressed = -1;
            draw();
        } else if (alert != A_NONE) {
            alert_event(&ev);
            draw();
        } else {
            touch(&ev);
        }
    }
}
