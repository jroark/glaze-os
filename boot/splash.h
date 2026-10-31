/*
 * Boot splash: the "Glaze" wordmark and a progress bar on black, drawn
 * straight into the framebuffer by the SPL and then the kernel until
 * SpringBoard maps it. A NAND boot is otherwise dark until SpringBoard's
 * first frame. 240x320 RGB565, 256-pixel (512-byte) rows.
 */
#ifndef GLAZE_SPLASH_H
#define GLAZE_SPLASH_H

#include <stdint.h>
#include "bootlogo.h"

#define SPLASH_PITCH    256
#define SPLASH_LOGO_X   ((240 - BOOTLOGO_W) / 2)
#define SPLASH_LOGO_Y   124
#define SPLASH_BAR_X    64
#define SPLASH_BAR_W    112
#define SPLASH_BAR_Y    182
#define SPLASH_BAR_H    4
#define SPLASH_TRACK    0x2945u     /* dark grey */
#define SPLASH_FILL     0xFFFFu

/* Grey for a 4-bit alpha on black. */
static inline uint16_t splash_grey(unsigned a)
{
    unsigned c5 = a * 31u / 15u, c6 = a * 63u / 15u;
    return (uint16_t)(c5 << 11 | c6 << 5 | c5);
}

static inline void splash_bar(volatile uint16_t *fb, unsigned from, unsigned to,
    uint16_t colour)
{
    for (unsigned y = 0; y < SPLASH_BAR_H; y++)
        for (unsigned x = from; x < to; x++)
            fb[(SPLASH_BAR_Y + y) * SPLASH_PITCH + SPLASH_BAR_X + x] = colour;
}

static inline void splash_draw(volatile uint16_t *fb)
{
    volatile uint32_t *w = (volatile uint32_t *)fb;

    for (unsigned i = 0; i < SPLASH_PITCH * 320 / 2; i++)
        w[i] = 0;
    for (unsigned y = 0; y < BOOTLOGO_H; y++)
        for (unsigned x = 0; x < BOOTLOGO_W; x++) {
            unsigned b = bootlogo[y * ((BOOTLOGO_W + 1) / 2) + x / 2];
            unsigned a = x & 1 ? b >> 4 : b & 15u;
            if (a)
                fb[(SPLASH_LOGO_Y + y) * SPLASH_PITCH + SPLASH_LOGO_X + x] = splash_grey(a);
        }
    splash_bar(fb, 0, SPLASH_BAR_W, SPLASH_TRACK);
}

/* Fill the bar to permille/1000; it only ever grows. */
static inline void splash_progress(volatile uint16_t *fb, unsigned permille)
{
    if (permille > 1000)
        permille = 1000;
    splash_bar(fb, 0, SPLASH_BAR_W * permille / 1000, SPLASH_FILL);
}

#endif
