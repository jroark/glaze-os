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
};

enum {
    SB_STATUS_GRAY = 0,     /* light gray gradient, dark text */
    SB_STATUS_BLACK,        /* opaque black, white text */
};

#define SB_NOTIFY_EVENTS    0x1u

#define SB_RING             64

struct sb_shared {
    volatile uint32_t head;         /* written by SpringBoard */
    volatile uint32_t tail;         /* written by the app */
    struct gz_input_event ev[SB_RING];
    uint32_t surface_offset;        /* bytes from the start of the shm */
    uint32_t stride;                /* pixels */
};

/* App-level events beyond the kernel's input codes. */
#define GZ_EV_APP_ACTIVATE  0x100

#endif
