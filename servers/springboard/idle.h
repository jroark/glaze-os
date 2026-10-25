#ifndef SB_IDLE_H
#define SB_IDLE_H
#include <glaze/abi.h>

struct sb_idle {
    uint32_t last_activity, timeout_ms, keys_down, swallow_keys;
    int16_t touch_x, touch_y;
    bool dark, touch_down, swallow_touch;
};
enum sb_idle_input { SB_IDLE_PASS, SB_IDLE_CONSUME, SB_IDLE_WAKE };
void sb_idle_load(struct sb_idle *s, uint32_t now);
int sb_idle_wait(const struct sb_idle *s, uint32_t now); /* -1: no deadline */
void sb_idle_sleep(struct sb_idle *s);
enum sb_idle_input sb_idle_input(struct sb_idle *s, const struct gz_input_event *e, uint32_t now);
#endif
