/* Hard icons: touch.dll's zones, the sb.icons text and SpringBoard's strokes. */
#include "../../servers/springboard/hardicon.h"
#include <glaze/prefs.h>
#include <glaze/sb.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char disk[512];
static int disk_len = GZ_ENOENT;

int pref_get(const char *key, void *buf, uint32_t len)
{
    assert(!strcmp(key, HARDICON_PREF));
    if (disk_len > 0)
        memcpy(buf, disk, len < (uint32_t)disk_len ? len : (uint32_t)disk_len);
    return disk_len;
}

int pref_put(const char *key, const void *buf, uint32_t len)
{
    assert(!strcmp(key, HARDICON_PREF) && len < sizeof(disk));
    memcpy(disk, buf, len);
    disk_len = (int)len;
    return 0;
}

static bool same(const struct hardicon_config *a, const struct hardicon_config *b)
{
    for (int i = 0; i < HARDICON_COUNT; i++) {
        if (a->icon[i].kind != b->icon[i].kind || a->icon[i].key != b->icon[i].key ||
            strcmp(a->icon[i].target, b->icon[i].target))
            return false;
    }
    return a->apps == b->apps;
}

static void zones(void)
{
    /* touch.dll's upper bounds in quarter pixels: 137, 274, 411, 548, 685, 822, 959. */
    static const int first[HARDICON_COUNT] = { 0, 34, 69, 103, 137, 171, 206 };
    for (int x = 0; x < GZ_FB_WIDTH; x++) {
        int z = hardicon_zone(x), want = 0;
        while (want + 1 < HARDICON_COUNT && x >= first[want + 1])
            want++;
        assert(z == want);
    }
    assert(hardicon_zone(-5) == 0 && hardicon_zone(400) == HARDICON_COUNT - 1);
    assert(!strcmp(hardicon_label(0), "Calendar") && !strcmp(hardicon_label(6), "Control Panel"));
    assert(!strcmp(hardicon_label(7), ""));
}

static void prefs(void)
{
    struct hardicon_config c, d;
    char buf[512];

    hardicon_load(&c);              /* nothing saved: Windows CE's assignments */
    hardicon_defaults(&d);
    assert(same(&c, &d));
    assert(c.icon[0].kind == HARDICON_APP && !strcmp(c.icon[0].target, "bin/calendar"));
    assert(c.icon[1].kind == HARDICON_NONE && c.icon[2].kind == HARDICON_NONE);
    assert(c.icon[4].kind == HARDICON_PACKAGE && !strcmp(c.icon[4].target, "gmail"));
    assert(c.apps);
    hardicon_format(&c, buf, sizeof(buf));
    assert(!strcmp(buf, "app bin/calendar\nnone\nnone\napp bin/notes\npkg gmail\napp bin/browser\n"
                        "app bin/settings\napps 1\n"));

    c.icon[1] = (struct hardicon_action){ .kind = HARDICON_HOME };
    c.icon[2] = (struct hardicon_action){ .kind = HARDICON_KEY, .key = GZ_KEY_PAGEDOWN };
    c.icon[3] = (struct hardicon_action){ .kind = HARDICON_SWITCHER };
    c.icon[5] = (struct hardicon_action){ .kind = HARDICON_SCREEN_OFF };
    c.icon[6] = (struct hardicon_action){ .kind = HARDICON_SLEEP };
    c.icon[0] = (struct hardicon_action){ .kind = HARDICON_LOCK };
    c.apps = false;
    assert(hardicon_save(&c) == 0);
    hardicon_load(&d);
    assert(same(&c, &d));

    /* Each bad line keeps its icon's default; CRLF and a short file are fine. */
    static const char bad[] = "app\r\nkey f13\nhome extra\nbogus\npkg has space\napp bin/browser\r\n";
    hardicon_parse(&d, bad, (int)strlen(bad));
    hardicon_defaults(&c);
    c.icon[5] = (struct hardicon_action){ .kind = HARDICON_APP };
    strcpy(c.icon[5].target, "bin/browser");
    assert(same(&c, &d));
    char longname[80];
    snprintf(longname, sizeof(longname), "pkg %0*d\napps 0\n", HARDICON_TARGET, 7);
    hardicon_parse(&d, longname, (int)strlen(longname));
    assert(d.icon[0].kind == HARDICON_APP && d.apps);   /* "apps" only as line 8 */
    hardicon_parse(&d, "", 0);
    hardicon_defaults(&c);
    assert(same(&c, &d));

    for (int i = 0; i < hardicon_num_keys; i++) {
        const char *n = hardicon_key_name(hardicon_keys[i]);
        assert(n && hardicon_key_code(n, (int)strlen(n)) == hardicon_keys[i]);
    }
    assert(!hardicon_key_name(GZ_KEY_HOME) && !hardicon_key_code("home", 4));
    assert(GZ_KEY_ICON7 == GZ_KEY_ICON1 + 6 && GZ_KEY_LAST == GZ_KEY_ICON7 && GZ_KEY_LAST < 32);
}

static uint32_t clock_ms = 1000;

/* Each event comes a PIU scan (45 ms) after the last; a DOWN starts a new
 * stroke a while later. */
static bool feed(struct sb_hardicon *s, uint16_t type, int x, int y, int *fire)
{
    clock_ms += type == GZ_EV_TOUCH_DOWN ? 500 : 45;
    struct gz_input_event e = { .type = type, .x = (int16_t)x, .y = (int16_t)y, .time_ms = clock_ms };
    return sb_hardicon_input(s, &e, x, y, fire);
}

static bool feed_at(struct sb_hardicon *s, uint16_t type, int x, int y, uint32_t ms, int *fire)
{
    struct gz_input_event e = { .type = type, .x = (int16_t)x, .y = (int16_t)y, .time_ms = ms };
    return sb_hardicon_input(s, &e, x, y, fire);
}

static void strokes(void)
{
    struct sb_hardicon s = { 0 };
    int fire;

    /* A tap on icon 4 fires on release, and nothing reaches the app. */
    assert(feed(&s, GZ_EV_TOUCH_DOWN, 120, 340, &fire) && fire < 0);
    assert(feed(&s, GZ_EV_TOUCH_MOVE, 122, 345, &fire) && fire < 0);
    assert(feed(&s, GZ_EV_TOUCH_UP, 122, 345, &fire) && fire == 3);
    /* Released over another icon, or up on the LCD: kept, not fired. */
    assert(feed(&s, GZ_EV_TOUCH_DOWN, 10, 330, &fire));
    assert(feed(&s, GZ_EV_TOUCH_UP, 60, 330, &fire) && fire < 0);
    assert(feed(&s, GZ_EV_TOUCH_DOWN, 10, 330, &fire));
    assert(feed(&s, GZ_EV_TOUCH_MOVE, 10, 200, &fire));
    assert(feed(&s, GZ_EV_TOUCH_UP, 10, 200, &fire) && fire < 0);
    /* Within the slop above the strip still counts; edges clamp. */
    assert(feed(&s, GZ_EV_TOUCH_DOWN, 239, 359, &fire));
    assert(feed(&s, GZ_EV_TOUCH_UP, 250, HARDICON_TOP - HARDICON_SLOP, &fire) && fire == 6);
    assert(feed(&s, GZ_EV_TOUCH_DOWN, -3, 330, &fire));
    assert(feed(&s, GZ_EV_TOUCH_UP, 0, HARDICON_TOP - HARDICON_SLOP - 1, &fire) && fire < 0);
    /* A stroke from the LCD into the strip stays with its owner. */
    assert(!feed(&s, GZ_EV_TOUCH_DOWN, 100, 300, &fire));
    assert(!feed(&s, GZ_EV_TOUCH_MOVE, 100, 340, &fire));
    assert(!feed(&s, GZ_EV_TOUCH_UP, 100, 340, &fire) && fire < 0);
    /* A cancel ends a strip stroke without firing. */
    assert(feed(&s, GZ_EV_TOUCH_DOWN, 50, 340, &fire));
    assert(!feed(&s, GZ_EV_TOUCH_CANCEL, 50, 340, &fire) && !s.active);
    assert(!feed(&s, GZ_EV_TOUCH_UP, 50, 340, &fire) && fire < 0);
    /* One noisy sample: DOWN and UP at the same moment never fire. */
    assert(feed_at(&s, GZ_EV_TOUCH_DOWN, 120, 340, 5000, &fire));
    assert(feed_at(&s, GZ_EV_TOUCH_UP, 120, 340, 5000, &fire) && fire < 0 && !strcmp(s.reject, "short"));
    assert(feed_at(&s, GZ_EV_TOUCH_DOWN, 120, 340, 6000, &fire));
    assert(feed_at(&s, GZ_EV_TOUCH_UP, 120, 340, 6000 + HARDICON_MIN_MS - 1, &fire) && fire < 0);
    assert(feed_at(&s, GZ_EV_TOUCH_DOWN, 120, 340, 7000, &fire));
    assert(feed_at(&s, GZ_EV_TOUCH_UP, 120, 340, 7000 + HARDICON_MIN_MS, &fire) && fire == 3 && !s.reject);
    /* Pressed just below the LCD: kept from the app, but never fired. */
    assert(feed(&s, GZ_EV_TOUCH_DOWN, 120, HARDICON_TOP, &fire));
    assert(feed(&s, GZ_EV_TOUCH_MOVE, 120, HARDICON_TOP + 10, &fire));
    assert(feed(&s, GZ_EV_TOUCH_UP, 120, HARDICON_TOP + 10, &fire) && fire < 0 && !strcmp(s.reject, "edge"));
    assert(feed(&s, GZ_EV_TOUCH_DOWN, 120, HARDICON_TOP + HARDICON_MARGIN, &fire));
    assert(feed(&s, GZ_EV_TOUCH_UP, 120, HARDICON_TOP + HARDICON_MARGIN, &fire) && fire == 3);
    /* A stroke that leaves its icon and comes back doesn't fire. */
    assert(feed(&s, GZ_EV_TOUCH_DOWN, 120, 340, &fire));
    assert(feed(&s, GZ_EV_TOUCH_MOVE, 160, 340, &fire));
    assert(feed(&s, GZ_EV_TOUCH_MOVE, 120, 340, &fire));
    assert(feed(&s, GZ_EV_TOUCH_UP, 120, 340, &fire) && fire < 0 && !strcmp(s.reject, "moved"));
    assert(feed(&s, GZ_EV_TOUCH_DOWN, 120, 340, &fire));
    assert(feed(&s, GZ_EV_TOUCH_MOVE, 120, 200, &fire));
    assert(feed(&s, GZ_EV_TOUCH_MOVE, 120, 340, &fire));
    assert(feed(&s, GZ_EV_TOUCH_UP, 120, 340, &fire) && fire < 0);
    /* Moving within its icon and the slop above is fine. */
    assert(feed(&s, GZ_EV_TOUCH_DOWN, 110, 350, &fire));
    assert(feed(&s, GZ_EV_TOUCH_MOVE, 130, HARDICON_TOP - HARDICON_SLOP, &fire));
    assert(feed(&s, GZ_EV_TOUCH_UP, 125, 345, &fire) && fire == 3);
    /* Keys pass through. */
    assert(!feed(&s, GZ_EV_KEY_DOWN, 0, 0, &fire));
}

int main(void)
{
    zones();
    prefs();
    strokes();
    puts("hardicon: zones, preferences and strokes PASS");
    return 0;
}
