/* Battery display settings, shared by Settings and SpringBoard. The status
 * itself is the kernel's: gz_power_status() (glaze/abi.h, docs/POWER.md). */
#ifndef GLAZE_POWER_H
#define GLAZE_POWER_H
#include <glaze/prefs.h>

/* Show the level as a percentage beside the status bar's battery. */
#define POWER_PERCENT_PREF "sb.battery_pct"

static inline bool power_percent_get(void)
{
    int32_t on;
    return pref_get(POWER_PERCENT_PREF, &on, sizeof(on)) == sizeof(on) && on;
}

static inline int power_percent_put(bool on)
{
    int32_t v = on;
    return pref_put(POWER_PERCENT_PREF, &v, sizeof(v));
}

#endif
