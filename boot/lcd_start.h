/*
 * LCD panel start, traced from the stock BE-300 Windows CE SPL (0x80F038DC,
 * 0x80F034A0) and OAL (0x80078F38); see docs/DISPLAY.md. Shared by the SPL,
 * which lights the panel for the boot splash just as Casio's SPL does for its
 * own, and kernel/drivers/display.c. The includer defines the DISP_* register
 * names of kernel/hw.h and display_delay_us(us).
 */
#ifndef GLAZE_LCD_START_H
#define GLAZE_LCD_START_H

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

static void lcd_start(void)
{
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
}

#endif
