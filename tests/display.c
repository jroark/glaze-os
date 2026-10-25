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
#define DISP_BACKLIGHT backlight
#define DISP_CONTRAST panel
#include "../kernel/drivers/display.c"

static uint64_t us;
static uint32_t previous;
static unsigned shifted, clocks, latches;
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
        write_result = fault == 0 ? GZ_ENOSPC : fault == 1 ? sizeof(disk) - 1 : 0;
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

int main(void)
{
    driver_tests();
    service_tests();
    puts("display: hardware duty, serial contrast, native ranges, persistence and rollback PASS");
    return 0;
}
