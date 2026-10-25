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
