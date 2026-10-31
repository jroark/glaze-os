/* SpringBoard internals shared by the device loop and the renderer. */
#ifndef SB_INTERNAL_H
#define SB_INTERNAL_H

#include <glaze/gfx.h>
#include <glaze/sb.h>
#include <glaze/ui.h>

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

/* Built-in apps come first, then installed ones (glaze/pkg.h). An app keeps
 * its index for the session, so badges and notes indexed by it stay put:
 * an app on a removed card is hidden (!present), an uninstalled one leaves
 * an empty slot (path ""). */
struct sb_app_info {
    char label[32];
    char icon[24];              /* res name; installed apps load res/icon.gzi */
    char path[48];              /* romfs binary, or an installed app's bundle */
    char exec[16];              /* installed: the executable in the bundle */
    char id[28];                /* installed: package id (process name) */
    char version[16];           /* installed: package version */
    uint32_t install_id;        /* installed: the bundle's install id at launch */
    int status_style;
    bool dock;
    bool installed;
    bool wince;                /* direct NAND runner; path is writable context */
    bool present;
};

#define SB_MAX_APPS     32
#define SWITCHER_SLOTS  4           /* icons shown in the multitasking bar */
#define SB_MAX_RUNNING  SWITCHER_SLOTS  /* running apps (front + background), all in the bar */
#define SWITCHER_H      74          /* height of the bottom multitasking tray */
#define SB_MAX_NOTES    16          /* scheduled, all apps together */
#define SB_LOCK_NOTES   4           /* shown on the lock screen */
#define LOCK_NOTE_H     32
#define BANNER_H        44          /* below the status bar */

extern struct sb_app_info sb_apps[SB_MAX_APPS];
extern int sb_num_apps;

static inline bool sb_app_shown(int i)
{
    return sb_apps[i].present && sb_apps[i].path[0];
}

struct sb_note {
    int app;                    /* index into sb_apps */
    uint32_t id, when, badge;   /* when: glaze/notify.h seconds */
    char title[32];
    char body[96];
};

struct sb_clock {
    char status[12];            /* "9:41 AM" */
    char big[8];                /* "9:41" */
    char date[40];              /* "Thursday, January 1" */
    char weekday[12];
    int day;
};

struct sb_view {
    struct sb_clock clk;
    bool card_mounted;         /* mounted CF storage or an initialized network card */
    unsigned debugger;        /* GU_DEBUG_*; hidden when disconnected */
    int wallpaper;              /* index into sb_wallpapers */
    int knob_x;                 /* 0..KNOB_TRAVEL */
    int shimmer_x;              /* centre of the slide-to-unlock highlight */
    int scroll_x;               /* home pages scroll position in pixels */
    int pressed;                /* app index under the finger, or -1 */
    const uint16_t *badges;     /* per app, or NULL */
    const struct sb_note *lock_notes;   /* delivered while locked, newest first */
    int num_lock_notes;
    struct ui_passcode pin;     /* passcode entry after the slide */
    bool jiggle;                /* editing the home screen: icons wobble */
    int jiggle_phase;
    bool switcher_open;         /* double-Home multitasking bar open */
    bool switcher_edit;         /* holding switcher icon: '-' badges shown */
    int switcher_apps[SB_MAX_RUNNING]; /* app indices in MRU order */
    int num_switcher_apps;
    int switcher_pressed;       /* switcher slot under finger, or -1 */
    int switcher_minus_pressed; /* switcher '-' badge under finger, or -1 */
    struct gz_power_status power;   /* the kernel's battery status */
    bool battery_pct;           /* show the level as a percentage */
};

extern bool sb_24h;
extern const char *const sb_wallpapers[];
extern const int sb_num_wallpapers;

void sb_draw_init(void);
/* Whether romfs holds program `path` (main.c; true in the host preview). */
bool sb_has_program(const char *path);
/* An installed app's icon (NULL for the generic one); data must stay alive. */
void sb_set_icon(int app, const gfx_image *img);
void sb_clock_from(struct sb_clock *c, const struct gz_datetime *dt);
void sb_draw_status(gfx_surface *s, const struct sb_view *v, int style, bool padlock);
gfx_rect sb_card_icon_rect(const struct sb_view *v);
struct sb_card_details {
    char title[32];
    char lines[8][48];
    int num_lines;
};
/* Draws the card details over the current screen; returns the OK button. */
gfx_rect sb_draw_card_details(gfx_surface *s, const struct sb_card_details *d, bool pressed);
void sb_draw_wallpaper(gfx_surface *s, const struct sb_view *v, gfx_rect r);
/* Lock UI: top and bottom bars shifted apart by `open` pixels. */
void sb_draw_lock(gfx_surface *s, const struct sb_view *v, int open);
void sb_draw_lock_track(gfx_surface *s, const struct sb_view *v);
/* Passcode keypad over the dimmed wallpaper, below the status bar. */
void sb_draw_passcode(gfx_surface *s, const struct sb_view *v);
gfx_rect sb_passcode_area(void);
void sb_draw_home(gfx_surface *s, const struct sb_view *v);
int sb_num_pages(void);
gfx_rect sb_icon_rect(int app, int scroll_x);
int sb_hit_icon(int x, int y, int scroll_x);
/* Jiggle mode: the delete button of an installed app's icon. */
gfx_rect sb_delete_rect(int app, int scroll_x);
/* Multitasking switcher bar at the bottom of the home screen. */
gfx_rect sb_switcher_icon_rect(int slot);
gfx_rect sb_switcher_minus_rect(int slot);
int sb_hit_switcher_icon(int x, int y, int count);
int sb_hit_switcher_minus(int x, int y, int count);
/* Notification banner over the top of the screen, below the status bar. */
void sb_draw_banner(gfx_surface *s, const struct sb_note *n);

/* notify.c: badges, scheduled notes, notes delivered while locked. */
extern uint16_t sb_badges[SB_MAX_APPS];
extern struct sb_note sb_delivered[SB_LOCK_NOTES];
extern int sb_num_delivered;

void notify_load(void);
/* apps.c changed the list: drop an uninstalled app's badge and notes, or
 * pick up saved ones for an app that appeared. */
void notify_forget(int app);
void notify_attach(int app);

/* apps.c: installed apps. */
void apps_scan(bool card_only);     /* updates sb_apps[] */
bool apps_changed(void);            /* since the last call: redraw */
uint32_t apps_reinstalled_mask(void); /* bitmask of slots updated/removed */
int apps_wince(const char *path); /* direct NAND context -> app index/error */
int apps_launch(int app);           /* pid, or an error */
bool apps_replaced(int app);        /* reinstalled since apps_launch */
int apps_uninstall(int app);
int notify_app_for_path(const char *path);
void notify_set_badge(int app, uint32_t count);
void notify_add_badge(int app, uint32_t n);
int notify_post(int app, uint32_t id, uint32_t when, uint32_t badge, const char *title, const char *body);
int notify_cancel(int app, uint32_t id);
uint32_t notify_pending(int app, uint32_t id);
/* ms until the next note is due (capped at an hour), or -1 if none. */
int notify_next_ms(uint32_t now);
/* Removes and returns the earliest note due at `now`. */
bool notify_pop_due(uint32_t now, struct sb_note *out);
void notify_deliver_locked(const struct sb_note *n);
void notify_clear_delivered(void);

/* timesync.c: NTP in a thread of its own. It notifies SpringBoard's port
 * with SB_NOTIFY_CLOCK after setting the clock. */
#define SB_NOTIFY_CLOCK     0x4u
void timesync_start(int port);
void hwdump_start(void);
void timesync_wake(void);           /* SB_NOTIFY_TIME: settings changed */

#endif
