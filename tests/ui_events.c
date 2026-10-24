/*
 * libui's side of backgrounding (lib/ui/app.c), natively: a touch held at
 * Home ends with GZ_EV_TOUCH_CANCEL before GZ_EV_APP_DEACTIVATE, the next
 * ui_next_event suspends the process and returns the ACTIVATE that resumes
 * it (dropping what came in between), and ui_set_background_running turns
 * suspension off. Also the libui widgets' handling of the cancel.
 *   make -C host ui-events-test
 */

#include <assert.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SELF 42

static struct sb_shared *sh;
static uint16_t pixels[UI_W * UI_H];
static uint32_t now;
static int suspends, sounds, set_bg_rv;
/* Queued by the fake SpringBoard when the app suspends itself. */
static struct gz_input_event while_away[4];
static int n_away;

int gz_port_create(void) { return 1; }
int gz_port_lookup(const char *name) { return 2; }
int gz_send(int port, const struct gz_msg *m, struct gz_msg *r) { return 0; }
void *gz_shm_map(int id) { return NULL; }
int gz_getpid(void) { return SELF; }
void gz_sleep_ms(uint32_t ms) { now += ms; }
uint32_t gz_time_ms(void) { return now; }

int gz_recv(int port, struct gz_msg *m, int timeout_ms)
{
    assert(timeout_ms >= 0);            /* only wait_activate blocks, and it is never left waiting */
    now += (uint32_t)timeout_ms;
    return GZ_ETIMEDOUT;
}

static void push(uint16_t type, int x, int y)
{
    sh->ev[sh->head % SB_RING] = (struct gz_input_event){.type = type, .x = (int16_t)x, .y = (int16_t)y};
    sh->head++;
}

/* The app suspends itself: while it is frozen the user taps around, then
 * brings it back. */
int gz_proc_set_bg(int pid, int state)
{
    assert(pid == SELF && state == GZ_PROC_SUSPENDED);
    suspends++;
    for (int i = 0; i < n_away; i++)
        push(while_away[i].type, while_away[i].x, while_away[i].y);
    n_away = 0;
    sh->background = 0;
    push(GZ_EV_APP_ACTIVATE, 0, 0);
    return set_bg_rv;
}

void uisnd_event(const struct gz_input_event *e) { sounds++; }
void uisnd_sounded(void) {}
void uitr_release(void) {}
void audio_sound(int sound) {}
void res_set_bundle(const char *bundle) {}

static struct gz_input_event next(int timeout)
{
    struct gz_input_event e = {0};
    assert(ui_next_event(&e, timeout) == 1);
    return e;
}

static void home(void)
{
    sh->background = 1;
    push(GZ_EV_APP_DEACTIVATE, 0, 0);
}

static void reset(void)
{
    memset(sh, 0, sizeof(*sh));
    suspends = sounds = n_away = 0;
    set_bg_rv = 0;
    ui_set_background_running(false);
}

int main(void)
{
    sh = calloc(1, sizeof(*sh));
    ui.shared = sh;
    gfx_surface_init(&ui.surf, pixels, UI_W, UI_H, UI_W);

    /* Home mid-touch: the touch is cancelled where it last was, then
     * DEACTIVATE; the next call suspends, drops the taps made meanwhile and
     * returns ACTIVATE with everything to redraw. */
    reset();
    push(GZ_EV_TOUCH_DOWN, 10, 20);
    push(GZ_EV_TOUCH_MOVE, 11, 21);
    push(GZ_EV_TOUCH_MOVE, 12, 22);
    home();
    assert(next(0).type == GZ_EV_TOUCH_DOWN);
    struct gz_input_event e = next(0);
    assert(e.type == GZ_EV_TOUCH_MOVE && e.x == 12 && e.y == 22);  /* moves coalesced */
    int heard = sounds;
    e = next(0);
    assert(e.type == GZ_EV_TOUCH_CANCEL && e.x == 12 && e.y == 22);
    assert(sounds == heard);           /* no tap sound for it */
    assert(next(0).type == GZ_EV_APP_DEACTIVATE && suspends == 0);
    while_away[n_away++] = (struct gz_input_event){.type = GZ_EV_TOUCH_DOWN, .x = 5, .y = 5};
    while_away[n_away++] = (struct gz_input_event){.type = GZ_EV_TOUCH_UP, .x = 5, .y = 5};
    ui.dirty = gfx_r(0, 0, 0, 0);
    assert(next(0).type == GZ_EV_APP_ACTIVATE && suspends == 1);
    assert(ui.dirty.w == UI_W && ui.dirty.h == UI_H);
    assert(sh->tail == sh->head);       /* the taps made while away are gone */
    assert(ui_next_event(&e, 0) == 0);  /* and nothing else is pending */
    printf("cancel, deactivate, suspend, activate: PASS\n");

    /* A touch already lifted needs no cancel. */
    reset();
    push(GZ_EV_TOUCH_DOWN, 1, 1);
    push(GZ_EV_TOUCH_UP, 1, 1);
    home();
    assert(next(0).type == GZ_EV_TOUCH_DOWN && next(0).type == GZ_EV_TOUCH_UP);
    assert(next(0).type == GZ_EV_APP_DEACTIVATE);
    assert(next(0).type == GZ_EV_APP_ACTIVATE && suspends == 1);
    printf("no cancel after a lift: PASS\n");

    /* An app that keeps running gets DEACTIVATE and carries on. */
    reset();
    ui_set_background_running(true);
    assert(sh->keep_running == 1);
    home();
    assert(next(0).type == GZ_EV_APP_DEACTIVATE);
    assert(ui_next_event(&e, 0) == 0 && suspends == 0);
    printf("background running: PASS\n");

    /* Brought back before it could suspend: the kernel refuses (it is no
     * longer in the background) and the ACTIVATE already queued ends the
     * wait. */
    reset();
    home();
    assert(next(0).type == GZ_EV_APP_DEACTIVATE);
    set_bg_rv = GZ_EPERM;
    assert(next(0).type == GZ_EV_APP_ACTIVATE && suspends == 1);
    printf("resumed before suspending: PASS\n");

    /* libui's widgets drop a cancelled touch without acting on it. */
    struct gz_input_event down = {.type = GZ_EV_TOUCH_DOWN, .x = 20, .y = 60};
    struct gz_input_event cancel = {.type = GZ_EV_TOUCH_CANCEL, .x = 20, .y = 60};
    struct ui_list list;
    bool dirty = false;
    ui_list_init(&list, gfx_r(0, 44, UI_W, 200), 40);
    ui_list_reset(&list, 10);
    assert(ui_list_event(&list, &down, &dirty) < 0 && list.pressed == 0);
    assert(ui_list_event(&list, &cancel, &dirty) < 0 && list.pressed < 0 && !list.sc.dragging);
    assert(!list.sc.animating);

    struct ui_keyboard kb;
    bool changed = false;
    memset(&kb, 0, sizeof(kb));
    kb.pressed = -1;
    int kb_y = UI_H - UI_KB_H;
    struct gz_input_event key = {.type = GZ_EV_TOUCH_DOWN, .x = 12, .y = (int16_t)(kb_y + 20)};
    ui_keyboard_event(&kb, &key, kb_y, &changed);
    assert(kb.pressed >= 0);
    key.type = GZ_EV_TOUCH_CANCEL;
    assert(ui_keyboard_event(&kb, &key, kb_y, &changed) == 0 && kb.pressed < 0 && changed);
    printf("list and keyboard cancel: PASS\n");

    printf("ui events: PASS\n");
    free(sh);
    return 0;
}
