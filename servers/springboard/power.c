/* Low-battery rules (power.h). The kernel's flags follow Windows CE's
 * driver; the thresholds here follow iPhone OS: a banner at 20 % and at
 * 10 % of the level estimate, once per discharge, and a safe sleep once
 * the battery has stayed critical for SB_POWER_CRITICAL_MS. */
#include "power.h"
#include <string.h>

void sb_power_init(struct sb_power *p)
{
    memset(p, 0, sizeof(*p));
    p->st.flags = GZ_POWER_AC;  /* until the kernel answers: no warnings */
    p->st.percent = p->st.level = 100;
    p->warned = 100;
}

bool sb_power_on_battery(const struct gz_power_status *st)
{
    return (st->flags & (GZ_POWER_VALID | GZ_POWER_AC)) == GZ_POWER_VALID;
}

bool sb_power_critical(const struct sb_power *p)
{
    const struct gz_power_status *st = &p->st;

    /* PMU BATTINTR only counts with the A/D agreeing: its source on the
     * BE-300 isn't known (docs/POWER.md). */
    return sb_power_on_battery(st) && ((st->flags & GZ_POWER_CRITICAL) ||
        ((st->flags & GZ_POWER_BATTINT) && (st->flags & GZ_POWER_LOW)));
}

bool sb_power_set(struct sb_power *p, const struct gz_power_status *st)
{
    const uint32_t shown = GZ_POWER_VALID | GZ_POWER_AC | GZ_POWER_CHARGING |
                           GZ_POWER_LOW | GZ_POWER_CRITICAL;
    bool changed = (p->st.flags & shown) != (st->flags & shown) ||
                   p->st.level != st->level;

    p->st = *st;
    if (!sb_power_on_battery(st)) {
        p->warned = 100;
        p->critical_since = 0;
        p->slept = false;
    }
    return changed;
}

enum sb_power_action sb_power_check(struct sb_power *p, uint32_t now_ms)
{
    const struct gz_power_status *st = &p->st;

    if (!sb_power_on_battery(st))
        return SB_POWER_NONE;
    if (sb_power_critical(p)) {
        if (!p->critical_since)
            p->critical_since = now_ms | 1u;
        else if (!p->slept && now_ms - p->critical_since >= SB_POWER_CRITICAL_MS) {
            p->slept = true;
            return SB_POWER_SLEEP;
        }
    } else {
        p->critical_since = 0;
    }
    /* By level, not CE's LOW band (25 % and below, level 10-25). */
    int at = st->level <= 10 ? 10 : st->level <= 20 ? 20 : 100;
    if (at < p->warned) {
        p->warned = at;
        return SB_POWER_WARN;
    }
    return SB_POWER_NONE;
}
