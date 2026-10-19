/* Passcode entry: title, four digit boxes, a message line and a 3 x 4
 * numeric keypad along the bottom of an area (iPhone OS 1-3 style). Only
 * libgfx and the fonts, so SpringBoard and the host preview use it too. */

#include <glaze/ui.h>

#define KEY_GAP     1
#define BOX_W       40
#define BOX_GAP     12
#define TOP_H       108     /* title, boxes and message above the keypad */

static int key_h(gfx_rect area)
{
    int h = (area.h - TOP_H) / 4;
    return h > 44 ? 44 : h;
}

static gfx_rect keypad_rect(gfx_rect area)
{
    int h = key_h(area) * 4;
    return gfx_r(area.x, area.y + area.h - h, area.w, h);
}

/* Key at row r, column c: '1'..'9', then cancel, '0', backspace. */
static int key_at(const struct ui_passcode *pc, int r, int c)
{
    if (r < 3)
        return '1' + r * 3 + c;
    if (c == 0)
        return pc->cancel ? UI_PASSCODE_CANCEL : -1;
    return c == 1 ? '0' : '\b';
}

static gfx_rect key_rect(gfx_rect area, int r, int c)
{
    gfx_rect kp = keypad_rect(area);
    int kh = key_h(area);
    int x0 = kp.x + kp.w * c / 3, x1 = kp.x + kp.w * (c + 1) / 3;

    return gfx_r(x0, kp.y + r * kh, x1 - x0, kh);
}

void ui_draw_passcode(gfx_surface *s, gfx_rect area, const struct ui_passcode *pc)
{
    const gfx_font *tf = ui_font(FONT_B14), *mf = ui_font(FONT_R12), *kf = ui_font(FONT_B20),
                   *sf = ui_font(FONT_B12);
    uint32_t fg = pc->dark ? 0x2B3442 : 0xFFFFFF;
    int y = area.y + 10;

    if (pc->title)
        gfx_text_center(s, area.x, y, area.w, tf, pc->title, fg, 255);
    int bx = area.x + (area.w - (4 * BOX_W + 3 * BOX_GAP)) / 2, by = y + 26;
    for (int i = 0; i < UI_PASSCODE_LEN; i++) {
        gfx_rect b = gfx_r(bx + i * (BOX_W + BOX_GAP), by, BOX_W, BOX_W);
        gfx_round_rect(s, b, 5, 0x5A6170, 255);
        gfx_round_rect_vgradient(s, gfx_r(b.x + 1, b.y + 1, b.w - 2, b.h - 2), 4, 0xE8E8E8, 0xFFFFFF, 255);
        if (i < pc->digits)
            gfx_circle(s, b.x + b.w / 2, b.y + b.h / 2, 6, 0x111111, 255);
    }
    if (pc->msg)
        gfx_text_center(s, area.x, by + BOX_W + 10, area.w, mf, pc->msg, pc->dark ? 0x4C566C : 0xFFFFFF, 255);

    gfx_rect kp = keypad_rect(area);
    gfx_fill(s, kp, 0x2A2E36);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            int k = key_at(pc, r, c);
            gfx_rect kr = key_rect(area, r, c);
            gfx_rect in = gfx_r(kr.x + (c ? KEY_GAP : 0), kr.y + KEY_GAP, kr.w - (c ? KEY_GAP : 0), kr.h - KEY_GAP);
            bool down = k >= 0 && k == pc->pressed;
            bool digit = k >= '0' && k <= '9';
            if (digit)
                gfx_vgradient(s, in, down ? 0x5E95F2 : 0xF4F5F7, down ? 0x2A63D4 : 0xC9CDD4, 255);
            else
                gfx_vgradient(s, in, down ? 0x5E95F2 : 0x9AA1AE, down ? 0x2A63D4 : 0x6E7686, 255);
            if (k < 0)
                continue;
            const char *label = k == '\b' ? GFX_CH_LARROW : k == UI_PASSCODE_CANCEL ? pc->cancel : NULL;
            char d[2] = { (char)k, 0 };
            const gfx_font *f = k == UI_PASSCODE_CANCEL ? sf : kf;
            uint32_t col = (digit && !down) ? 0x111111 : 0xFFFFFF;
            gfx_text_center(s, in.x, in.y + (in.h - f->line_height) / 2, in.w, f, label ? label : d, col, 255);
        }
    }
}

int ui_passcode_hit(gfx_rect area, const struct ui_passcode *pc, int x, int y)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            if (gfx_rect_contains(key_rect(area, r, c), x, y))
                return key_at(pc, r, c);
        }
    }
    return -1;
}
