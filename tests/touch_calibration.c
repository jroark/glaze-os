/* Production fit, persistence and compositor mapping under ASan/UBSan. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <glaze/fs.h>
#include <glaze/prefs.h>
#include <glaze/sb.h>
#include <glaze/touch.h>
#include "../servers/springboard/calibration.h"

static const struct touch_point targets[TOUCH_POINTS] = {
    {120, 168}, {24, 40}, {216, 40}, {216, 296}, {24, 296},
};
static struct touch_calibration disk, temporary;
static int disk_len = GZ_ENOENT, write_error, rename_error, delete_error;

int pref_get(const char *key, void *buf, uint32_t len)
{
    assert(!strcmp(key, TOUCH_PREF));
    if (disk_len > 0)
        memcpy(buf, &disk, MIN(len, sizeof(disk)));
    return disk_len;
}
int pref_delete(const char *key)
{
    assert(!strcmp(key, TOUCH_PREF));
    if (delete_error)
        return delete_error;
    int rv = disk_len < 0 ? GZ_ENOENT : 0;
    disk_len = GZ_ENOENT;
    return rv;
}
int fs_mkdirs(const char *path) { return 0; }
int fs_write_file(const char *path, const void *buf, uint32_t len)
{
    assert(!strcmp(path, PREFS_DIR "/" TOUCH_PREF ".tmp"));
    assert(len == sizeof(temporary));
    if (write_error)
        return write_error;
    memcpy(&temporary, buf, len);
    return len;
}
int fs_rename(const char *from, const char *to)
{
    assert(!strcmp(from, PREFS_DIR "/" TOUCH_PREF ".tmp"));
    assert(!strcmp(to, PREFS_DIR "/" TOUCH_PREF));
    if (rename_error)
        return rename_error;
    disk = temporary;
    disk_len = sizeof(disk);
    return 0;
}

static void expect_point(const struct touch_calibration *c, int x, int y, int tx, int ty)
{
    struct touch_point p;
    touch_apply(c, x, y, &p);
    assert(p.x >= tx - 1 && p.x <= tx + 1 && p.y >= ty - 1 && p.y <= ty + 1);
}

static void fits(void)
{
    struct touch_calibration c;
    struct touch_point raw[TOUCH_POINTS];
    touch_identity(&c);
    assert(touch_valid(&c));
    assert(touch_fit(&c, targets, targets) == 0);
    expect_point(&c, 239, 319, 239, 319);
    for (int i = 0; i < TOUCH_POINTS; i++)
        raw[i] = (struct touch_point){targets[i].x - 17, targets[i].y + 23};
    assert(touch_fit(&c, raw, targets) == 0);
    expect_point(&c, 100, 200, 117, 177);
    for (int i = 0; i < TOUCH_POINTS; i++)
        raw[i] = (struct touch_point){2 * targets[i].x + targets[i].y / 4 - 100, 2 * targets[i].y + 20};
    assert(touch_fit(&c, raw, targets) == 0);
    for (int i = 0; i < TOUCH_POINTS; i++)
        expect_point(&c, raw[i].x, raw[i].y, targets[i].x, targets[i].y);
    /* Unclipped points near the edge and extrapolation to the screen bounds. */
    for (int i = 0; i < TOUCH_POINTS; i++)
        raw[i] = (struct touch_point){targets[i].x - 80, targets[i].y - 100};
    assert(touch_fit(&c, raw, targets) == 0);
    expect_point(&c, -80, -100, 0, 0);
    expect_point(&c, 159, 219, 239, 319);
    struct touch_calibration before = c;
    raw[0].x += 80;
    assert(touch_fit(&c, raw, targets) == GZ_EINVAL);
    assert(!memcmp(&before, &c, sizeof(c)));
    for (int i = 0; i < TOUCH_POINTS; i++)
        raw[i] = (struct touch_point){i * 10, i * 20};
    assert(touch_fit(&c, raw, targets) == GZ_EINVAL);
    memset(raw, 0, sizeof(raw));
    assert(touch_fit(&c, raw, targets) == GZ_EINVAL);
    raw[0].x = 32767;
    assert(touch_fit(&c, raw, targets) == GZ_EINVAL);
    touch_identity(&c);
    c.version++;
    assert(!touch_valid(&c));
    touch_identity(&c);
    c.x[0] = -TOUCH_ONE;
    assert(!touch_valid(&c));
    touch_identity(&c);
    c.y[2] = INT32_MAX;
    assert(!touch_valid(&c));
}

static void sessions(void)
{
    struct sb_calibration s = {0};
    struct touch_calibration candidate;
    touch_identity(&candidate);
    candidate.x[2] = 20 * TOUCH_ONE;
    candidate.y[2] = -10 * TOUCH_ONE;
    sb_calibration_load(&s);
    expect_point(&s.saved, 10, 20, 10, 20);
    assert(!s.owner);
    struct gz_input_event ev = { .type = GZ_EV_TOUCH_DOWN, .x = -20, .y = 359 };
    assert(!sb_calibration_input(&s, &ev));
    assert(ev.x == 0 && ev.y == 319);   /* factory icon strip behavior */
    assert(!sb_calibration_request(&s, SB_TOUCH_BEGIN, 7, NULL, 100));
    ev.x = -20; ev.y = 359;
    assert(sb_calibration_input(&s, &ev));
    assert(ev.x == -20 && ev.y == 359); /* uncorrected sampling bypass */
    assert(sb_calibration_request(&s, SB_TOUCH_CANCEL, 8, NULL, 100) == GZ_EPERM);
    assert(sb_calibration_request(&s, SB_TOUCH_PREVIEW, 8, &candidate, 100) < 0);
    assert(!sb_calibration_request(&s, SB_TOUCH_PREVIEW, 7, &candidate, 100));
    ev.x = 100; ev.y = 100;
    assert(!sb_calibration_input(&s, &ev));
    assert(ev.x == 120 && ev.y == 90); /* full screen before app status offset */
    ev = (struct gz_input_event){.type = GZ_EV_KEY_DOWN, .code = GZ_KEY_HOME, .x = 13, .y = 18};
    assert(!sb_calibration_input(&s, &ev));
    assert(ev.code == GZ_KEY_HOME && ev.x == 13 && ev.y == 18);
    assert(!sb_calibration_expire(&s, 100 + TOUCH_PREVIEW_MS - 1));
    assert(sb_calibration_expire(&s, 100 + TOUCH_PREVIEW_MS));
    assert(!s.owner && !s.preview);
    expect_point(&s.saved, 10, 20, 10, 20);
    assert(sb_calibration_request(&s, SB_TOUCH_SAVE, 7, NULL, 30100) < 0);
    assert(!sb_calibration_request(&s, SB_TOUCH_BEGIN, 7, NULL, 0));
    assert(!sb_calibration_request(&s, SB_TOUCH_PREVIEW, 7, &candidate, 0));
    write_error = GZ_ENOSPC;
    assert(sb_calibration_request(&s, SB_TOUCH_SAVE, 7, NULL, 1) == GZ_ENOSPC);
    assert(s.owner && s.preview && disk_len == GZ_ENOENT);
    write_error = sizeof(candidate) - 1; /* short writes cannot replace saved record */
    assert(sb_calibration_request(&s, SB_TOUCH_SAVE, 7, NULL, 1) == GZ_EIO);
    write_error = 0;
    rename_error = GZ_EIO;
    assert(sb_calibration_request(&s, SB_TOUCH_SAVE, 7, NULL, 1) == GZ_EIO);
    assert(disk_len == GZ_ENOENT);
    rename_error = 0;
    assert(!sb_calibration_request(&s, SB_TOUCH_SAVE, 7, NULL, 1));
    assert(!s.owner && !memcmp(&s.saved, &candidate, sizeof(candidate)));
    sb_calibration_load(&s);          /* restart restores the saved mapping */
    assert(!memcmp(&s.saved, &candidate, sizeof(candidate)));
    assert(!sb_calibration_request(&s, SB_TOUCH_BEGIN, 7, NULL, 0));
    assert(!sb_calibration_request(&s, SB_TOUCH_PREVIEW, 7, &candidate, 0));
    sb_calibration_cancel(&s);        /* Home, lock, backgrounding and death */
    assert(!s.owner && !s.preview);
    assert(!memcmp(&s.saved, &candidate, sizeof(candidate)));
    delete_error = GZ_EIO;
    assert(sb_calibration_request(&s, SB_TOUCH_RESET, 7, NULL, 0) == GZ_EIO);
    assert(!memcmp(&s.saved, &candidate, sizeof(candidate)));
    delete_error = 0;
    assert(!sb_calibration_request(&s, SB_TOUCH_RESET, 7, NULL, 0));
    expect_point(&s.saved, 10, 20, 10, 20);
    assert(disk_len == GZ_ENOENT);
    assert(!sb_calibration_request(&s, SB_TOUCH_RESET, 7, NULL, 0));
    disk = candidate; disk.version++;
    disk_len = sizeof(disk);
    sb_calibration_load(&s);
    expect_point(&s.saved, 10, 20, 10, 20);
    disk = candidate; disk_len--;
    sb_calibration_load(&s);
    expect_point(&s.saved, 10, 20, 10, 20);
    disk_len = sizeof(disk) + 1;
    sb_calibration_load(&s);
    expect_point(&s.saved, 10, 20, 10, 20);
    assert(!sb_calibration_request(&s, SB_TOUCH_BEGIN, 7, NULL, 0));
    assert(!sb_calibration_request(&s, SB_TOUCH_PREVIEW, 7, &candidate, UINT32_MAX - 100));
    assert(!sb_calibration_expire(&s, 5));
    assert(sb_calibration_expire(&s, TOUCH_PREVIEW_MS - 101)); /* timer wraps */
}

int main(void)
{
    fits();
    sessions();
    puts("touch calibration: affine fitting, mapping, persistence, failures and session lifecycle PASS");
    return 0;
}
