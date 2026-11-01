#ifndef SB_IDLE_H
#define SB_IDLE_H
#include <glaze/abi.h>

struct sb_idle {
    uint32_t last_activity, timeout_ms, keys_down, swallow_keys;
    uint32_t sleep_after_ms;    /* power off this long after the timeout */
    int16_t touch_x, touch_y;
    bool dark, touch_down, swallow_touch;
    bool sleep;                 /* power off at all (glaze/power.h) */
};
enum sb_idle_input { SB_IDLE_PASS, SB_IDLE_CONSUME, SB_IDLE_WAKE };
void sb_idle_load(struct sb_idle *s, uint32_t now);
int sb_idle_wait(const struct sb_idle *s, uint32_t now); /* -1: no deadline */
void sb_idle_sleep(struct sb_idle *s);
enum sb_idle_input sb_idle_input(struct sb_idle *s, const struct gz_input_event *e, uint32_t now);
/* Power off once the screen has been dark for after_ms (docs/POWER.md). */
void sb_idle_set_sleep(struct sb_idle *s, bool on, uint32_t after_ms);
int sb_idle_sleep_wait(const struct sb_idle *s, uint32_t now); /* -1: never */

/* The Power button: a press locks, holding it SB_POWER_HOLD_MS powers off. */
#define SB_POWER_HOLD_MS 1500u
struct sb_hold {
    uint32_t down_at;
    bool held, fired;
};
enum sb_hold_result { SB_HOLD_NONE, SB_HOLD_PRESS };
/* SB_HOLD_PRESS for a release before the hold time, of a press seen down. */
enum sb_hold_result sb_hold_input(struct sb_hold *h, const struct gz_input_event *e, uint32_t now);
int sb_hold_wait(const struct sb_hold *h, uint32_t now);     /* -1: not held */
bool sb_hold_due(struct sb_hold *h, uint32_t now);           /* true once per hold */
void sb_hold_reset(struct sb_hold *h);
#endif
