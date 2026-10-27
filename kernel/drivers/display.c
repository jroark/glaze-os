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
     * Restore the inherited configuration, including its reserved bits;
     * leave the period and brightness duty untouched. */
    if (!on) {
        saved_control = DISP_BL_CONTROL;
        saved_config = DISP_BL_CONFIG;
        saved_gate = DISP_BL_GATE & 1u;
        DISP_BL_CONTROL = 0;
        DISP_BL_CONFIG = 0;
        DISP_BL_GATE &= ~1u;
    } else {
        DISP_BL_GATE = (DISP_BL_GATE & ~1u) | saved_gate;
        DISP_BL_CONFIG = saved_config;
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

static void program_contrast(uint8_t command)
{
    /* Native command, MSB first: data=bit 2, clock=bit 1, latch=bit 0.
     * Clear only these three bits; preserve the rest of the register. */
    for (int bit = 7; bit >= 0; bit--) {
        DISP_CONTRAST &= ~7u;
        DISP_CONTRAST |= ((command >> bit) & 1u) << 2;
        display_delay_us(5);
        DISP_CONTRAST |= 2u;
        display_delay_us(5);
    }
    DISP_CONTRAST &= ~7u;
    display_delay_us(5);
    DISP_CONTRAST |= 1u;
    display_delay_us(5);
    DISP_CONTRAST &= ~1u;
    display_delay_us(5);
}

void display_init(bool inherit)
{
    /* Never use a post-boot readback as evidence that a NAND cold boot
     * configured the LCD. Only a validated RAM handoff may retain a live
     * WinCE display, including its contrast trim and framebuffer contents. */
    brightness_now = contrast_now = -1;
    powered = true;
    if (inherit && DISP_LCD(0x48) == 0x030Fu &&
        (DISP_LCD_CLOCK & 1u) && (DISP_LCD_GATE & 1u) &&
        (DISP_BL_CONTROL & 1u) && (DISP_BL_GATE & 1u))
        return;

    /* WinCE SPL 0x80F038DC enables CMM and the two LCD clock gates, then
     * starts the backlight with period 0x0F70 and duty zero. Select BLG
     * mode 2 as ddi.dll 0x01A55900 does, preserving reserved config bits.
     * Duty zero is maximum brightness, not power-off. */
    DISP_BL_GATE = 1;
    DISP_LCD_CLOCK = 1;
    DISP_LCD_GATE = 1;
    DISP_BL_CONTROL = 0;
    DISP_BL_CONFIG = (DISP_BL_CONFIG & ~0xFu) | 2u;
    DISP_BL_PERIOD = 0x0F70;
    DISP_BACKLIGHT = 0;
    DISP_BL_CONTROL = 1;

    /* WinCE SPL 0x80F034A0 and OAL 0x80078F38. These are the captured
     * original-panel parameters, not inferred width/pitch/porch fields.
     * The framebuffer remains RGB565 at PA 0x0A200000, pitch 512 bytes. */
    DISP_LCD(0x00) = 0x20;
    DISP_LCD(0x04) = 0x02;
    DISP_LCD(0x08) = 0x50;
    DISP_LCD(0x0C) = 0xF1;
    DISP_LCD(0x10) = 0x02;
    DISP_LCD(0x14) = 0;
    DISP_LCD(0x1C) = 0;
    DISP_LCD(0x24) = 0x14;
    DISP_LCD(0x48) = 0;
    DISP_LCD(0x48) |= 1u;
    display_delay_us(1000);
    DISP_LCD(0x48) |= 0x0100u;
    DISP_LCD(0x2C) = 1;
    program_contrast(0xC1);       /* stock nominal contrast level 5 */
    DISP_LCD(0x48) |= 2u;
    display_delay_us(1000);
    DISP_LCD(0x48) |= 0x0Cu;
    display_delay_us(20000);
    DISP_LCD(0x48) |= 0x0200u;

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
