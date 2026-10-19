/*
 * Hello: the test app package (packages/, glaze/pkg.h). It is installed from
 * Files rather than built into romfs, shows which volume it runs from, and
 * counts taps in a file of its own under <bundle>/data, which deleting the
 * app removes with it.
 */

#include <glaze/fs.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <string.h>

static uint32_t count;
static bool pressed;
static gfx_rect button;
static gfx_image logo;
static bool logo_ok;

static void data_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/data/count", res_bundle());
}

static void load(void)
{
    char path[FS_PATH_MAX];

    data_path(path, sizeof(path));
    if (fs_read_file(path, &count, sizeof(count)) != sizeof(count))
        count = 0;
}

static void save(void)
{
    char path[FS_PATH_MAX], dir[FS_PATH_MAX];

    data_path(path, sizeof(path));
    if (fs_write_file(path, &count, sizeof(count)) == GZ_ENOENT) {
        snprintf(dir, sizeof(dir), "%s/data", res_bundle());
        fs_mkdirs(dir);
        fs_write_file(path, &count, sizeof(count));
    }
}

static void draw(void)
{
    gfx_surface *s = &ui.surf;
    const gfx_font *big = ui_font(FONT_B20), *small = ui_font(FONT_R12);
    const char *bundle = res_bundle();
    char line[64];
    int y = UI_NAV_H + 24;

    ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H));
    ui_draw_navbar(s, "Hello", NULL, NULL, 0);
    if (logo_ok) {
        gfx_draw_image(s, (UI_W - logo.w) / 2, y, &logo, 255);
        y += logo.h + 12;
    }
    gfx_text_center(s, 0, y, UI_W, big, "Hello, Glaze!", UI_TEXT, 255);
    y += big->line_height + 6;
    snprintf(line, sizeof(line), "Installed on the %s.", !strncmp(bundle, "/Card/", 6) ? "card" : "flash");
    gfx_text_center(s, 0, y, UI_W, small, line, 0x4C566C, 255);
    gfx_text_center(s, 0, y + small->line_height + 2, UI_W, ui_font(FONT_R10), bundle, 0x6C7686, 255);
    snprintf(line, sizeof(line), count == 1 ? "Tapped once" : "Tapped %u times", count);
    gfx_text_center(s, 0, UI_H - 100, UI_W, ui_font(FONT_B14), line, UI_TEXT, 255);
    button = gfx_r(40, UI_H - 70, UI_W - 80, 40);
    ui_draw_button(s, button, "Tap Me", pressed);
    ui_invalidate_all();
    ui_present();
}

int main(void)
{
    ui_init(SB_STATUS_GRAY);
    logo_ok = res_image(&logo, "icon.gzi");     /* from the bundle's res/ */
    load();
    draw();
    for (;;) {
        struct gz_input_event ev;
        if (!ui_next_event(&ev, -1))
            continue;
        if (ev.type == GZ_EV_TOUCH_DOWN || ev.type == GZ_EV_TOUCH_MOVE) {
            bool in = gfx_rect_contains(button, ev.x, ev.y);
            if (in != pressed) {
                pressed = in;
                draw();
            }
        } else if (ev.type == GZ_EV_TOUCH_UP) {
            if (pressed && gfx_rect_contains(button, ev.x, ev.y)) {
                count++;
                save();
            }
            pressed = false;
            draw();
        }
    }
}
