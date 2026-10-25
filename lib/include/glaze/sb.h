/*
 * SpringBoard (compositor) <-> app protocol.
 *
 * An app looks up the "sb" port and sends SB_HELLO with its own event port.
 * SpringBoard replies with a shared-memory id holding a struct sb_shared
 * header followed by the app's 240 x 304 RGB565 surface (the top 16 rows of
 * the screen are SpringBoard's status bar). The app draws into the surface
 * and sends SB_PRESENT with the dirty rectangle; the reply arrives once the
 * rectangle is on screen, which paces the app's rendering.
 *
 * Input travels the other way through the event ring in the shared header:
 * SpringBoard appends events (in surface coordinates) and notifies the
 * app's port with SB_NOTIFY_EVENTS, so SpringBoard never blocks on an app.
 *
 * Badges and notifications are SpringBoard's, so they outlive the app: the
 * front app sets its icon badge and posts notifications, optionally for a
 * later wall-clock time (glaze/notify.h seconds), and SpringBoard keeps them
 * in preferences across reboots. A note fires as a banner, on the lock
 * screen, or as GZ_EV_APP_NOTIFY when its own app is in front. The SB_*
 * requests carry the sender's pid in w3 and are accepted only from the
 * current app; their text travels in sb_shared.req.
 *
 * Other processes (servers) post for an app named by its romfs path (an
 * installed app's bundle folder, glaze/pkg.h) through
 * the *_FOR requests, after SB_CLIENT_OPEN hands them a request page of their
 * own (client in lib/libc/notify.c). The kernel does not identify senders,
 * so these are not authenticated: any process may badge or notify any app.
 */
#ifndef GLAZE_SB_H
#define GLAZE_SB_H

#include <glaze/abi.h>

#define SB_PORT_NAME        "sb"
#define SB_STATUS_H         16
#define SB_APP_W            240
#define SB_APP_H            (GZ_FB_HEIGHT - SB_STATUS_H)

enum {
    SB_HELLO = 1,           /* w1 = app event port, w2 = status style; reply w4 = GZ_INPUT_* */
    SB_PRESENT,             /* w1..w4 = dirty x, y, w, h */
    SB_SET_STATUS,          /* w1 = status style */
    SB_BADGE_SET,           /* w1 = count (0 clears) */
    SB_NOTE_POST,           /* w1 = fire time (0 = now), w2 = id, w4 = badge increment */
    SB_NOTE_CANCEL,         /* w1 = id */
    SB_NOTE_PENDING,        /* w1 = id; reply w1 = fire time, 0 if none */
    SB_CLIENT_OPEN,         /* reply w1 = shm id of a struct sb_note_req */
    SB_BADGE_SET_FOR,       /* as SB_BADGE_SET, for req.path */
    SB_NOTE_POST_FOR,       /* as SB_NOTE_POST, for req.path */
    SB_NOTE_CANCEL_FOR,     /* as SB_NOTE_CANCEL, for req.path */
    /* Foreground built-in Settings only; w3 = pid. PREVIEW carries a
     * touch_calibration in w4..w10. All replies return a status in w0. */
    SB_TOUCH_BEGIN,
    SB_TOUCH_PREVIEW,
    SB_TOUCH_SAVE,
    SB_TOUCH_CANCEL,
    SB_TOUCH_RESET,
    /* Foreground Settings only; w1 = native brightness, w2 = contrast,
     * w3 = pid. Replies return status in w0 and current levels in w1/w2. */
    SB_DISPLAY_GET,
    SB_DISPLAY_PREVIEW,
    SB_DISPLAY_SAVE,
    SB_DISPLAY_CANCEL,
    SB_DISPLAY_RESET,
};

enum {
    SB_STATUS_GRAY = 0,     /* light gray gradient, dark text */
    SB_STATUS_BLACK,        /* opaque black, white text */
    SB_STATUS_FULLSCREEN,   /* opt-in 240 x 320 surface; Home stays with SpringBoard */
};

#define SB_NOTIFY_EVENTS    0x1u
/* To SpringBoard's port: the clock or the NTP settings (glaze/tz.h)
 * changed, so redraw the time and look at the settings again. */
#define SB_NOTIFY_TIME      0x2u

#define SB_RING             64

/* Text of a notification request; path is used only by the *_FOR requests. */
struct sb_note_req {
    char path[48];                  /* "bin/clock", or "/Applications/hello.app" */
    char title[32];
    char body[96];
};

struct sb_shared {
    volatile uint32_t head;         /* written by SpringBoard */
    volatile uint32_t tail;         /* written by the app */
    struct gz_input_event ev[SB_RING];
    uint32_t surface_offset;        /* bytes from the start of the shm */
    uint32_t stride;                /* pixels */
    struct sb_note_req req;         /* written by the app before a request */
    char bundle[48];                /* an installed app's bundle folder, else "" */
    volatile uint32_t background;   /* 1 while the app is in the background */
    volatile uint32_t keep_running; /* app: not to be suspended there (ui_set_background_running) */
};

/* App-level events beyond the kernel's input codes. */
#define GZ_EV_APP_ACTIVATE      0x100
#define GZ_EV_APP_NOTIFY        0x101   /* code = low 16 bits of the note's id */
#define GZ_EV_APP_DEACTIVATE    0x102
/* From libui, before DEACTIVATE when a touch is down: the touch is over, but
 * nothing was released, so nothing should act (x, y: its last position). */
#define GZ_EV_TOUCH_CANCEL      0x103
/* SpringBoard cancelled an unsaved calibration (timeout, Home or lock). */
#define GZ_EV_TOUCH_CAL_END     0x104
/* A system transition reverted a display drag; x/y = brightness/contrast. */
#define GZ_EV_DISPLAY_REVERT    0x105

#endif
