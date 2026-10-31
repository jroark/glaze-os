#ifndef SB_POWER_H
#define SB_POWER_H
#include <glaze/abi.h>

/* Low-battery warnings and the critical safe sleep (docs/POWER.md), apart
 * from SpringBoard's drawing so the rules test natively. */
#define SB_POWER_CRITICAL_MS    30000u  /* critical this long: safe sleep */
#define SB_POWER_CRITICAL_IDLE  15000u  /* screen timeout while critical */

struct sb_power {
    struct gz_power_status st;
    int warned;                 /* lowest level warned at this discharge */
    uint32_t critical_since;    /* ms, 0: not critical */
    bool slept;                 /* safe sleep done this discharge */
};

enum sb_power_action { SB_POWER_NONE, SB_POWER_WARN, SB_POWER_SLEEP };

void sb_power_init(struct sb_power *p);
/* Takes a new status; true when what the status bar shows changed. */
bool sb_power_set(struct sb_power *p, const struct gz_power_status *st);
/* What to do now: warn (at 20 and 10 %) or safe-sleep. */
enum sb_power_action sb_power_check(struct sb_power *p, uint32_t now_ms);
bool sb_power_on_battery(const struct gz_power_status *st);
bool sb_power_critical(const struct sb_power *p);
#endif
