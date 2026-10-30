/* The real kernel driver with MMIO redirected to registers, plus the real
 * SpringBoard display service. ASan/UBSan checks persistence and ranges. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <glaze/display.h>
#include <glaze/fs.h>
#include <glaze/prefs.h>
#include <glaze/sb.h>
#include "../servers/springboard/display.h"

#define GLAZE_HW_H
static volatile uint32_t backlight, panel = 0xA5A50000;
static volatile uint32_t bl_control = 1, bl_config = 0x202, bl_gate = 0xA5A50001;
static volatile uint32_t bl_period, lcd_clock, lcd_gate, lcd[0x50 / 4];
static volatile uint32_t buzz_control;
bool buzzer_active(void) { return buzz_control != 0; }
#define DISP_BACKLIGHT backlight
#define DISP_CONTRAST panel
#define DISP_BL_CONTROL bl_control
#define DISP_BL_CONFIG bl_config
#define DISP_BL_GATE bl_gate
#define DISP_BL_PERIOD bl_period
#define DISP_LCD_CLOCK lcd_clock
#define DISP_LCD_GATE lcd_gate
#define DISP_LCD(off) lcd[(off) / 4]
#include "../kernel/drivers/display.c"

static uint64_t us;
static uint32_t previous;
static unsigned shifted, clocks, latches;
static bool watch_startup;
static struct { uint32_t control; uint64_t at; } phases[5];
static unsigned phase_count;
uint64_t time_us(void)
{
    assert((panel & ~7u) == 0xA5A50000);   /* unrelated GPIOs stay intact */
    if ((panel & 2) && !(previous & 2)) {
        shifted = shifted * 2 + ((panel >> 2) & 1);
        clocks++;
    }
    if ((panel & 1) && !(previous & 1))
        latches++;
    previous = panel;
    if (watch_startup) {
        /* The first delay must follow clock, backlight and parameter setup.
         * Unspecified timing words are deliberately left alone. */
        assert(lcd_clock == 1 && lcd_gate == 1);
        assert(bl_gate == 1 && bl_control == 1 && bl_period == 0xF70 && !backlight);
        assert(lcd[0] == 0x20 && lcd[1] == 2 && lcd[2] == 0x50 && lcd[3] == 0xF1);
        assert(lcd[4] == 2 && !lcd[5] && !lcd[7] && lcd[9] == 0x14);
        assert(lcd[6] == 0xDEADBEEF && lcd[13] == 0xDEADBEEF);
        if (!phase_count || lcd[0x48 / 4] != phases[phase_count - 1].control) {
            assert(phase_count < 5);
            phases[phase_count].control = lcd[0x48 / 4];
            phases[phase_count++].at = us;
            if (lcd[0x48 / 4] >= 0x103) {
                assert(lcd[0x2C / 4] == 1);
                assert(clocks == 8 && latches == 1 && shifted == 0xC1);
                assert(!(panel & 7));
            }
        }
    }
    return us++;
}
int gz_display_set(int brightness, int contrast) { return display_set(brightness, contrast); }

static struct display_settings disk, temporary;
static int disk_len = GZ_ENOENT, write_result, rename_error, delete_error;
int pref_get(const char *key, void *buf, uint32_t len)
{
    assert(!strcmp(key, DISPLAY_PREF));
    if (disk_len > 0)
        memcpy(buf, &disk, MIN(len, sizeof(disk)));
    return disk_len;
}
int pref_delete(const char *key)
{
    assert(!strcmp(key, DISPLAY_PREF));
    if (delete_error)
        return delete_error;
    int rv = disk_len < 0 ? GZ_ENOENT : 0;
    disk_len = GZ_ENOENT;
    return rv;
}
int fs_mkdirs(const char *path) { return 0; }
int fs_write_file(const char *path, const void *buf, uint32_t len)
{
    assert(!strcmp(path, PREFS_DIR "/" DISPLAY_PREF ".tmp"));
    assert(len == sizeof(temporary));
    if (write_result)
        return write_result;
    memcpy(&temporary, buf, len);
    return len;
}
int fs_rename(const char *from, const char *to)
{
    assert(!strcmp(from, PREFS_DIR "/" DISPLAY_PREF ".tmp"));
    assert(!strcmp(to, PREFS_DIR "/" DISPLAY_PREF));
    if (rename_error)
        return rename_error;
    disk = temporary;
    disk_len = sizeof(disk);
    return 0;
}

static void driver_tests(void)
{
    /* Values independently recovered from stock ddi.dll's level tables. */
    static const unsigned expected_duty[] = {3339, 2553, 1964, 981, 0};
    for (int b = 1; b <= 5; b++) {
        assert(!display_set(b, 5));
        assert(backlight == expected_duty[b - 1]);
    }
    /* Force a contrast change at each iteration, including the first one. */
    for (int c = 0; c <= 9; c++) {
        clocks = latches = shifted = 0;
        uint64_t start = us;
        assert(!display_set(3, c));
        assert(clocks == 8 && latches == 1);
        assert(shifted == (unsigned)(208 - 3 * c));
        assert((panel & 7) == 0 && us - start >= 95);
        start = us;
        assert(!display_set(3, c));
        assert(us == start);              /* same value never reprograms GPIOs */
    }
    uint32_t duty = backlight;
    uint64_t start = us;
    assert(display_set(0, 5) == GZ_EINVAL);
    assert(display_set(6, 5) == GZ_EINVAL);
    assert(display_set(3, -1) == GZ_EINVAL);
    assert(display_set(3, 10) == GZ_EINVAL);
    assert(backlight == duty && us == start);
    for (int b = 1; b <= 5; b++) {
        assert(!display_set(b, 5));
        uint32_t saved_duty = backlight;
        assert(!display_power(0));
        assert(!bl_control && !(bl_config & 0xf) && bl_gate == 0xA5A50000);
        assert(backlight == saved_duty);  /* zero duty must never mean off */
        assert(!display_power(0));       /* repeated calls preserve the snapshot */
        bl_gate |= 0x10;                 /* another peripheral changed while dark */
        assert(!display_power(1));
        assert(bl_control == 1 && bl_config == 0x202 && bl_gate == 0xA5A50011);
        assert(backlight == saved_duty);
        bl_gate &= ~0x10u;
        assert(!display_power(1));
    }
    assert(display_power(2) == GZ_EINVAL && display_power(-1) == GZ_EINVAL);
    /* Settings changes and preview rollback must not enable a dark display. */
    assert(!display_power(0));
    assert(!display_set(4, 5) && !bl_control && !(bl_config & 0xf));
    assert(!display_power(1) && backlight == 981 && bl_config == 0x202);
    buzz_control=1;
    assert(!display_power(0) && !bl_control && (bl_gate&1) && bl_config==0x202);
    bl_config|=0x400; /* another BLG channel changes while the panel is dark */
    assert(!display_power(1) && bl_control==1 && bl_config==0x602 && (bl_gate&1));
    buzz_control=0;bl_config&=~0x400u;
}

static void service_tests(void)
{
    struct sb_display s;
    sb_display_load(&s);
    assert(s.current.brightness == 3 && s.current.contrast == 5);
    assert(backlight == 1964);
    assert(!sb_display_request(&s, SB_DISPLAY_GET, 7, 0, 0));
    assert(sb_display_request(&s, SB_DISPLAY_PREVIEW, 7, 0, 5) == GZ_EINVAL);
    assert(!s.owner);
    assert(!sb_display_request(&s, SB_DISPLAY_PREVIEW, 7, 5, 9));
    assert(s.owner == 7 && s.current.brightness == 5 && s.current.contrast == 9);
    assert(disk_len == GZ_ENOENT && s.saved.brightness == 3);
    assert(sb_display_request(&s, SB_DISPLAY_CANCEL, 8, 0, 0) == GZ_EPERM);
    assert(!sb_display_request(&s, SB_DISPLAY_CANCEL, 7, 0, 0));
    assert(!s.owner && s.current.brightness == 3 && s.current.contrast == 5);
    assert(backlight == 1964);
    assert(!sb_display_request(&s, SB_DISPLAY_SAVE, 7, 4, 7));
    assert(!s.owner && disk_len == sizeof(disk) && disk.brightness == 4 && disk.contrast == 7);
    sb_display_load(&s);
    assert(s.current.brightness == 4 && s.current.contrast == 7 && backlight == 981);
    for (int fault = 0; fault < 3; fault++) {
        write_result = fault == 0 ? GZ_ENOSPC : fault == 1 ? (int)sizeof(disk) - 1 : 0;
        rename_error = fault == 2 ? GZ_EIO : 0;
        assert(!sb_display_request(&s, SB_DISPLAY_PREVIEW, 7, 1, 0));
        assert(sb_display_request(&s, SB_DISPLAY_SAVE, 7, 1, 0) < 0);
        assert(!s.owner && s.current.brightness == 4 && s.current.contrast == 7);
        assert(disk.brightness == 4 && disk.contrast == 7 && backlight == 981);
    }
    write_result = rename_error = 0;
    assert(!sb_display_request(&s, SB_DISPLAY_PREVIEW, 7, 1, 0));
    sb_display_cancel(&s);        /* Home, lock, deactivation and process exit */
    assert(!s.owner && s.current.brightness == 4 && s.current.contrast == 7);
    delete_error = GZ_EIO;
    assert(sb_display_request(&s, SB_DISPLAY_RESET, 7, 0, 0) == GZ_EIO);
    assert(s.current.brightness == 4);
    delete_error = 0;
    assert(!sb_display_request(&s, SB_DISPLAY_RESET, 7, 0, 0));
    assert(disk_len == GZ_ENOENT && s.current.brightness == 3 && s.current.contrast == 5);
    assert(!sb_display_request(&s, SB_DISPLAY_RESET, 7, 0, 0));
    disk = (struct display_settings){DISPLAY_VERSION, 6, 100};
    disk_len = sizeof(disk);
    sb_display_load(&s);
    assert(s.current.brightness == 3 && s.current.contrast == 5);
    disk = (struct display_settings){DISPLAY_VERSION + 1, 1, 0};
    sb_display_load(&s);
    assert(s.current.brightness == 3 && s.current.contrast == 5);
    disk = (struct display_settings){DISPLAY_VERSION, 1, 0};
    disk_len--;
    sb_display_load(&s);
    assert(s.current.brightness == 3 && s.current.contrast == 5);
    disk_len = sizeof(disk) + 1;
    sb_display_load(&s);
    assert(s.current.brightness == 3 && s.current.contrast == 5);
}

static void startup_test(bool inherit, uint32_t control, bool backlight_on)
{
    for (unsigned i = 0; i < sizeof(lcd) / sizeof(lcd[0]); i++)
        lcd[i] = 0xDEADBEEF;
    lcd[0x48 / 4] = control;
    lcd_clock = lcd_gate = 0;
    bl_control = backlight_on ? 1 : 0;
    bl_gate = backlight_on ? 0xA5A50001 : 0xA5A50000;
    bl_config = 0xA5A5020F;        /* mode bits dirty, unrelated bits retained */
    bl_period = 0x1234;
    backlight = 981;
    panel = previous = 0xA5A50000;
    clocks = latches = shifted = phase_count = 0;
    uint64_t start = us;
    watch_startup = true;
    display_init(inherit);
    time_us();                    /* observe final enable write */
    watch_startup = false;
    static const uint32_t expected[] = {1, 0x101, 0x103, 0x10F, 0x30F};
    assert(phase_count == 5 && us - start >= 22095);
    for (unsigned i = 0; i < phase_count; i++)
        assert(phases[i].control == expected[i]);
    assert(phases[1].at - phases[0].at >= 1000);
    assert(phases[2].at - phases[1].at >= 95); /* eight clocks plus latch */
    assert(phases[3].at - phases[2].at >= 1000);
    assert(phases[4].at - phases[3].at >= 20000);
    assert(bl_config == 0xA5A50202);
    assert(brightness_now == 5 && contrast_now == 5 && powered);
    /* Runtime settings and sleep/wake still work from the initialized state. */
    assert(!display_set(3, 5) && backlight == 1964);
    assert(!display_power(0) && !bl_control && !(bl_config & 0xf) && !bl_gate);
    assert(!display_power(1) && bl_control == 1 && bl_config == 0xA5A50202 && bl_gate == 1);
}

static void startup_tests(void)
{
    startup_test(false, 0, false);      /* actual reset state */
    startup_test(false, 0x30F, true);   /* cold boot must not trust emulator seeds */
    startup_test(true, 0, true);       /* incomplete RAM-handoff panel setup */
    startup_test(true, 0x30F, false);   /* configured panel, disabled backlight */
    startup_test(true, 0x30F, true);    /* configured panel, clock gates disabled */

    /* A live GlazeBoot display, including trim, is left untouched. */
    uint32_t before[sizeof(lcd) / sizeof(lcd[0])];
    for (unsigned i = 0; i < sizeof(before) / sizeof(before[0]); i++)
        before[i] = lcd[i];
    lcd_clock = 0x1235;
    lcd_gate = 0x5679;
    panel = previous = 0xA5A50004;
    uint64_t start = us;
    display_init(true);
    assert(us == start && panel == previous && lcd_clock == 0x1235 && lcd_gate == 0x5679);
    for (unsigned i = 0; i < sizeof(before) / sizeof(before[0]); i++)
        assert(before[i] == lcd[i]);
    assert(backlight == 1964 && bl_period == 0xF70 && bl_config == 0xA5A50202 && bl_gate == 1);
    assert(brightness_now == -1 && contrast_now == -1); /* preferences still apply */
}

int main(void)
{
    driver_tests();
    service_tests();
    startup_tests();
    puts("display: LCD startup, delays, handoff, duty, contrast, persistence and rollback PASS");
    return 0;
}
