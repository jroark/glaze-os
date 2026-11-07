#include "hardicon.h"
#include <glaze/sb.h>

static bool over(const struct sb_hardicon *s, int x, int y)
{
    return y >= HARDICON_TOP - HARDICON_SLOP && hardicon_zone(x) == s->zone;
}

bool sb_hardicon_input(struct sb_hardicon *s, const struct gz_input_event *e, int x, int y, int *fire)
{
    *fire = -1;
    x = x < 0 ? 0 : x >= GZ_FB_WIDTH ? GZ_FB_WIDTH - 1 : x;
    switch (e->type) {
    case GZ_EV_TOUCH_DOWN:
        s->active = y >= HARDICON_TOP;
        s->zone = hardicon_zone(x);
        s->down_ms = e->time_ms;
        s->down_x = x;
        s->down_y = y;
        /* Just below the LCD may be a bottom-row tap that calibration
         * extrapolated past it: keep it from the app but never fire. */
        s->reject = y < HARDICON_TOP + HARDICON_MARGIN ? "edge" : NULL;
        return s->active;
    case GZ_EV_TOUCH_MOVE:
        /* A resistive panel reads a palm plus a stylus as a point between
         * them, so a stroke that wanders off its icon is not trusted. */
        if (s->active && !s->reject && !over(s, x, y))
            s->reject = "moved";
        return s->active;
    case GZ_EV_TOUCH_UP:
        if (!s->active)
            return false;
        s->active = false;
        if (!s->reject && !over(s, x, y))
            s->reject = "off";
        /* One noisy sample makes a DOWN and UP at the same moment. */
        if (!s->reject && (int32_t)(e->time_ms - s->down_ms) < HARDICON_MIN_MS)
            s->reject = "short";
        if (!s->reject)
            *fire = s->zone;
        return true;
    case GZ_EV_TOUCH_CANCEL:
        s->active = false;
        return false;
    default:
        return false;
    }
}
