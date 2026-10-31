/* Original LCD startup and physical display controls, traced from the
 * stock BE-300 Windows CE 3.0 SPL, OAL and ddi.dll. See docs/DISPLAY.md. */
#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"
#include <glaze/display.h>

static int brightness_now = -1, contrast_now = -1;
static bool powered = true;
static uint32_t saved_control, saved_config, saved_gate;

int display_power(int on)
{
    if (on != 0 && on != 1)
        return GZ_EINVAL;
    if (powered == (bool)on)
        return 0;
    /* Stock ddi.dll 0x01A55900: off writes zero to BLG+0/+4, then
     * clears CMM+0 bit 0. The inverse enables that gate before configuring
     * PWM and enabling BLG+0. See docs/DISPLAY.md for the pointer mapping.
     * Restore our mode nibble while preserving the buzzer/other channel
     * settings; leave both channels' period and duty words untouched. */
    if (!on) {
        saved_control = DISP_BL_CONTROL;
        saved_config = DISP_BL_CONFIG;
        saved_gate = DISP_BL_GATE & 1u;
        DISP_BL_CONTROL = 0;
        /* BLG config and CMM gate are shared with the piezo channel. */
        if (buzzer_active()) DISP_BL_CONFIG |= 0x0202;
        else { DISP_BL_CONFIG &= ~0xFu; DISP_BL_GATE &= ~1u; }
    } else {
        DISP_BL_GATE = (DISP_BL_GATE & ~1u) | saved_gate | (buzzer_active() ? 1u : 0u);
        DISP_BL_CONFIG = (DISP_BL_CONFIG & ~0xFu) | (saved_config & 0xFu);
        DISP_BL_CONTROL = saved_control;
    }
    powered = on;
    return 0;
}

static void display_delay_us(uint32_t duration)
{
    uint64_t start = time_us();
    /* One extra clock quantum keeps the real interval >= duration even when
     * the first timestamp falls just before a microsecond boundary. */
    while (time_us() - start <= duration)
        ;
}

#include "../../boot/lcd_start.h"

void display_init(bool inherit)
{
    /* Never use a post-boot readback as evidence that a NAND cold boot
     * configured the LCD. Only a validated RAM handoff may retain a live
     * WinCE display, including its contrast trim and framebuffer contents,
     * and a NAND boot the splash Glaze's SPL lit (kernel/bootsplash.c). */
    brightness_now = contrast_now = -1;
    powered = true;
    if (inherit && DISP_LCD(0x48) == 0x030Fu &&
        (DISP_LCD_CLOCK & 1u) && (DISP_LCD_GATE & 1u) &&
        (DISP_BL_CONTROL & 1u) && (DISP_BL_GATE & 1u)) {
        if (splash_live()) {    /* the SPL ran lcd_start() itself */
            brightness_now = DISPLAY_BRIGHTNESS_MAX;
            contrast_now = DISPLAY_CONTRAST_DEFAULT;
        }
        return;
    }

    lcd_start();

    brightness_now = DISPLAY_BRIGHTNESS_MAX;
    contrast_now = DISPLAY_CONTRAST_DEFAULT;
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
        program_contrast((uint8_t)(208 - contrast * 3));
        contrast_now = contrast;
    }
    return 0;
}
