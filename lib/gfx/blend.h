/* libgfx's RGB565 blend, inlined into the pixel loops. The exported
 * gfx_blend565 can't be: under -fPIC GCC calls it through the GOT, once per
 * pixel, in case another module interposes it. */

#ifndef GFX_BLEND_H
#define GFX_BLEND_H

#include <stdint.h>

/* alpha 0..255; both colours split into 0x07E0F81F lanes and mixed at 5 bits. */
static inline uint16_t blend565(uint16_t dst, uint16_t src, int alpha)
{
    uint32_t a = ((uint32_t)alpha + 4) >> 3;      /* 0..32 */
    uint32_t d = (dst | ((uint32_t)dst << 16)) & 0x07E0F81Fu;
    uint32_t s = (src | ((uint32_t)src << 16)) & 0x07E0F81Fu;
    uint32_t r = (d + (((s - d) * a) >> 5)) & 0x07E0F81Fu;

    return (uint16_t)(r | (r >> 16));
}

#endif
