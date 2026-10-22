/*
 * Photos: thumbnail grid and a full-screen viewer with swipe paging.
 * Tapping the viewer toggles the translucent bars, as in the original.
 *
 * The Camera Roll is the GZI pictures in /Pictures. Thumbnails are cached
 * in THUMB_DIR and filled in one at a time while the grid is up; full
 * pictures are read on demand into a few slots kept for the current photo
 * and its neighbours.
 */

#include <glaze/fs.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PICTURES    "/Pictures"
#define THUMB_DIR   "/Library/Caches/Photos"
#define MAX_PHOTOS  64
#define IMAGE_MAX   (256 * 1024)
#define NSLOTS      3
#define THUMB       56
#define GAP         4
#define COLS        4
#define PAGE_MS     260
#define THUMB_MAGIC 0x31544D47u     /* "GMT1" */

struct photo {
    char name[FS_NAME_MAX];
    uint32_t size;              /* of the picture file, to spot stale thumbnails */
    gfx_surface thumb;          /* px is NULL until loaded */
    bool tried;
};

/* Thumbnail cache file: this header, then THUMB x THUMB RGB565. */
struct thumb_hdr {
    uint32_t magic;
    uint32_t src_size;
};

/* A full picture read from flash. */
struct slot {
    int index;                  /* -1 when free */
    uint8_t *buf;
    gfx_image img;
    bool ok;
};

static struct photo photos[MAX_PHOTOS];
static int nphotos;
static struct slot slots[NSLOTS];

enum view { V_GRID, V_VIEWER };
static enum view view = V_GRID;
static int current;
static int scroll_x;            /* viewer pan offset during swipes */
static bool bars = true;
static gfx_rect back_rect;
static int pressed_thumb = -1;
static struct ui_scroll grid_scroll;

/* Viewer touch tracking. */
static bool touching, panning;
static int down_x, last_x, velocity;
static uint32_t last_ms;

/* ---- storage ------------------------------------------------------------ */

static bool is_picture(const struct fs_dirent *e)
{
    const char *dot = strrchr(e->name, '.');

    return e->type == FS_FILE && dot && !strcasecmp(dot, ".gzi");
}

static int photo_cmp(const void *pa, const void *pb)
{
    return strcasecmp(((const struct photo *)pa)->name, ((const struct photo *)pb)->name);
}

static void list_pictures(void)
{
    struct fs_dirent ents[8];
    uint32_t skip = 0;
    int n;

    while (nphotos < MAX_PHOTOS && (n = fs_list(PICTURES, skip, ents, 8)) > 0) {
        for (int i = 0; i < n && nphotos < MAX_PHOTOS; i++) {
            if (!is_picture(&ents[i]))
                continue;
            struct photo *p = &photos[nphotos++];
            memcpy(p->name, ents[i].name, sizeof(p->name));
            p->size = ents[i].size;
        }
        skip += (uint32_t)n;
    }
    qsort(photos, (size_t)nphotos, sizeof(photos[0]), photo_cmp);
}

static void thumb_path(int i, char *buf, size_t n)
{
    snprintf(buf, n, THUMB_DIR "/%s.thm", photos[i].name);
}

/* Reads picture i into a slot, reusing one that holds no neighbour of the
 * current photo. */
static struct slot *load_slot(int i)
{
    struct slot *s = NULL;
    char path[FS_PATH_MAX];

    for (int k = 0; k < NSLOTS; k++) {
        if (slots[k].index == i)
            return &slots[k];
    }
    for (int k = 0; k < NSLOTS && !s; k++) {
        if (slots[k].index < 0 || abs(slots[k].index - current) > 1)
            s = &slots[k];
    }
    if (!s)
        s = &slots[0];
    if (!s->buf)
        s->buf = malloc(IMAGE_MAX);
    s->index = i;
    snprintf(path, sizeof(path), PICTURES "/%s", photos[i].name);
    int n = s->buf ? fs_read_file(path, s->buf, IMAGE_MAX) : GZ_ENOMEM;
    s->ok = n > 0 && gfx_image_parse(&s->img, s->buf, (size_t)n);
    return s;
}

static const gfx_image *image_for(int i)
{
    struct slot *s = load_slot(i);

    return s->ok ? &s->img : NULL;
}

/* Centre square crop, scaled down. */
static void make_thumb(int i, uint16_t *px)
{
    const gfx_image *img = image_for(i);
    gfx_surface src, t;

    gfx_surface_init(&t, px, THUMB, THUMB, THUMB);
    if (!img) {
        gfx_fill(&t, gfx_r(0, 0, THUMB, THUMB), 0xCCCCCC);
        return;
    }
    gfx_surface_init(&src, (uint16_t *)img->px, img->w, img->h, img->w);
    int side = MIN(img->w, img->h);
    gfx_blit_scaled(&t, gfx_r(0, 0, THUMB, THUMB), &src,
                    gfx_r((img->w - side) / 2, (img->h - side) / 2, side, side), 255);
}

/* Loads thumbnail i from the cache, or makes it and caches it. */
static void load_thumb(int i)
{
    static uint8_t file[sizeof(struct thumb_hdr) + THUMB * THUMB * 2];
    struct thumb_hdr *h = (struct thumb_hdr *)file;
    uint16_t *px = malloc(THUMB * THUMB * 2);
    char path[FS_PATH_MAX];

    photos[i].tried = true;
    if (!px)
        return;
    thumb_path(i, path, sizeof(path));
    if (fs_read_file(path, file, sizeof(file)) == (int)sizeof(file) && h->magic == THUMB_MAGIC &&
        h->src_size == photos[i].size) {
        memcpy(px, file + sizeof(*h), THUMB * THUMB * 2);
    } else {
        make_thumb(i, px);
        h->magic = THUMB_MAGIC;
        h->src_size = photos[i].size;
        memcpy(file + sizeof(*h), px, THUMB * THUMB * 2);
        if (fs_write_file(path, file, sizeof(file)) == GZ_ENOENT && fs_mkdirs(THUMB_DIR) == 0)
            fs_write_file(path, file, sizeof(file));
    }
    gfx_surface_init(&photos[i].thumb, px, THUMB, THUMB, THUMB);
}

/* Drops cached thumbnails of pictures that are gone. */
static void prune_thumbs(void)
{
    struct fs_dirent ents[8];
    uint32_t skip = 0;
    int n;

    while ((n = fs_list(THUMB_DIR, skip, ents, 8)) > 0) {
        for (int i = 0; i < n; i++) {
            size_t len = strlen(ents[i].name);
            bool keep = false;
            for (int k = 0; k < nphotos && !keep; k++)
                keep = len == strlen(photos[k].name) + 4 && !strncmp(ents[i].name, photos[k].name, len - 4);
            if (keep) {
                skip++;
            } else {
                char path[FS_PATH_MAX];
                snprintf(path, sizeof(path), THUMB_DIR "/%s", ents[i].name);
                if (fs_remove(path) < 0)
                    skip++;
            }
        }
        if (n < 8)
            break;
    }
}

/* The next thumbnail still to load, or -1. Visible ones go first. */
static int next_thumb(void)
{
    int first = (grid_scroll.offset / (THUMB + GAP)) * COLS;

    for (int k = 0; k < nphotos; k++) {
        int i = (first + k) % nphotos;
        if (!photos[i].tried)
            return i;
    }
    return -1;
}

/* ---- drawing ------------------------------------------------------------ */

static gfx_rect thumb_rect(int i)
{
    return gfx_r(GAP + (i % COLS) * (THUMB + GAP),
                 UI_NAV_H + GAP + (i / COLS) * (THUMB + GAP) - grid_scroll.offset, THUMB, THUMB);
}

static int grid_content_h(void)
{
    return GAP + (nphotos + COLS - 1) / COLS * (THUMB + GAP) + 32;
}

static void draw_grid(gfx_surface *s)
{
    const gfx_font *f = ui_font(FONT_R14);

    gfx_fill(s, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H), 0xFFFFFF);
    for (int i = 0; i < nphotos; i++) {
        gfx_rect r = thumb_rect(i);
        if (r.y + THUMB <= UI_NAV_H || r.y >= UI_H)
            continue;
        if (photos[i].thumb.px)
            gfx_blit(s, r.x, r.y, &photos[i].thumb, gfx_r(0, 0, THUMB, THUMB));
        else
            gfx_fill(s, r, 0xE4E4E4);
        if (i == pressed_thumb)
            gfx_fill_alpha(s, r, 0x000000, 100);
    }
    if (nphotos) {
        char count[24];
        snprintf(count, sizeof(count), nphotos == 1 ? "%d Photo" : "%d Photos", nphotos);
        gfx_text_center(s, 0, thumb_rect(nphotos - 1).y + THUMB + 12, UI_W, f, count, 0x666666, 255);
    } else {
        gfx_text_center(s, 0, UI_NAV_H + 100, UI_W, ui_font(FONT_B17), "No Photos", 0x666666, 255);
        gfx_text_center(s, 0, UI_NAV_H + 126, UI_W, f, "Pictures you add to " PICTURES, 0x888888, 255);
        gfx_text_center(s, 0, UI_NAV_H + 144, UI_W, f, "appear here.", 0x888888, 255);
    }
    ui_draw_navbar(s, "Camera Roll", NULL, NULL, 1);
}

/* The photo fills the whole screen behind the status bar, so it is drawn
 * shifted up by the status bar height. */
static void draw_photo(gfx_surface *s, int i, int x)
{
    const gfx_image *img = image_for(i);
    gfx_surface sh = *s;

    sh.px -= SB_STATUS_H * s->stride;
    sh.clip = gfx_r(s->clip.x, s->clip.y + SB_STATUS_H, s->clip.w, s->clip.h);
    if (img)
        gfx_draw_image(&sh, x, 0, img, 255);
    else
        gfx_text_center(s, x, UI_H / 2 - 8, UI_W, ui_font(FONT_R14), "Can't open this picture", 0x999999, 255);
}

static void draw_viewer(gfx_surface *s)
{
    gfx_fill(s, gfx_r(0, 0, UI_W, UI_H), 0x000000);
    draw_photo(s, current, -scroll_x);
    if (scroll_x > 0 && current + 1 < nphotos)
        draw_photo(s, current + 1, UI_W - scroll_x + 8);
    if (scroll_x < 0 && current > 0)
        draw_photo(s, current - 1, -UI_W - scroll_x - 8);
    if (bars) {
        char title[24];
        snprintf(title, sizeof(title), "%d of %d", current + 1, nphotos);
        /* Translucent bar: render it aside, then blend over the photo. */
        static uint16_t barbuf[UI_W * UI_NAV_H];
        gfx_surface bar;
        gfx_surface_init(&bar, barbuf, UI_W, UI_NAV_H, UI_W);
        ui_draw_navbar(&bar, title, "Camera Roll", &back_rect, 1);
        gfx_blit_alpha(s, 0, 0, &bar, gfx_r(0, 0, UI_W, UI_NAV_H), 200);
        gfx_rect tb = gfx_r(0, UI_H - 34, UI_W, 34);
        gfx_fill_alpha(s, tb, 0x000000, 140);
        gfx_vgradient(s, gfx_r(0, tb.y, UI_W, 17), 0xFFFFFF, 0xFFFFFF, 20);
        /* Previous / next arrows. */
        const gfx_font *f = ui_font(FONT_B20);
        gfx_text_center(s, 40, tb.y + 4, 40, f, GFX_CH_LARROW, current > 0 ? 0xFFFFFF : 0x666666, 255);
        gfx_text_center(s, UI_W - 80, tb.y + 4, 40, f, GFX_CH_RARROW, current + 1 < nphotos ? 0xFFFFFF : 0x666666, 255);
    }
}

static void draw_current(gfx_surface *s)
{
    if (view == V_GRID)
        draw_grid(s);
    else
        draw_viewer(s);
}

static void redraw(void)
{
    draw_current(&ui.surf);
    ui_invalidate_all();
    ui_present();
}

static void animate_to(int target_scroll, int next_index)
{
    int from = scroll_x;
    uint32_t t0 = gz_time_ms();

    for (;;) {
        int p = ui_ease_out(gz_time_ms() - t0, PAGE_MS);
        scroll_x = from + (target_scroll - from) * p / 1024;
        redraw();
        if (p >= 1024)
            break;
        gz_sleep_ms(10);
    }
    current = next_index;
    scroll_x = 0;
    redraw();
}

static void go_neighbour(int dir)
{
    int next = current + dir;

    if (next < 0 || next >= nphotos) {
        animate_to(0, current);
        return;
    }
    animate_to(dir * (UI_W + 8), next);
}

static void viewer_event(const struct gz_input_event *e)
{
    uint32_t t = gz_time_ms();

    switch (e->type) {
    case GZ_EV_TOUCH_DOWN:
        if (bars && gfx_rect_contains(back_rect, e->x, e->y) && e->y < UI_NAV_H) {
            view = V_GRID;
            ui_transition(draw_current, false);
            return;
        }
        if (bars && e->y >= UI_H - 34) {
            if (e->x < UI_W / 2)
                go_neighbour(-1);
            else
                go_neighbour(1);
            return;
        }
        touching = true;
        panning = false;
        down_x = last_x = e->x;
        last_ms = t;
        velocity = 0;
        break;
    case GZ_EV_TOUCH_MOVE:
        if (!touching)
            break;
        if (abs(e->x - down_x) > 8)
            panning = true;
        if (t > last_ms)
            velocity = (e->x - last_x) * 1000 / (int)(t - last_ms);
        last_x = e->x;
        last_ms = t;
        if (panning) {
            scroll_x = down_x - e->x;
            if ((scroll_x < 0 && current == 0) || (scroll_x > 0 && current + 1 == nphotos))
                scroll_x /= 3;          /* resist past the ends */
            redraw();
        }
        break;
    case GZ_EV_TOUCH_UP:
        if (!touching)
            break;
        touching = false;
        if (!panning) {
            bars = !bars;
            redraw();
        } else if (scroll_x > UI_W / 3 || velocity < -400) {
            go_neighbour(1);
        } else if (scroll_x < -UI_W / 3 || velocity > 400) {
            go_neighbour(-1);
        } else {
            animate_to(0, current);
        }
        break;
    }
}

static int thumb_hit(int x, int y)
{
    if (y < UI_NAV_H)
        return -1;
    for (int i = 0; i < nphotos; i++) {
        if (gfx_rect_contains(thumb_rect(i), x, y))
            return i;
    }
    return -1;
}

static void grid_event(const struct gz_input_event *e)
{
    if (e->type == GZ_EV_TOUCH_DOWN) {
        if (e->y < UI_NAV_H)
            return;
        ui_scroll_event(&grid_scroll, e, e->y - UI_NAV_H);
        pressed_thumb = thumb_hit(e->x, e->y);
        redraw();
    } else if (e->type == GZ_EV_TOUCH_MOVE) {
        if (ui_scroll_event(&grid_scroll, e, e->y - UI_NAV_H)) {
            if (grid_scroll.moved)
                pressed_thumb = -1;
            redraw();
        }
    } else if (e->type == GZ_EV_TOUCH_UP) {
        ui_scroll_event(&grid_scroll, e, e->y - UI_NAV_H);
        int i = pressed_thumb;
        pressed_thumb = -1;
        if (i >= 0 && !grid_scroll.moved && thumb_hit(e->x, e->y) == i) {
            current = i;
            scroll_x = 0;
            bars = true;
            view = V_VIEWER;
            ui_transition(draw_current, true);
        } else {
            redraw();
        }
    }
}

static bool in_slot(int i)
{
    for (int k = 0; k < NSLOTS; k++) {
        if (slots[k].index == i)
            return true;
    }
    return false;
}

/* Background work between events: thumbnails for the grid, the current
 * photo's neighbours for the viewer. Returns false when there is none. */
static bool idle_work(void)
{
    if (view == V_GRID) {
        int i = next_thumb();
        if (i < 0)
            return false;
        load_thumb(i);
        redraw();
        return true;
    }
    for (int d = 1; d >= -1; d -= 2) {
        int i = current + d;
        if (i >= 0 && i < nphotos && !in_slot(i)) {
            load_slot(i);
            return true;
        }
    }
    return false;
}

int main(void)
{
    ui_init(SB_STATUS_BLACK);
    for (int k = 0; k < NSLOTS; k++)
        slots[k].index = -1;
    list_pictures();
    prune_thumbs();
    ui_scroll_reset(&grid_scroll, grid_content_h(), UI_H - UI_NAV_H);
    redraw();

    for (;;) {
        struct gz_input_event ev;
        bool busy = (view == V_GRID && next_thumb() >= 0) ||
                    (view == V_VIEWER && ((current > 0 && !in_slot(current - 1)) ||
                                          (current + 1 < nphotos && !in_slot(current + 1))));
        int wait = busy ? 0 : view == V_GRID && grid_scroll.animating ? 30 : -1;

        if (!ui_next_event(&ev, wait)) {
            if (view == V_GRID && ui_scroll_step(&grid_scroll))
                redraw();
            else
                idle_work();
            continue;
        }
        if (ev.type == GZ_EV_TOUCH_CANCEL) {
            /* Home mid-touch: drop the touch without acting on it. */
            pressed_thumb = -1;
            ui_scroll_event(&grid_scroll, &ev, 0);
            if (touching && panning)
                animate_to(0, current);     /* back to the photo it started on */
            touching = false;
            redraw();
        } else if (view == V_GRID)
            grid_event(&ev);
        else if (ev.type == GZ_EV_KEY_DOWN && (ev.code == GZ_KEY_LEFT || ev.code == GZ_KEY_RIGHT))
            go_neighbour(ev.code == GZ_KEY_LEFT ? -1 : 1);
        else if (ev.type == GZ_EV_KEY_DOWN && ev.code == GZ_KEY_ESC) {
            view = V_GRID;
            ui_transition(draw_current, false);
        } else
            viewer_event(&ev);
    }
}
