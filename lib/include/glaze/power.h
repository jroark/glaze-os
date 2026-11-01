/* Battery display and sleep settings, shared by Settings and SpringBoard.
 * The status itself is the kernel's: gz_power_status() (glaze/abi.h,
 * docs/hardware/POWER.md). */
#ifndef GLAZE_POWER_H
#define GLAZE_POWER_H
#include <glaze/abi.h>
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

/*
 * Sleep (docs/hardware/POWER.md "Power off"): on battery and plugged in, whether the
 * device powers off by itself once the screen has gone dark, how deeply
 * (GZ_SLEEP_SUSPEND or GZ_SLEEP_HIBERNATE, POWER_SLEEP_NEVER for never) and
 * after how many more minutes. Holding Power uses the same mode, or Suspend
 * when it is Never. Settings notifies SpringBoard with SB_NOTIFY_POWER.
 */
#define POWER_SLEEP_PREF    "power.sleep"
#define POWER_SLEEP_VERSION 1
#define POWER_SLEEP_NEVER   0

struct power_sleep {
    uint8_t version;
    uint8_t battery_mode, ac_mode;
    uint8_t pad;
    uint16_t battery_min, ac_min;   /* minutes after the screen goes dark */
};

#define POWER_SLEEP_DEFAULTS { POWER_SLEEP_VERSION, GZ_SLEEP_SUSPEND, GZ_SLEEP_SUSPEND, 0, 0, 15 }

/* The choices Settings offers. */
#define POWER_SLEEP_CHOICES 6
static inline int power_sleep_minutes(int i)
{
    static const uint16_t m[POWER_SLEEP_CHOICES] = { 0, 1, 5, 15, 30, 60 };
    return i >= 0 && i < POWER_SLEEP_CHOICES ? m[i] : 0;
}

static inline bool power_sleep_valid_mode(int mode)
{
    return mode == POWER_SLEEP_NEVER || mode == GZ_SLEEP_SUSPEND || mode == GZ_SLEEP_HIBERNATE;
}

static inline bool power_sleep_valid(const struct power_sleep *s)
{
    return s->version == POWER_SLEEP_VERSION && power_sleep_valid_mode(s->battery_mode) &&
           power_sleep_valid_mode(s->ac_mode) && s->battery_min <= 24 * 60 && s->ac_min <= 24 * 60;
}

static inline struct power_sleep power_sleep_get(void)
{
    struct power_sleep s;
    if (pref_get(POWER_SLEEP_PREF, &s, sizeof(s)) != sizeof(s) || !power_sleep_valid(&s))
        s = (struct power_sleep)POWER_SLEEP_DEFAULTS;
    return s;
}

static inline int power_sleep_put(const struct power_sleep *s)
{
    return pref_put(POWER_SLEEP_PREF, s, sizeof(*s));
}

static inline const char *power_sleep_mode_name(int mode)
{
    return mode == GZ_SLEEP_SUSPEND ? "Suspend" : mode == GZ_SLEEP_HIBERNATE ? "Hibernate" : "Never";
}

#endif
