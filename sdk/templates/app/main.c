/* A touch counter with per-bundle state. See docs/SDK.md in the SDK. */
#include "app.h"
#include <glaze/fs.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>

static uint32_t count;
static bool pressed;
static gfx_rect button;
static char state_path[FS_PATH_MAX];

static void save(void)
{
    int n = fs_write_file(state_path, &count, sizeof(count));
    if (n != (int)sizeof(count))
        printf("sdk-app: save failed (%d)\n", n);
}

static void draw(void)
{
    char text[48];
    ui_draw_pinstripes(&ui.surf, gfx_r(0, 0, UI_W, UI_H));
    ui_draw_navbar(&ui.surf, APP_TITLE, NULL, NULL, 0);
    gfx_text_center(&ui.surf, 0, 85, UI_W, ui_font(FONT_B20), "Hello, Glaze!", UI_TEXT, 255);
    snprintf(text, sizeof(text), "Tapped %u times", count);
    gfx_text_center(&ui.surf, 0, 125, UI_W, ui_font(FONT_R14), text, UI_TEXT, 255);
    button = gfx_r(40, 180, UI_W - 80, 44);
    ui_draw_button(&ui.surf, button, "Tap Me", pressed);
    ui_invalidate_all();
    ui_present();
}

int main(void)
{
    char dir[FS_PATH_MAX];
    ui_init(SB_STATUS_GRAY);
    snprintf(dir, sizeof(dir), "%s/data", res_bundle());
    if (fs_mkdirs(dir) < 0)
        puts("sdk-app: could not create data directory");
    snprintf(state_path, sizeof(state_path), "%s/data/count", res_bundle());
    if (fs_read_file(state_path, &count, sizeof(count)) != (int)sizeof(count))
        count = 0;
    printf("sdk-app: ready count=%u bundle=%s\n", count, res_bundle());
    draw();
    for (;;) {
        struct gz_input_event ev;
        if (!ui_next_event(&ev, -1))
            continue;
        if (ev.type == GZ_EV_TOUCH_DOWN || ev.type == GZ_EV_TOUCH_MOVE) {
            pressed = gfx_rect_contains(button, ev.x, ev.y);
        } else if (ev.type == GZ_EV_TOUCH_UP) {
            if (pressed && gfx_rect_contains(button, ev.x, ev.y)) {
                count++;
                save();
                printf("sdk-app: count=%u\n", count);
            }
            pressed = false;
        } else if (ev.type == GZ_EV_TOUCH_CANCEL) {
            pressed = false;
        } else if (ev.type == GZ_EV_APP_DEACTIVATE) {
            pressed = false;
            save(); /* Save before ui_next_event suspends this process. */
            continue;
        }
        draw();
    }
}
