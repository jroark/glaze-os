/* Input-only inactivity clock, independent of drawing and wall-clock time. */
#include "idle.h"
#include <glaze/display.h>

void sb_idle_load(struct sb_idle *s, uint32_t now)
{
    s->timeout_ms = (uint32_t)display_timeout_get() * 1000u;
    s->last_activity = now;
}

int sb_idle_wait(const struct sb_idle *s, uint32_t now)
{
    if (s->dark || !s->timeout_ms)
        return -1;
    uint32_t elapsed = now - s->last_activity;
    return elapsed >= s->timeout_ms ? 0 : (int)(s->timeout_ms - elapsed);
}

void sb_idle_sleep(struct sb_idle *s)
{
    s->dark = true;
    s->swallow_touch = s->touch_down;
    s->swallow_keys = s->keys_down;
}

enum sb_idle_input sb_idle_input(struct sb_idle *s, const struct gz_input_event *e, uint32_t now)
{
    bool wake = false, consume = s->dark;
    uint32_t key = e->code < 32 ? 1u << e->code : 0;
    if (e->type == GZ_EV_TOUCH_DOWN || e->type == GZ_EV_TOUCH_MOVE || e->type == GZ_EV_TOUCH_UP) {
        s->touch_x = e->x;
        s->touch_y = e->y;
    }
    switch (e->type) {
    case GZ_EV_TOUCH_DOWN:
        s->touch_down = true;
        if (s->dark)
            s->swallow_touch = true;
        wake = true;
        consume |= s->swallow_touch;
        break;
    case GZ_EV_TOUCH_MOVE:
        consume |= s->swallow_touch;
        break;
    case GZ_EV_TOUCH_UP:
        consume |= s->swallow_touch;
        s->touch_down = s->swallow_touch = false;
        break;
    case GZ_EV_KEY_DOWN:
        s->keys_down |= key;
        if (s->dark)
            s->swallow_keys |= key;
        wake = true;
        consume |= (s->swallow_keys & key) != 0;
        break;
    case GZ_EV_KEY_UP:
        consume |= (s->swallow_keys & key) != 0;
        s->keys_down &= ~key;
        s->swallow_keys &= ~key;
        break;
    case GZ_EV_TEXT:
        consume |= s->swallow_keys != 0;
        wake = true;
        break;
    default:
        return SB_IDLE_PASS;
    }
    s->last_activity = now;
    if (s->dark && wake) {
        s->dark = false;
        return SB_IDLE_WAKE;
    }
    return consume ? SB_IDLE_CONSUME : SB_IDLE_PASS;
}

void sb_idle_set_sleep(struct sb_idle *s, bool on, uint32_t after_ms)
{
    s->sleep = on;
    s->sleep_after_ms = after_ms;
}

/* From the last activity, like the screen timeout it follows; never with
 * the timeout off, since the screen then never goes dark. */
int sb_idle_sleep_wait(const struct sb_idle *s, uint32_t now)
{
    if (!s->sleep || !s->timeout_ms)
        return -1;
    uint32_t due = s->timeout_ms + s->sleep_after_ms;
    uint32_t elapsed = now - s->last_activity;
    return elapsed >= due ? 0 : (int)(due - elapsed);
}

enum sb_hold_result sb_hold_input(struct sb_hold *h, const struct gz_input_event *e, uint32_t now)
{
    if (e->code != GZ_KEY_POWER)
        return SB_HOLD_NONE;
    if (e->type == GZ_EV_KEY_DOWN && !h->held) {
        h->held = true;
        h->fired = false;
        h->down_at = now;
    } else if (e->type == GZ_EV_KEY_UP && h->held) {
        bool press = !h->fired;
        sb_hold_reset(h);
        return press ? SB_HOLD_PRESS : SB_HOLD_NONE;
    }
    return SB_HOLD_NONE;
}

int sb_hold_wait(const struct sb_hold *h, uint32_t now)
{
    if (!h->held || h->fired)
        return -1;
    uint32_t elapsed = now - h->down_at;
    return elapsed >= SB_POWER_HOLD_MS ? 0 : (int)(SB_POWER_HOLD_MS - elapsed);
}

bool sb_hold_due(struct sb_hold *h, uint32_t now)
{
    if (sb_hold_wait(h, now) != 0)
        return false;
    h->fired = true;
    return true;
}

/* After a sleep: the press that woke the device was never seen down. */
void sb_hold_reset(struct sb_hold *h)
{
    h->held = h->fired = false;
}
