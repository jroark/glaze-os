/* Exercise timestamp durability and failure handling independently of CE apps. */
#define GLAZE_SYS_H
#define GLAZE_THREAD_H
#include "../apps/wince/runtime.h"
#include <glaze/notify.h>
#include <assert.h>
struct gz_mutex { int unused; };
#define GZ_MUTEX_INIT {0}
static void gz_mutex_lock(struct gz_mutex *m) { (void)m; }
static void gz_mutex_unlock(struct gz_mutex *m) { (void)m; }
static struct gz_datetime test_now = { .year = 2026, .month = 10, .day = 21, .hour = 12 };
static int gz_rtc_get(struct gz_datetime *out) { *out = test_now; return 0; }
int tz_current(void) { return 0; }
uint32_t tz_local_to_utc(int zone, uint32_t t) { (void)zone; return t; }
const char *res_bundle(void) { return "/fixture.app"; }
void ce_fatal(const char *message) { fprintf(stderr, "%s\n", message); abort(); }
static uint8_t *saved, *staged;
static unsigned saved_size, staged_size;
static bool fail_write;
int fs_stat(const char *path, struct fs_stat *out)
{
    (void)path;
    if (!saved) return GZ_ENOENT;
    *out = (struct fs_stat){FS_FILE, saved_size};
    return 0;
}
int fs_read_file(const char *path, void *out, uint32_t size)
{
    (void)path;
    if (!saved || size < saved_size) return GZ_EINVAL;
    memcpy(out, saved, saved_size);
    return saved_size;
}
int fs_mkdirs(const char *path) { (void)path; return 0; }
int fs_write_file(const char *path, const void *data, uint32_t size)
{
    (void)path;
    if (fail_write) return GZ_ENOSPC;
    free(staged);
    staged = malloc(size);
    assert(staged);
    memcpy(staged, data, size);
    staged_size = size;
    return size;
}
int fs_rename(const char *from, const char *to)
{
    assert(strstr(from, "filetimes.tmp") && strstr(to, "filetimes"));
    free(saved);
    saved = staged; saved_size = staged_size; staged = NULL;
    return 0;
}
#include "../apps/wince/filemeta.c"
static void cold_boot(void)
{
    memset(metadata, 0, sizeof(metadata));
    meta_loaded = false;
    meta_dirty = false;
}
int main(void)
{
    uint8_t first[24], after[24], explicit[24];
    const char *path = "/fixture.app/data/drive/ToDo/00000001";
    assert(ce_file_times(path, first, false));
    for (unsigned i = 0; i < sizeof(first); i++) assert(!first[i]);
    assert(ce_file_touch(path, true));
    assert(ce_file_times(path, first, false));
    assert(ce_u32(first) || ce_u32(first + 4));
    assert(!memcmp(first, first + 8, 8) && !memcmp(first, first + 16, 8));
    test_now.second++;
    assert(ce_file_touch(path, false));
    assert(!saved); /* Automatic timestamps are cached until file close. */
    fail_write = true;
    assert(!ce_file_flush_times());
    assert(ce_file_times(path, after, false) && memcmp(first + 16, after + 16, 8));
    fail_write = false;
    assert(ce_file_flush_times());
    cold_boot();
    assert(ce_file_times(path, after, false));
    assert(!memcmp(first, after, 8) && memcmp(first + 16, after + 16, 8));
    memcpy(explicit, after, 24);
    ce_put32(explicit + 16, 12345);
    assert(ce_file_set_times(path, explicit));
    cold_boot();
    assert(ce_file_times(path, after, false) && !memcmp(explicit, after, 24));
    const char *moved = "/fixture.app/data/drive/ToDo/renamed";
    assert(ce_file_move_times(path, moved));
    cold_boot();
    assert(ce_file_times(moved, after, false) && !memcmp(explicit, after, 24));
    assert(ce_file_times(path, after, false) && !ce_u32(after + 16));
    fail_write = true;
    uint8_t lost[24] = {1};
    assert(!ce_file_set_times(moved, lost));
    assert(ce_file_times(moved, after, false) && !memcmp(explicit, after, 24));
    cold_boot();
    assert(ce_file_times(moved, after, false) && !memcmp(explicit, after, 24));
    fail_write = false;
    assert(ce_file_move_times(moved, NULL));
    cold_boot();
    assert(ce_file_times(moved, after, false) && !ce_u32(after + 16));
    free(saved); free(staged);
    puts("WinCE file timestamps survive cold boots, rename and failed saves");
    return 0;
}
