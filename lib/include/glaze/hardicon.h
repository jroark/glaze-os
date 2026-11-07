/*
 * Hard icons: the seven printed touch buttons in the panel rows below the
 * LCD (factory-scaled y 320..359). SpringBoard turns a tap there into the
 * icon's action; Settings → Icon Buttons edits them. docs/os/HARD_ICONS.md.
 */
#ifndef GLAZE_HARDICON_H
#define GLAZE_HARDICON_H

#include <glaze/abi.h>

#define HARDICON_COUNT  7
#define HARDICON_PREF   "sb.icons"
#define HARDICON_TOP    GZ_FB_HEIGHT    /* first panel row of the strip */
#define HARDICON_SLOP   8               /* a release this far above still counts */
#define HARDICON_MARGIN 4               /* a press this close to the LCD never fires */
#define HARDICON_MIN_MS 40              /* about one 45 ms PIU scan: shorter is a phantom */
#define HARDICON_TARGET 48              /* a romfs path or a package id */

enum hardicon_kind {
    HARDICON_NONE,
    HARDICON_HOME,          /* as the Home button */
    HARDICON_SWITCHER,      /* the multitasking bar */
    HARDICON_LOCK,
    HARDICON_SLEEP,         /* Goodbye, then the Settings sleep mode */
    HARDICON_SCREEN_OFF,
    HARDICON_APP,           /* target: a built-in's romfs path, "bin/calendar" */
    HARDICON_PACKAGE,       /* target: an installed package id, "gmail" */
    HARDICON_KEY,           /* key: a GZ_KEY_* for the front app */
    HARDICON_KINDS,
};

struct hardicon_action {
    uint8_t kind;
    uint16_t key;
    char target[HARDICON_TARGET];
};

struct hardicon_config {
    struct hardicon_action icon[HARDICON_COUNT];
    bool apps;              /* an app may claim icons (sb_shared.hard_icons) */
};

/* The icon under panel column x (0..239), from Windows CE's touch.dll. */
int hardicon_zone(int x);
/* Windows CE's printed label for icon n ("Calendar" ... "Control Panel"). */
const char *hardicon_label(int n);
/* Windows CE's assignments, mapped to Glaze's apps where there is one. */
void hardicon_defaults(struct hardicon_config *c);
/* Text form, one line per icon then "apps 0|1"; unparsable lines keep the
 * default for that icon. Returns the length written (without the NUL). */
int hardicon_format(const struct hardicon_config *c, char *buf, int len);
void hardicon_parse(struct hardicon_config *c, const char *text, int len);
void hardicon_load(struct hardicon_config *c);
int hardicon_save(const struct hardicon_config *c);
/* "esc", "pageup", ...; NULL / 0 when unknown. The keys an icon may send. */
const char *hardicon_key_name(uint16_t key);
uint16_t hardicon_key_code(const char *name, int len);
extern const uint16_t hardicon_keys[];
extern const int hardicon_num_keys;

#endif
