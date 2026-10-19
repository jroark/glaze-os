/* SpringBoard internals shared by the device loop and the renderer. */
#ifndef SB_INTERNAL_H
#define SB_INTERNAL_H

#include <glaze/gfx.h>
#include <glaze/sb.h>

/* Layout (240 x 320 screen). */
#define SCREEN_W        240
#define SCREEN_H        320
#define GRID_COLS       4
#define GRID_ROWS       3
#define PER_PAGE        (GRID_COLS * GRID_ROWS)
#define ICON_SIZE       44
#define CELL_W          60
#define GRID_TOP        26
#define ROW_PITCH       72
#define DOCK_Y          258
#define DOCK_SLOTS      4
#define PAGE_DOTS_Y     246
#define LOCK_TOP_H      82          /* below the status bar */
#define LOCK_BOTTOM_Y   238
#define TRACK_X         15
#define TRACK_Y         260
#define TRACK_W         210
#define TRACK_H         40
#define KNOB_W          50
#define KNOB_H          36
#define KNOB_TRAVEL     (TRACK_W - KNOB_W - 4)

struct sb_app_info {
    const char *label;
    const char *icon;           /* res name */
    const char *path;           /* romfs binary */
    int status_style;
    bool dock;
};

extern const struct sb_app_info sb_apps[];
extern const int sb_num_apps;

struct sb_clock {
    char status[12];            /* "9:41 AM" */
    char big[8];                /* "9:41" */
    char date[40];              /* "Thursday, January 1" */
    char weekday[12];
    int day;
};

struct sb_view {
    struct sb_clock clk;
    int wallpaper;              /* index into sb_wallpapers */
    int knob_x;                 /* 0..KNOB_TRAVEL */
    int shimmer_x;              /* centre of the slide-to-unlock highlight */
    int scroll_x;               /* home pages scroll position in pixels */
    int pressed;                /* app index under the finger, or -1 */
};

extern const char *const sb_wallpapers[];
extern const int sb_num_wallpapers;

void sb_draw_init(void);
void sb_clock_from(struct sb_clock *c, const struct gz_datetime *dt);
void sb_draw_status(gfx_surface *s, const struct sb_clock *c, int style, bool padlock);
void sb_draw_wallpaper(gfx_surface *s, const struct sb_view *v, gfx_rect r);
/* Lock UI: top and bottom bars shifted apart by `open` pixels. */
void sb_draw_lock(gfx_surface *s, const struct sb_view *v, int open);
void sb_draw_lock_track(gfx_surface *s, const struct sb_view *v);
void sb_draw_home(gfx_surface *s, const struct sb_view *v);
int sb_num_pages(void);
gfx_rect sb_icon_rect(int app, int scroll_x);
int sb_hit_icon(int x, int y, int scroll_x);

#endif
