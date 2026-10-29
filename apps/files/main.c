/*
 * Files: browse the flash file system (servers/fs). Folders push like
 * Settings' screens; text files and pictures open in viewers. Edit mode
 * deletes (minus badge, then a Delete button, as on the iPhone), renames
 * by tapping a row, and makes new folders.
 */

#include <glaze/fs.h>
#include <glaze/pkg.h>
#include <glaze/notify.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_ENTRIES     128
#define ROW_H           36
#define TOOLBAR_H       34
#define FOOTER_H        34
#define TEXT_MAX        (32 * 1024)
#define IMAGE_MAX       (256 * 1024)
#define MARGIN          10
#define TEXT_LINE_H     17

enum view { V_LIST, V_NAME, V_TEXT, V_IMAGE, V_INFO, V_PACKAGE };
enum kind { K_FOLDER, K_TEXT, K_IMAGE, K_PACKAGE, K_WINCE, K_OTHER };

static enum view view = V_LIST;

/* Current folder. */
static char cwd[FS_PATH_MAX] = "/";
static struct fs_dirent ents[MAX_ENTRIES];
static int nents;
static struct fs_info info;
static struct ui_list list;
static bool editing;
static int confirm_row = -1;        /* row showing its Delete button */
static int list_err;                /* listing failed with this error */

/* Touch state. */
static enum {
    P_NONE, P_BACK, P_EDIT, P_NEWFOLDER, P_DELETE, P_BADGE, P_CANCEL, P_SAVE, P_FLASH, P_CARD
} pressed;
static gfx_rect back_rect, edit_rect, newfolder_rect, delete_rect, cancel_rect, save_rect;
static gfx_rect flash_rect, card_rect;

/* Alert: a message, or a confirmation for deleting a non-empty folder. */
static bool alert_open, alert_confirm;
static char alert_title[40], alert_msg[64];
static int alert_pressed = -1, alert_row = -1;
static gfx_rect alert_r1, alert_r2;

/* Name sheet (new folder / rename). */
static bool naming_new;
static int naming_row, name_cursor;
static char name_buf[FS_NAME_MAX];
static struct ui_keyboard kb;

/* Viewers. */
static int open_row;
static char *text_buf;
static int text_len;
static bool text_truncated;
static int line_starts[2000];
static int nlines;
static struct ui_scroll text_scroll;
static uint8_t *image_buf;
static gfx_image image;
static bool image_ok;

/* Package sheet (glaze/pkg.h). */
static struct pkg_info pkg;
static int pkg_err;                 /* the package can't be read */
static char pkg_replaces[16];       /* version already installed, or "" */
static bool pkg_card;               /* a card is in */
static bool pkg_busy;
static uint8_t pkg_icon_buf[16 * 1024];
static gfx_image pkg_icon;
static bool pkg_icon_ok;

/* ---- helpers -------------------------------------------------------------- */

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int name_cmp(const char *a, const char *b)
{
    for (; *a && lower(*a) == lower(*b); a++, b++)
        ;
    return lower(*a) - lower(*b);
}

static int ent_cmp(const void *pa, const void *pb)
{
    const struct fs_dirent *a = pa, *b = pb;

    if (a->type != b->type)
        return a->type == FS_DIR ? -1 : 1;
    return name_cmp(a->name, b->name);
}

static bool has_ext(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');

    return dot && !name_cmp(dot + 1, ext);
}

static enum kind kind_of(const struct fs_dirent *e)
{
    if (e->type == FS_DIR)
        return K_FOLDER;
    if (has_ext(e->name, "txt") || has_ext(e->name, "text") || has_ext(e->name, "md") ||
        has_ext(e->name, "log"))
        return K_TEXT;
    if (has_ext(e->name, "gzi"))
        return K_IMAGE;
    if (has_ext(e->name, "gzp"))
        return K_PACKAGE;
    if(has_ext(e->name,"exe"))return K_WINCE;
    if(has_ext(e->name,"cpk")){
        size_t n=strlen(e->name);
        if(n>=8&&!strncasecmp(e->name+n-8,".exe.cpk",8))return K_WINCE;
    }
    return K_OTHER;
}

static const char *kind_name(enum kind k)
{
    static const char *const names[] = { "Folder", "Text Document", "Picture", "App Package", "Windows CE App", "Document" };
    return names[k];
}

static void format_size(uint32_t bytes, char *buf, size_t n)
{
    if (bytes < 1024)
        snprintf(buf, n, "%u bytes", bytes);
    else if (bytes < 1024 * 1024)
        snprintf(buf, n, "%u KB", (bytes + 1023) / 1024);
    else
        snprintf(buf, n, "%u.%u MB", bytes / (1024 * 1024), bytes % (1024 * 1024) * 10 / (1024 * 1024));
}

/* The root's "Card" entry: a CompactFlash card's volume, which can't be
 * renamed or deleted. */
static bool is_card(int row)
{
    return !strcmp(cwd, "/") && ents[row].type == FS_DIR && !strcmp(ents[row].name, "Card");
}

static void join(char *out, const char *dir, const char *name)
{
    snprintf(out, FS_PATH_MAX, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
}

/* Last path component, or "Files" for the root. */
static const char *folder_title(const char *path)
{
    const char *slash = strrchr(path, '/');

    return !strcmp(path, "/") ? "Files" : slash + 1;
}

static void parent_of(const char *path, char *out)
{
    strlcpy(out, path, FS_PATH_MAX);
    char *slash = strrchr(out, '/');
    if (slash == out)
        out[1] = 0;
    else if (slash)
        *slash = 0;
}

static void show_alert(const char *title, const char *msg)
{
    strlcpy(alert_title, title, sizeof(alert_title));
    strlcpy(alert_msg, msg, sizeof(alert_msg));
    alert_open = true;
    alert_confirm = false;
    alert_pressed = -1;
}

/* ---- loading ---------------------------------------------------------------- */

static gfx_rect list_area(void)
{
    return gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H - (editing ? TOOLBAR_H : 0));
}

static void load_dir(void)
{
    int n = fs_list(cwd, 0, ents, MAX_ENTRIES);

    list_err = n < 0 ? n : 0;
    nents = n < 0 ? 0 : n;
    qsort(ents, (size_t)nents, sizeof(ents[0]), ent_cmp);
    if (fs_statvfs(cwd, &info) < 0)
        memset(&info, 0, sizeof(info));
    confirm_row = -1;
    ui_list_set_area(&list, list_area());
    ui_list_reset(&list, nents);
}

static void load_text(int row)
{
    char path[FS_PATH_MAX];

    if (!text_buf)
        text_buf = malloc(TEXT_MAX + 1);
    join(path, cwd, ents[row].name);
    int n = text_buf ? fs_read_file(path, text_buf, TEXT_MAX) : GZ_ENOMEM;
    if (n < 0) {
        snprintf(text_buf, TEXT_MAX, "%s.", fs_strerror(n));
        n = (int)strlen(text_buf);
    }
    text_len = n;
    text_buf[n] = 0;
    /* Show tabs and other control characters as spaces. */
    for (int i = 0; i < n; i++) {
        if ((uint8_t)text_buf[i] < ' ' && text_buf[i] != '\n')
            text_buf[i] = ' ';
    }
    text_truncated = ents[row].size > TEXT_MAX;
    nlines = ui_wrap(ui_font(FONT_R12), text_buf, UI_W - MARGIN * 2, line_starts,
                     (int)(sizeof(line_starts) / sizeof(line_starts[0])));
    ui_scroll_reset(&text_scroll, nlines * TEXT_LINE_H + MARGIN * 2 + (text_truncated ? 24 : 0),
                    UI_H - UI_NAV_H);
}

static void load_image(int row)
{
    char path[FS_PATH_MAX];

    image_ok = false;
    if (!image_buf)
        image_buf = malloc(IMAGE_MAX);
    join(path, cwd, ents[row].name);
    int n = image_buf ? fs_read_file(path, image_buf, IMAGE_MAX) : GZ_ENOMEM;
    image_ok = n > 0 && gfx_image_parse(&image, image_buf, (size_t)n);
}

/* ---- drawing: list ------------------------------------------------------------ */

/* A nav-bar title centred between `left` and `right`, shortened to fit;
 * ui_draw_navbar can't shorten. */
static void nav_title(gfx_surface *s, const char *title, int left, int right, uint32_t shadow)
{
    const gfx_font *tf = ui_font(FONT_B14);
    int room = MIN(right - left, UI_W - 2 * MAX(UI_W - right, left));
    int tw = MIN(gfx_text_width(tf, title), room);
    int tx = MAX((UI_W - tw) / 2, left), ty = (UI_NAV_H - tf->line_height) / 2;

    gfx_text_fit(s, tx, ty - 1, right - tx, tf, title, shadow, 255);
    gfx_text_fit(s, tx, ty, right - tx, tf, title, 0xFFFFFF, 255);
}

static void draw_folder_glyph(gfx_surface *s, int x, int y)
{
    gfx_round_rect(s, gfx_r(x + 1, y + 4, 10, 5), 2, 0xB97E22, 255);
    gfx_round_rect(s, gfx_r(x + 1, y + 6, 22, 15), 2, 0xB97E22, 255);
    gfx_fill(s, gfx_r(x + 3, y + 8, 18, 6), 0xFFFFFF);
    gfx_round_rect_vgradient(s, gfx_r(x, y + 10, 24, 12), 2, 0xFFDB7A, 0xE9A93C, 255);
}

/* A CompactFlash card: a dark slab with a label and a row of contacts. */
static void draw_card_glyph(gfx_surface *s, int x, int y)
{
    gfx_round_rect_vgradient(s, gfx_r(x + 1, y + 2, 22, 20), 2, 0x5A6270, 0x2E343D, 255);
    gfx_fill(s, gfx_r(x + 4, y + 5, 16, 9), 0xD8DCE2);
    gfx_fill(s, gfx_r(x + 6, y + 8, 12, 1), 0x7A8290);
    gfx_fill(s, gfx_r(x + 6, y + 10, 8, 1), 0x7A8290);
    for (int i = 0; i < 5; i++)
        gfx_fill(s, gfx_r(x + 4 + i * 4, y + 18, 2, 2), 0xC9A64A);
}

static void draw_doc_glyph(gfx_surface *s, int x, int y, enum kind k)
{
    gfx_rect page = gfx_r(x + 4, y + 1, 17, 22);

    gfx_fill(s, page, 0x9AA0A8);
    gfx_fill(s, gfx_r(page.x + 1, page.y + 1, page.w - 2, page.h - 2), 0xFFFFFF);
    /* Folded corner. */
    gfx_fill(s, gfx_r(page.x + page.w - 5, page.y, 5, 5), 0xFFFFFF);
    gfx_line(s, page.x + page.w - 5, page.y, page.x + page.w - 1, page.y + 4, 0x9AA0A8, 255);
    gfx_fill(s, gfx_r(page.x + page.w - 5, page.y + 1, 1, 4), 0x9AA0A8);
    gfx_fill(s, gfx_r(page.x + page.w - 5, page.y + 4, 4, 1), 0x9AA0A8);
    if (k == K_IMAGE) {
        gfx_rect pic = gfx_r(page.x + 3, page.y + 7, page.w - 6, 11);
        gfx_vgradient(s, pic, 0x3D8FD6, 0xB9E1F7, 255);
        gfx_fill(s, gfx_r(pic.x, pic.y + 7, pic.w, 4), 0x4E9A35);
        gfx_circle(s, pic.x + 8, pic.y + 4, 2, 0xFFD23C, 255);
    } else if (k == K_PACKAGE) {
        /* A small cardboard box. */
        gfx_rect box = gfx_r(page.x + 2, page.y + 8, page.w - 4, 11);
        gfx_vgradient(s, box, 0xD9A55E, 0xA8743A, 255);
        gfx_fill(s, gfx_r(box.x, box.y, box.w, 3), 0xE8BE7E);
        gfx_fill(s, gfx_r(box.x + box.w / 2 - 1, box.y, 2, box.h), 0xF3E2C0);
    } else if (k == K_TEXT) {
        for (int i = 0; i < 5; i++)
            gfx_fill(s, gfx_r(page.x + 3, page.y + 7 + i * 3, i == 4 ? 7 : page.w - 6, 1), 0x8E9AAB);
    }
}

static void draw_row(gfx_surface *s, int i, gfx_rect r, bool pressed_here, void *ctx)
{
    const struct fs_dirent *e = &ents[i];
    enum kind k = kind_of(e);
    bool sel = pressed_here && confirm_row < 0;
    int y = r.y;

    (void)ctx;
    const gfx_font *nf = ui_font(FONT_B13), *df = ui_font(FONT_R11);
    int x = MARGIN;
    char size[16];

    if (sel)
        gfx_vgradient(s, gfx_r(0, y, UI_W, ROW_H - 1), UI_SELECT_TOP, UI_SELECT_BOTTOM, 255);
    if (editing) {
        if (!is_card(i))
            ui_draw_delete_badge(s, MARGIN + 7, y + ROW_H / 2, confirm_row == i);
        x += UI_DELETE_BADGE_W;
    }
    if (is_card(i))
        draw_card_glyph(s, x, y + (ROW_H - 24) / 2);
    else if (k == K_FOLDER)
        draw_folder_glyph(s, x, y + (ROW_H - 24) / 2);
    else
        draw_doc_glyph(s, x, y + (ROW_H - 24) / 2, k);
    x += 32;

    int right = UI_W - MARGIN;
    if (confirm_row == i) {
        delete_rect = ui_draw_delete_button(s, UI_W - 8, y + ROW_H / 2, pressed == P_DELETE);
        right = delete_rect.x - 6;
    } else if (k == K_FOLDER) {
        if (!editing)
            ui_draw_chevron(s, UI_W - 16, y + ROW_H / 2 - 5, sel ? 0xFFFFFF : 0x808080);
        right = UI_W - 24;
    } else {
        format_size(e->size, size, sizeof(size));
        int w = gfx_text_width(df, size);
        gfx_text(s, right - w, y + (ROW_H - df->line_height) / 2, df, size, sel ? 0xFFFFFF : 0x808080, 255);
        right -= w + 8;
    }
    gfx_text_fit(s, x, y + (ROW_H - nf->line_height) / 2, right - x, nf, e->name, sel ? 0xFFFFFF : UI_TEXT, 255);
    gfx_fill(s, gfx_r(0, y + ROW_H - 1, UI_W, 1), 0xE0E0E0);
}

static void draw_toolbar(gfx_surface *s)
{
    gfx_rect r = gfx_r(0, UI_H - TOOLBAR_H, UI_W, TOOLBAR_H);
    const gfx_font *f = ui_font(FONT_B11);
    bool p = pressed == P_NEWFOLDER;

    gfx_vgradient(s, gfx_r(0, r.y, UI_W, r.h / 2), UI_NAV_TOP, 0x8D9DB4, 255);
    gfx_vgradient(s, gfx_r(0, r.y + r.h / 2, UI_W, r.h - r.h / 2), 0x8195AF, UI_NAV_BOTTOM, 255);
    gfx_fill(s, gfx_r(0, r.y, UI_W, 1), UI_NAV_LINE);
    newfolder_rect = gfx_r(8, r.y + 5, gfx_text_width(f, "New Folder") + 20, 24);
    gfx_round_rect(s, newfolder_rect, 5, 0x2A3442, 220);
    gfx_round_rect_vgradient(s, gfx_r(newfolder_rect.x + 1, newfolder_rect.y + 1, newfolder_rect.w - 2,
                                      newfolder_rect.h - 2), 4, p ? 0x3F4A58 : 0x7E93AF,
                             p ? 0x25313F : 0x4A6383, 255);
    gfx_text_shadow(s, newfolder_rect.x + 10, newfolder_rect.y + (24 - f->line_height) / 2, f, "New Folder",
                    0xFFFFFF, 0x2A3442, -1, 255);
}

static void draw_list(gfx_surface *s)
{
    gfx_rect body = list.area;
    char footer[64], avail[16], parent[FS_PATH_MAX];

    gfx_set_clip(s, body);
    gfx_fill(s, body, 0xFFFFFF);
    ui_list_draw(s, &list, draw_row, NULL);
    int fy = ui_list_row_y(&list, nents);
    if (list_err) {
        gfx_text_center(s, 0, body.y + 90, UI_W, ui_font(FONT_B17), "Storage Unavailable", 0x999999, 255);
        gfx_text_center(s, 0, body.y + 116, UI_W, ui_font(FONT_R12), fs_strerror(list_err), 0x999999, 255);
    } else if (!nents) {
        gfx_text_center(s, 0, body.y + 100, UI_W, ui_font(FONT_B20), "No Files", 0xB0B0B0, 255);
    } else {
        format_size(info.total > info.used ? info.total - info.used : 0, avail, sizeof(avail));
        snprintf(footer, sizeof(footer), "%d item%s, %s available", nents, nents == 1 ? "" : "s", avail);
        gfx_text_center(s, 0, fy + 10, UI_W, ui_font(FONT_R12), footer, 0x808080, 255);
    }
    gfx_reset_clip(s);

    bool root = !strcmp(cwd, "/");
    if (!root)
        parent_of(cwd, parent);
    ui_draw_navbar(s, NULL, root ? NULL : folder_title(parent), &back_rect, 0);
    if (root)
        back_rect = gfx_r(0, 0, 0, 0);
    edit_rect = list_err ? gfx_r(0, 0, 0, 0)
                         : ui_draw_bar_button(s, editing ? "Done" : "Edit", true, pressed == P_EDIT, editing, 0);
    nav_title(s, folder_title(cwd), MAX(back_rect.x + back_rect.w, 6), edit_rect.w ? edit_rect.x - 4 : UI_W - 6,
              0x3E4E66);
    if (editing)
        draw_toolbar(s);
}

/* ---- drawing: name sheet ------------------------------------------------------ */

static bool name_valid(void)
{
    return name_buf[0] && !strchr(name_buf, '/') && strcmp(name_buf, ".") && strcmp(name_buf, "..");
}

static void draw_name(gfx_surface *s)
{
    const gfx_font *f = ui_font(FONT_R14);
    gfx_rect field = gfx_r(MARGIN, UI_NAV_H + 16, UI_W - MARGIN * 2, 30);

    ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H - ui_keyboard_height()));
    ui_draw_navbar(s, naming_new ? "New Folder" : "Rename", NULL, NULL, 0);
    cancel_rect = ui_draw_bar_button(s, "Cancel", false, pressed == P_CANCEL, false, 0);
    save_rect = ui_draw_bar_button(s, "Save", true, pressed == P_SAVE, name_valid(), 0);

    gfx_round_rect(s, field, 6, 0x9AA0A8, 255);
    gfx_round_rect(s, gfx_r(field.x + 1, field.y + 1, field.w - 2, field.h - 2), 5, 0xFFFFFF, 255);
    int tx = field.x + 8, ty = field.y + (field.h - f->line_height) / 2;
    int w = gfx_text_width_n(f, name_buf, name_cursor), maxw = field.w - 20;
    /* Keep the end of a long name (and the cursor) in view. */
    gfx_set_clip(s, gfx_r(field.x + 4, field.y, field.w - 8, field.h));
    int shift = w > maxw ? w - maxw : 0;
    ui_draw_edit_selection(s, f, name_buf, name_cursor, 0, strlen(name_buf), tx - shift, ty, f->line_height);
    if (name_buf[0])
        gfx_text(s, tx - shift, ty, f, name_buf, UI_TEXT, 255);
    else
        gfx_text(s, tx, ty, f, naming_new ? "Folder name" : "Name", 0xB0B0B0, 255);
    gfx_fill(s, gfx_r(tx - shift + w + 1, field.y + 6, 2, field.h - 12), 0x3B6FD8);
    gfx_reset_clip(s);
    if (!ui_has_hardware_keyboard())
        ui_draw_keyboard(s, &kb, UI_H - UI_KB_H);
}

/* ---- drawing: viewers ----------------------------------------------------------- */

static void draw_viewer_nav(gfx_surface *s, int style)
{
    ui_draw_navbar(s, NULL, folder_title(cwd), &back_rect, style);
    nav_title(s, ents[open_row].name, back_rect.w, UI_W - 6, style ? 0x000000 : 0x3E4E66);
}

static void draw_text(gfx_surface *s)
{
    gfx_rect body = gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H);
    const gfx_font *f = ui_font(FONT_R12);

    gfx_set_clip(s, body);
    gfx_fill(s, body, 0xFFFFFF);
    for (int l = 0; l < nlines; l++) {
        int y = body.y + MARGIN + l * TEXT_LINE_H - text_scroll.offset;
        if (y + TEXT_LINE_H < body.y || y > body.y + body.h)
            continue;
        gfx_text_n(s, MARGIN, y, f, text_buf + line_starts[l], ui_line_len(text_buf, line_starts, nlines, l),
                   UI_TEXT, 255);
    }
    if (text_truncated) {
        int y = body.y + MARGIN + nlines * TEXT_LINE_H + 6 - text_scroll.offset;
        gfx_text_center(s, 0, y, UI_W, ui_font(FONT_R11), "Only the first 32 KB is shown.", 0x808080, 255);
    }
    gfx_reset_clip(s);
    draw_viewer_nav(s, 0);
}

static void draw_image(gfx_surface *s)
{
    gfx_rect body = gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H);

    gfx_fill(s, body, 0x000000);
    if (image_ok) {
        /* Fit inside the body, keeping the aspect ratio. */
        int w = body.w, h = image.h * body.w / image.w;
        if (h > body.h) {
            h = body.h;
            w = image.w * body.h / image.h;
        }
        gfx_set_clip(s, body);
        gfx_draw_image_scaled(s, gfx_r(body.x + (body.w - w) / 2, body.y + (body.h - h) / 2, w, h), &image, 255);
        gfx_reset_clip(s);
    } else {
        gfx_text_center(s, 0, body.y + 120, UI_W, ui_font(FONT_B14), "Can't open this picture.", 0xA0A0A0, 255);
    }
    draw_viewer_nav(s, 1);
}

static void draw_info(gfx_surface *s)
{
    const struct fs_dirent *e = &ents[open_row];
    const gfx_font *lf = ui_font(FONT_B13), *vf = ui_font(FONT_R13);
    char size[16];
    const char *labels[3] = { "Name", "Kind", "Size" };
    const char *values[3] = { e->name, kind_name(kind_of(e)), size };

    format_size(e->size, size, sizeof(size));
    ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H));
    draw_viewer_nav(s, 0);
    for (int i = 0; i < 3; i++) {
        gfx_rect r = gfx_r(10, UI_NAV_H + 12 + i * UI_ROW_H, UI_W - 20, UI_ROW_H);
        ui_draw_group_cell(s, r, i == 0 ? 1 : i == 2 ? 3 : 2, false);
        gfx_text(s, r.x + 10, r.y + (r.h - lf->line_height) / 2, lf, labels[i], UI_TEXT, 255);
        gfx_text_fit(s, r.x + 60, r.y + (r.h - vf->line_height) / 2, r.w - 70, vf, values[i], UI_TEXT_DETAIL, 255);
    }
    gfx_text_center(s, 0, UI_NAV_H + 12 + 3 * UI_ROW_H + 14, UI_W, ui_font(FONT_R12),
                    "Files can't open this kind of document.", 0x4C566C, 255);
}

/* An app package: what it is, and where to install it. */
static void draw_package(gfx_surface *s)
{
    const gfx_font *tf = ui_font(FONT_B17), *df = ui_font(FONT_R12);
    int y = UI_NAV_H + 14;
    char line[64], size[16];

    ui_draw_pinstripes(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H));
    draw_viewer_nav(s, 0);
    if (pkg_err) {
        gfx_text_center(s, 0, y + 40, UI_W, ui_font(FONT_B14), "This package is damaged.", 0x4C566C, 255);
        gfx_text_center(s, 0, y + 62, UI_W, df, fs_strerror(pkg_err), 0x4C566C, 255);
        flash_rect = card_rect = gfx_r(0, 0, 0, 0);
        return;
    }
    if (pkg_icon_ok) {
        gfx_draw_image(s, (UI_W - pkg_icon.w) / 2, y, &pkg_icon, 255);
        y += pkg_icon.h + 8;
    }
    gfx_text_center(s, 0, y, UI_W, tf, pkg.name, UI_TEXT, 255);
    y += tf->line_height + 2;
    format_size(ents[open_row].size, size, sizeof(size));
    snprintf(line, sizeof(line), "Version %s, %s", pkg.version[0] ? pkg.version : "1.0", size);
    gfx_text_center(s, 0, y, UI_W, df, line, 0x4C566C, 255);
    y += df->line_height;
    if (pkg_replaces[0]) {
        snprintf(line, sizeof(line), "Replaces the installed version %s.", pkg_replaces);
        gfx_text_center(s, 0, y, UI_W, df, line, 0x4C566C, 255);
    }
    flash_rect = gfx_r(20, UI_H - 96, UI_W - 40, 36);
    card_rect = gfx_r(20, UI_H - 52, UI_W - 40, 36);
    if (pkg_busy) {
        gfx_text_center(s, 0, UI_H - 70, UI_W, ui_font(FONT_B14), "Installing" GFX_CH_ELLIPSIS, UI_TEXT, 255);
        return;
    }
    ui_draw_button(s, flash_rect, "Install on Flash", pressed == P_FLASH);
    if (pkg_card) {
        ui_draw_button(s, card_rect, "Install on Card", pressed == P_CARD);
    } else {
        gfx_text_center(s, 0, card_rect.y + 10, UI_W, df, "Insert a card to install on it.", 0x6C7686, 255);
        card_rect = gfx_r(0, 0, 0, 0);
    }
}

static void draw_view(gfx_surface *s)
{
    switch (view) {
    case V_LIST: draw_list(s); break;
    case V_NAME: draw_name(s); break;
    case V_TEXT: draw_text(s); break;
    case V_IMAGE: draw_image(s); break;
    case V_INFO: draw_info(s); break;
    case V_PACKAGE: draw_package(s); break;
    }
    if (alert_open)
        ui_draw_alert(s, alert_title, alert_msg, alert_confirm ? "Cancel" : "OK",
                      alert_confirm ? "Delete" : NULL, &alert_r1, &alert_r2, alert_pressed);
}

static void redraw(void)
{
    draw_view(&ui.surf);
    ui_invalidate_all();
    ui_present();
}

static void go(enum view v, bool forward)
{
    pressed = P_NONE;
    list.pressed = -1;
    view = v;
    ui_transition(draw_view, forward);
}

/* ---- actions --------------------------------------------------------------------- */

static void enter_folder(int row)
{
    char path[FS_PATH_MAX];

    join(path, cwd, ents[row].name);
    if (strlen(path) >= FS_PATH_MAX - 1)
        return;
    strlcpy(cwd, path, sizeof(cwd));
    editing = false;
    load_dir();
    go(V_LIST, true);
}

static void go_up(void)
{
    char parent[FS_PATH_MAX];

    parent_of(cwd, parent);
    strlcpy(cwd, parent, sizeof(cwd));
    editing = false;
    load_dir();
    go(V_LIST, false);
}

static void load_package(int row)
{
    char path[FS_PATH_MAX], bundle[PKG_BUNDLE_MAX];
    struct pkg_info old;
    struct fs_stat card;

    join(path, cwd, ents[row].name);
    pkg_err = pkg_read_info(path, &pkg);
    int n = pkg_err ? -1 : pkg_read_entry(path, "res/icon.gzi", pkg_icon_buf, sizeof(pkg_icon_buf));
    pkg_icon_ok = n > 0 && gfx_image_parse(&pkg_icon, pkg_icon_buf, (size_t)n);
    pkg_replaces[0] = 0;
    if (!pkg_err && pkg_find(pkg.id, bundle) == 0 && pkg_bundle_info(bundle, &old) == 0)
        strlcpy(pkg_replaces, old.version[0] ? old.version : "1.0", sizeof(pkg_replaces));
    pkg_card = fs_stat("/Card", &card) == 0;     /* statvfs would fall back to the flash */
    pkg_busy = false;
}

static void install_package(bool card)
{
    char path[FS_PATH_MAX], bundle[PKG_BUNDLE_MAX], msg[64];

    pkg_busy = true;
    redraw();
    join(path, cwd, ents[open_row].name);
    int rv = pkg_install(path, card ? "/Card" : "", bundle);
    pkg_busy = false;
    if (rv) {
        show_alert("Cannot Install", fs_strerror(rv));
        return;
    }
    snprintf(msg, sizeof(msg), "\"%s\" is on the Home screen.", pkg.name);
    show_alert("App Installed", msg);
    load_package(open_row);         /* now "replaces" the installed copy */
    if (card || !strcmp(cwd, "/"))
        load_dir();
}

static void open_entry(int row)
{
    open_row = row;
    switch (kind_of(&ents[row])) {
    case K_FOLDER: enter_folder(row); return;
    case K_TEXT: load_text(row); go(V_TEXT, true); return;
    case K_IMAGE: load_image(row); go(V_IMAGE, true); return;
    case K_PACKAGE: load_package(row); go(V_PACKAGE, true); return;
    case K_WINCE: {
        char path[FS_PATH_MAX];join(path,cwd,ents[row].name);
        int rv=sb_wince_launch(path);
        if(rv<0)show_alert("Cannot Open WinCE App",rv==GZ_ENOENT?"The CE runner or source is unavailable.":fs_strerror(rv));
        return;
    }
    case K_OTHER: go(V_INFO, true); return;
    }
}

static void start_naming(bool is_new, int row)
{
    naming_new = is_new;
    naming_row = row;
    strlcpy(name_buf, is_new ? "" : ents[row].name, sizeof(name_buf));
    name_cursor = (int)strlen(name_buf);
    ui_keyboard_edit_init(&kb, UI_EDIT_FILENAME);
    kb.shift = is_new;
    confirm_row = -1;
    go(V_NAME, true);
}

static void finish_naming(bool save)
{
    if (save) {
        char path[FS_PATH_MAX], from[FS_PATH_MAX];
        join(path, cwd, name_buf);
        int rv = strlen(cwd) + strlen(name_buf) + 2 > FS_PATH_MAX ? GZ_EINVAL : 0;
        if (!rv && naming_new) {
            rv = fs_mkdir(path);
        } else if (!rv) {
            join(from, cwd, ents[naming_row].name);
            rv = strcmp(from, path) ? fs_rename(from, path) : 0;
        }
        if (rv < 0) {
            show_alert(naming_new ? "Can't Make Folder" : "Can't Rename", fs_strerror(rv));
            redraw();
            return;
        }
    }
    load_dir();
    go(V_LIST, false);
}

static void delete_row(int row)
{
    char path[FS_PATH_MAX];

    join(path, cwd, ents[row].name);
    int rv = fs_remove_all(path);
    load_dir();
    if (rv < 0)
        show_alert("Can't Delete", fs_strerror(rv));
}

/* Deleting a folder that has something in it asks first. */
static void request_delete(int row)
{
    char path[FS_PATH_MAX];
    struct fs_dirent probe;

    join(path, cwd, ents[row].name);
    if (ents[row].type == FS_DIR && fs_list(path, 0, &probe, 1) > 0) {
        show_alert("Delete Folder?", "Everything in it will be deleted.");
        alert_confirm = true;
        alert_row = row;
        return;
    }
    delete_row(row);
}

/* ---- input ------------------------------------------------------------------------ */

static void alert_event(const struct gz_input_event *e)
{
    int h = gfx_rect_contains(alert_r1, e->x, e->y) ? 0
          : alert_confirm && gfx_rect_contains(alert_r2, e->x, e->y) ? 1 : -1;

    if (e->type != GZ_EV_TOUCH_UP) {
        alert_pressed = h;
        return;
    }
    alert_pressed = -1;
    if (h < 0)
        return;
    alert_open = false;
    if (alert_confirm && h == 1)
        delete_row(alert_row);
}

static void list_event(const struct gz_input_event *e)
{
    if (e->type == GZ_EV_TOUCH_DOWN) {
        pressed = P_NONE;
        if (gfx_rect_contains(back_rect, e->x, e->y))
            pressed = P_BACK;
        else if (gfx_rect_contains(edit_rect, e->x, e->y))
            pressed = P_EDIT;
        else if (editing && gfx_rect_contains(newfolder_rect, e->x, e->y) && e->y >= UI_H - TOOLBAR_H)
            pressed = P_NEWFOLDER;
        else if (confirm_row >= 0 && gfx_rect_contains(delete_rect, e->x, e->y))
            pressed = P_DELETE;
        else {
            bool dirty = false;
            ui_list_event(&list, e, &dirty);
            if (editing && list.pressed >= 0 && e->x < MARGIN + UI_DELETE_BADGE_W)
                pressed = P_BADGE;
        }
        return;
    }

    if (pressed != P_NONE && pressed != P_BADGE) {
        if (e->type == GZ_EV_TOUCH_UP) {
            gfx_rect r = pressed == P_BACK ? back_rect : pressed == P_EDIT ? edit_rect
                       : pressed == P_NEWFOLDER ? newfolder_rect : delete_rect;
            int what = pressed;
            pressed = P_NONE;
            if (!gfx_rect_contains(r, e->x, e->y))
                return;
            if (what == P_BACK) {
                go_up();
            } else if (what == P_EDIT) {
                editing = !editing;
                confirm_row = -1;
                ui_list_set_area(&list, list_area());
            } else if (what == P_NEWFOLDER) {
                start_naming(true, -1);
            } else if (what == P_DELETE) {
                int row = confirm_row;
                confirm_row = -1;
                request_delete(row);
            }
        }
        return;
    }

    bool dirty = false;
    int row = ui_list_event(&list, e, &dirty);
    if (e->type != GZ_EV_TOUCH_UP)
        return;
    bool badge = pressed == P_BADGE;
    pressed = P_NONE;
    if (row < 0)
        return;
    if (confirm_row >= 0) {
        confirm_row = -1;       /* any other tap cancels a pending delete */
    } else if (editing && is_card(row)) {
        /* the card's volume stays as it is */
    } else if (editing && badge) {
        confirm_row = row;
    } else if (editing) {
        start_naming(false, row);
    } else {
        open_entry(row);
    }
}

static void name_input(const struct gz_input_event *e)
{
    int ch = ui_input_char(e);
    if (ch == '\n') {
        if (name_valid()) finish_naming(true);
    } else if (ch != '/') {
        ui_edit_event(name_buf, sizeof(name_buf), &name_cursor, e, UI_EDIT_FILENAME);
        if (ch >= ' ') kb.shift = false;
    }
}

static void name_event(const struct gz_input_event *e)
{
    int kb_y = UI_H - UI_KB_H;
    if (!ui_has_hardware_keyboard() && e->y >= kb_y && pressed == P_NONE) {
        bool changed;
        int ch = ui_keyboard_event(&kb, e, kb_y, &changed);
        struct gz_input_event key = { .type = GZ_EV_TEXT, .code = (uint16_t)ch };
        if (ch == '\b' || ch == '\n') {
            key.type = GZ_EV_KEY_DOWN;
            key.code = ch == '\b' ? GZ_KEY_BACKSPACE : GZ_KEY_ENTER;
        }
        if (ch) name_input(&key);
        return;
    }
    if (e->type == GZ_EV_TOUCH_DOWN) {
        pressed = gfx_rect_contains(cancel_rect, e->x, e->y) ? P_CANCEL
                : gfx_rect_contains(save_rect, e->x, e->y) && name_valid() ? P_SAVE : P_NONE;
    } else if (e->type == GZ_EV_TOUCH_UP) {
        int what = pressed;
        pressed = P_NONE;
        if (what == P_CANCEL && gfx_rect_contains(cancel_rect, e->x, e->y))
            finish_naming(false);
        else if (what == P_SAVE && gfx_rect_contains(save_rect, e->x, e->y))
            finish_naming(true);
    }
}

static void viewer_event(const struct gz_input_event *e)
{
    if (e->type == GZ_EV_TOUCH_DOWN && gfx_rect_contains(back_rect, e->x, e->y)) {
        pressed = P_BACK;
        return;
    }
    if (view == V_PACKAGE && !pkg_busy) {
        if (e->type == GZ_EV_TOUCH_DOWN) {
            pressed = gfx_rect_contains(flash_rect, e->x, e->y) ? P_FLASH
                    : gfx_rect_contains(card_rect, e->x, e->y) ? P_CARD : P_NONE;
            return;
        }
        if (pressed == P_FLASH || pressed == P_CARD) {
            if (e->type == GZ_EV_TOUCH_UP) {
                bool card = pressed == P_CARD;
                pressed = P_NONE;
                if (gfx_rect_contains(card ? card_rect : flash_rect, e->x, e->y))
                    install_package(card);
            }
            return;
        }
    }
    if (pressed == P_BACK) {
        if (e->type == GZ_EV_TOUCH_UP) {
            pressed = P_NONE;
            if (gfx_rect_contains(back_rect, e->x, e->y))
                go(V_LIST, false);
        }
        return;
    }
    if (view == V_TEXT && e->y >= UI_NAV_H)
        ui_scroll_event(&text_scroll, e, e->y - UI_NAV_H);
}

static void back_key(void)
{
    if (alert_open) {
        alert_open = false;
    } else if (view == V_NAME) {
        finish_naming(false);
    } else if (view != V_LIST) {
        go(V_LIST, false);
    } else if (confirm_row >= 0) {
        confirm_row = -1;
    } else if (editing) {
        editing = false;
        ui_list_set_area(&list, list_area());
    } else if (strcmp(cwd, "/")) {
        go_up();
    }
}

int main(void)
{
    ui_init(SB_STATUS_GRAY);
    ui_list_init(&list, list_area(), ROW_H);
    list.footer_h = FOOTER_H;
    load_dir();
    redraw();

    for (;;) {
        struct ui_scroll *sc = view == V_TEXT ? &text_scroll : &list.sc;
        struct gz_input_event ev;

        if (ui_next_event(&ev, sc->animating ? 30 : -1)) {
            if (ev.type == GZ_EV_KEY_DOWN && ev.code == GZ_KEY_ESC)
                back_key();
            else if (ev.type == GZ_EV_KEY_DOWN || ev.type == GZ_EV_KEY_UP || ev.type == GZ_EV_TEXT) {
                if (!alert_open && view == V_NAME) name_input(&ev);
            } else if (ev.type == GZ_EV_APP_ACTIVATE)
                ;
            else if (ev.type == GZ_EV_TOUCH_CANCEL) {
                /* Home mid-touch: drop the touch without acting on it. */
                bool changed;
                pressed = P_NONE;
                alert_pressed = -1;
                ui_list_event(&list, &ev, &changed);
                ui_scroll_event(&text_scroll, &ev, 0);
                ui_keyboard_event(&kb, &ev, 0, &changed);
            } else if (alert_open)
                alert_event(&ev);
            else if (view == V_LIST)
                list_event(&ev);
            else if (view == V_NAME)
                name_event(&ev);
            else
                viewer_event(&ev);
            redraw();
            continue;
        }
        if (ui_scroll_step(sc))
            redraw();
    }
}
