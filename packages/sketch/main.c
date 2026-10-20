/* One persistent RGB565 canvas with six inks and an eraser. */
#include <glaze/fs.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#define CW 224
#define CH 204
#define CY 38
static uint16_t pixels[CW * CH];
static gfx_surface canvas;
static uint32_t colors[] = {0x252525, 0xD63E36, 0xE59B20, 0x429347, 0x267FC5, 0x9B4AA0, 0xFFFFFF};
static int ink, lx = -1, ly;
static char path[FS_PATH_MAX];
static gfx_rect clear;
static bool dirty, confirm;
static uint32_t saved;
static void save(void)
{
    if (dirty && fs_write_file(path, pixels, sizeof(pixels)) == sizeof(pixels)) {
        dirty = false;
        saved = gz_time_ms();
    }
}
static void draw(void)
{
    gfx_surface *s = &ui.surf;
    ui_draw_pinstripes(s, gfx_r(0, 0, UI_W, UI_H));
    ui_draw_navbar(s, "Sketch", NULL, NULL, 0);
    clear = ui_draw_bar_button(s, confirm ? "Sure?" : "Clear", true, false, false, 0);
    gfx_round_rect(s, gfx_r(6, CY - 2, CW + 4, CH + 4), 3, 0x9099A7, 255);
    gfx_blit(s, 8, CY, &canvas, gfx_r(0, 0, CW, CH));
    for (int i = 0; i < 7; i++) {
        int x = 21 + i * 33;
        if (i == ink)
            gfx_circle(s, x, 267, 14, 0x687C99, 255);
        gfx_circle(s, x, 267, 11, 0xFFFFFF, 255);
        gfx_circle(s, x, 267, 9, colors[i], 255);
        if (i == 6)
            gfx_text_center(s, x - 8, 261, 16, ui_font(FONT_B10), "E", 0x7C8697, 255);
    }
    gfx_text_center(s, 0, 289, UI_W, ui_font(FONT_R10), dirty ? "Saving..." : "A little room for your ideas.",
                    0x5C6981, 255);
    ui_invalidate_all();
    ui_present();
}
int main(void)
{
    ui_init(SB_STATUS_GRAY);
    gfx_surface_init(&canvas, pixels, CW, CH, CW);
    snprintf(path, sizeof(path), "%s/data", res_bundle());
    fs_mkdirs(path);
    snprintf(path, sizeof(path), "%s/data/canvas", res_bundle());
    if (fs_read_file(path, pixels, sizeof(pixels)) != sizeof(pixels)) {
        gfx_fill(&canvas, gfx_r(0, 0, CW, CH), 0xFFFFFF);
        dirty = true;
        save();
    }
    draw();
    struct gz_input_event e;
    for (;;) {
        if (!ui_next_event(&e, 80)) {
            if (dirty && gz_time_ms() - saved > 150) {
                save();
                draw();
            }
            continue;
        }
        if (e.type == GZ_EV_TOUCH_DOWN || e.type == GZ_EV_TOUCH_MOVE) {
            if (e.x >= 8 && e.x < 8 + CW && e.y >= CY && e.y < CY + CH) {
                int x = e.x - 8, y = e.y - CY;
                if (e.type == GZ_EV_TOUCH_DOWN) {
                    lx = x;
                    ly = y;
                    confirm = false;
                }
                if (lx >= 0) {
                    gfx_line(&canvas, lx, ly, x, y, colors[ink], 255);
                    gfx_circle(&canvas, x, y, ink == 6 ? 5 : 1, colors[ink], 255);
                }
                lx = x;
                ly = y;
                dirty = true;
                draw();
                if (gz_time_ms() - saved > 150)
                    save();
            } else
                lx = -1;
        } else if (e.type == GZ_EV_TOUCH_UP) {
            lx = -1;
            if (gfx_rect_contains(clear, e.x, e.y)) {
                if (confirm) {
                    gfx_fill(&canvas, gfx_r(0, 0, CW, CH), 0xFFFFFF);
                    dirty = true;
                    confirm = false;
                } else
                    confirm = true;
            } else if (e.y >= 252 && e.y <= 282) {
                int i = (e.x - 5) / 33;
                if (i >= 0 && i < 7)
                    ink = i;
                confirm = false;
            }
            save();
            draw();
        }
    }
}
