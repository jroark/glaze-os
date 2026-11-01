#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <glaze/display.h>
#include "../servers/springboard/idle.h"

static int32_t seconds = 60;
int pref_get(const char *key, void *buf, uint32_t len)
{
    assert(!strcmp(key, DISPLAY_TIMEOUT_PREF) && len >= sizeof(seconds));
    memcpy(buf, &seconds, sizeof(seconds));
    return sizeof(seconds);
}

static enum sb_idle_input input(struct sb_idle *s, int type, int code, uint32_t now)
{
    struct gz_input_event e = {.type = type, .code = code, .x = 42, .y = 99};
    return sb_idle_input(s, &e, now);
}

int main(void)
{
    struct sb_idle s = {0};
    sb_idle_load(&s, 1000);
    assert(sb_idle_wait(&s, 1000) == 60000);
    assert(sb_idle_wait(&s, 60999) == 1);
    assert(sb_idle_wait(&s, 61000) == 0);
    assert(input(&s, GZ_EV_TOUCH_DOWN, 0, 61000) == SB_IDLE_PASS);
    assert(sb_idle_wait(&s, 61000) == 60000 && s.touch_down);
    assert(input(&s, GZ_EV_TOUCH_MOVE, 0, 62000) == SB_IDLE_PASS);
    assert(input(&s, GZ_EV_TOUCH_UP, 0, 63000) == SB_IDLE_PASS && !s.touch_down);
    assert(sb_idle_wait(&s, 63000) == 60000);
    /* A drawing/notification event is not user activity. */
    assert(input(&s, 0x101, 0, 64000) == SB_IDLE_PASS);
    assert(sb_idle_wait(&s, 64000) == 59000);
    seconds = 30;
    sb_idle_load(&s, 70000);
    assert(sb_idle_wait(&s, 70000) == 30000);
    seconds = 0;
    sb_idle_load(&s, 70000);
    assert(sb_idle_wait(&s, 0xffffffffu) == -1);
    seconds = 600;
    sb_idle_load(&s, 0xfffffff0u);
    assert(sb_idle_wait(&s, 0x10) == 600000 - 32);
    assert(sb_idle_wait(&s, 0xfffffff0u + 600000u) == 0);

    sb_idle_sleep(&s);
    assert(sb_idle_wait(&s, 90000) == -1);
    assert(input(&s, GZ_EV_TOUCH_MOVE, 0, 90000) == SB_IDLE_CONSUME && s.dark);
    assert(input(&s, GZ_EV_TOUCH_UP, 0, 90001) == SB_IDLE_CONSUME && s.dark);
    assert(input(&s, GZ_EV_TOUCH_DOWN, 0, 90002) == SB_IDLE_WAKE && !s.dark);
    assert(s.touch_x == 42 && s.touch_y == 99);
    assert(input(&s, GZ_EV_TOUCH_MOVE, 0, 90003) == SB_IDLE_CONSUME);
    assert(input(&s, GZ_EV_TOUCH_UP, 0, 90004) == SB_IDLE_CONSUME);
    assert(input(&s, GZ_EV_TOUCH_DOWN, 0, 90005) == SB_IDLE_PASS);
    sb_idle_sleep(&s);       /* timeout during a held gesture */
    assert(s.swallow_touch);
    assert(input(&s, GZ_EV_TOUCH_UP, 0, 90006) == SB_IDLE_CONSUME && s.dark);
    assert(input(&s, GZ_EV_KEY_DOWN, GZ_KEY_POWER, 90007) == SB_IDLE_WAKE);
    assert(input(&s, GZ_EV_KEY_DOWN, GZ_KEY_POWER, 90008) == SB_IDLE_CONSUME);
    assert(input(&s, GZ_EV_TEXT, 'x', 90009) == SB_IDLE_CONSUME);
    assert(input(&s, GZ_EV_KEY_UP, GZ_KEY_POWER, 90010) == SB_IDLE_CONSUME);
    assert(input(&s, GZ_EV_KEY_DOWN, GZ_KEY_HOME, 90011) == SB_IDLE_PASS);
    sb_idle_sleep(&s);
    assert(input(&s, GZ_EV_KEY_UP, GZ_KEY_HOME, 90012) == SB_IDLE_CONSUME && s.dark);
    assert(input(&s, GZ_EV_TEXT, 'x', 90013) == SB_IDLE_WAKE);
    assert(input(&s, GZ_EV_TEXT, 'y', 90014) == SB_IDLE_PASS);
    assert(sb_idle_wait(&s, 90014) == 600000);

    /* Power off follows the screen timeout by sleep_after_ms. */
    struct sb_idle z = {0};
    seconds = 60;
    sb_idle_load(&z, 1000);
    assert(sb_idle_sleep_wait(&z, 1000) == -1);
    sb_idle_set_sleep(&z, true, 0);
    assert(sb_idle_sleep_wait(&z, 1000) == 60000);
    assert(sb_idle_sleep_wait(&z, 61000) == 0);
    sb_idle_set_sleep(&z, true, 5 * 60000);
    assert(sb_idle_sleep_wait(&z, 61000) == 300000);
    sb_idle_sleep(&z);
    assert(sb_idle_wait(&z, 61000) == -1 && sb_idle_sleep_wait(&z, 361000) == 0);
    assert(input(&z, GZ_EV_TOUCH_DOWN, 0, 200000) == SB_IDLE_WAKE);
    assert(sb_idle_sleep_wait(&z, 200000) == 360000);
    seconds = 0;                /* Never: the screen never goes dark */
    sb_idle_load(&z, 0);
    assert(sb_idle_sleep_wait(&z, 0xffffffffu) == -1);

    /* A press locks; holding powers off once, and its release is no press. */
    struct sb_hold h = {0};
    struct gz_input_event down = {.type = GZ_EV_KEY_DOWN, .code = GZ_KEY_POWER};
    struct gz_input_event up = {.type = GZ_EV_KEY_UP, .code = GZ_KEY_POWER};
    struct gz_input_event home = {.type = GZ_EV_KEY_UP, .code = GZ_KEY_HOME};
    assert(sb_hold_wait(&h, 0) == -1 && sb_hold_input(&h, &up, 10) == SB_HOLD_NONE);
    assert(sb_hold_input(&h, &down, 100) == SB_HOLD_NONE);
    assert(sb_hold_wait(&h, 100) == 1500 && !sb_hold_due(&h, 1599));
    assert(sb_hold_input(&h, &home, 200) == SB_HOLD_NONE);
    assert(sb_hold_input(&h, &up, 1599) == SB_HOLD_PRESS && sb_hold_wait(&h, 1600) == -1);
    assert(sb_hold_input(&h, &down, 0xfffffff0u) == SB_HOLD_NONE);
    assert(sb_hold_input(&h, &down, 0xfffffff8u) == SB_HOLD_NONE);   /* repeats */
    assert(sb_hold_wait(&h, 0x10) == 1500 - 32);
    assert(sb_hold_due(&h, 0xfffffff0u + 1500u) && !sb_hold_due(&h, 0xfffffff0u + 1600u));
    assert(sb_hold_wait(&h, 0xfffffff0u + 1600u) == -1);
    assert(sb_hold_input(&h, &up, 3000) == SB_HOLD_NONE);
    assert(sb_hold_input(&h, &down, 4000) == SB_HOLD_NONE);
    sb_hold_reset(&h);          /* asleep meanwhile */
    assert(sb_hold_input(&h, &up, 9000) == SB_HOLD_NONE);
    puts("idle: deadlines, settings reload, wraparound, held gestures, wake input, power off and the Power hold PASS");
    return 0;
}
