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
        return s->active;
    case GZ_EV_TOUCH_MOVE:
        return s->active;
    case GZ_EV_TOUCH_UP:
        if (!s->active)
            return false;
        s->active = false;
        if (over(s, x, y))
            *fire = s->zone;
        return true;
    case GZ_EV_TOUCH_CANCEL:
        s->active = false;
        return false;
    default:
        return false;
    }
}
