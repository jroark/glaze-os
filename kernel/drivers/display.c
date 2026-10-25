/* Physical display controls, traced from the stock BE-300 Windows CE 3.0
 * ddi.dll. Keep the framebuffer unchanged. See docs/DISPLAY.md. */
#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"
#include <glaze/display.h>

static int brightness_now = -1, contrast_now = -1;

static void delay_edge(void)
{
    uint64_t start = time_us();
    /* One extra clock quantum keeps the real interval >= 5 us even when
     * the first timestamp falls just before a microsecond boundary. */
    while (time_us() - start <= 5)
        ;
}

int display_set(int brightness, int contrast)
{
    struct display_settings v = { DISPLAY_VERSION, brightness, contrast };
    if (!display_valid(&v))
        return GZ_EINVAL;
    if (brightness != brightness_now) {
        /* ddi.dll 0x01A52050/70: increasing the level lowers this duty
         * word; 0 is maximum brightness, not backlight-off. */
        static const uint16_t duty[] = { 0xD0B, 0x9F9, 0x7AC, 0x3D5, 0 };
        DISP_BACKLIGHT = duty[brightness - 1];
        brightness_now = brightness;
    }
    if (contrast != contrast_now) {
        /* Native commands, MSB first, data=bit 2, clock=bit 1, latch=bit 0.
         * Clear only these three bits; preserve the rest of the register. */
        uint8_t command = (uint8_t)(208 - contrast * 3);
        for (int bit = 7; bit >= 0; bit--) {
            DISP_CONTRAST &= ~7u;
            DISP_CONTRAST |= ((command >> bit) & 1u) << 2;
            delay_edge();
            DISP_CONTRAST |= 2u;
            delay_edge();
        }
        DISP_CONTRAST &= ~7u;
        delay_edge();
        DISP_CONTRAST |= 1u;
        delay_edge();
        DISP_CONTRAST &= ~1u;
        delay_edge();
        contrast_now = contrast;
    }
    return 0;
}
