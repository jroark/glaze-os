/* littlefs has no wall-clock timestamps. Keep CE file times in a private,
 * versioned sidecar; original file bytes and native Glaze files stay intact. */
#include "datetime.h"
#include "runtime.h"
#include <glaze/notify.h>
#include <glaze/thread.h>
#include <glaze/tz.h>

#define META_FILES 256
#define META_ENTRY (FS_PATH_MAX + 24)
struct ce_file_metadata {
    char path[FS_PATH_MAX];
    uint8_t times[24];
};
static struct ce_file_metadata metadata[META_FILES];
static bool meta_loaded, meta_dirty;
static struct gz_mutex meta_lock = GZ_MUTEX_INIT;

static void meta_path(char out[FS_PATH_MAX], bool temporary)
{
    snprintf(out, FS_PATH_MAX, "%s/data/filetimes%s", res_bundle(), temporary ? ".tmp" : "");
}
static void meta_load(void)
{
    if (meta_loaded)
        return;
    meta_loaded = true;
    char path[FS_PATH_MAX];
    meta_path(path, false);
    struct fs_stat st;
    if (fs_stat(path, &st))
        return;
    uint32_t max = 8 + META_FILES * META_ENTRY;
    if (st.size < 8 || st.size > max)
        ce_fatal("Invalid CE file timestamps");
    uint8_t *data = malloc(st.size);
    if (!data || fs_read_file(path, data, st.size) != (int)st.size)
        ce_fatal("Cannot read CE file timestamps");
    unsigned count = ce_u32(data + 4);
    if (memcmp(data, "CFT1", 4) || count > META_FILES || st.size != 8 + count * META_ENTRY)
        ce_fatal("Invalid CE file timestamps");
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *entry = data + 8 + i * META_ENTRY;
        if (!entry[0] || !memchr(entry, 0, FS_PATH_MAX))
            ce_fatal("Invalid CE timestamp path");
        memcpy(metadata[i].path, entry, FS_PATH_MAX);
        memcpy(metadata[i].times, entry + FS_PATH_MAX, 24);
    }
    free(data);
}
static int meta_find(const char *path, bool create)
{
    int empty = -1;
    for (unsigned i = 0; i < META_FILES; i++) {
        if (!metadata[i].path[0])
            empty = (int)i;
        else if (!strcmp(metadata[i].path, path))
            return (int)i;
    }
    if (create && empty >= 0)
        strlcpy(metadata[empty].path, path, FS_PATH_MAX);
    return create ? empty : -1;
}
static bool meta_save(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < META_FILES; i++)
        count += metadata[i].path[0] != 0;
    unsigned size = 8 + count * META_ENTRY, pos = 8;
    uint8_t *data = calloc(1, size);
    if (!data)
        return false;
    memcpy(data, "CFT1", 4);
    ce_put32(data + 4, count);
    for (unsigned i = 0; i < META_FILES; i++)
        if (metadata[i].path[0]) {
            memcpy(data + pos, metadata[i].path, FS_PATH_MAX);
            memcpy(data + pos + FS_PATH_MAX, metadata[i].times, 24);
            pos += META_ENTRY;
        }
    char path[FS_PATH_MAX], temporary[FS_PATH_MAX], folder[FS_PATH_MAX];
    meta_path(path, false);
    meta_path(temporary, true);
    snprintf(folder, sizeof(folder), "%s/data", res_bundle());
    bool ok = !fs_mkdirs(folder) && fs_write_file(temporary, data, size) == (int)size &&
              !fs_rename(temporary, path);
    free(data);
    if (ok)
        meta_dirty = false;
    return ok;
}
bool ce_file_flush_times(void)
{
    gz_mutex_lock(&meta_lock);
    bool ok = !meta_dirty || meta_save();
    gz_mutex_unlock(&meta_lock);
    return ok;
}
bool ce_file_times(const char *path, uint8_t out[24], bool create)
{
    gz_mutex_lock(&meta_lock);
    meta_load();
    int index = meta_find(path, create);
    if (index >= 0)
        memcpy(out, metadata[index].times, 24);
    else
        memset(out, 0, 24); /* Files predating this service have unknown times. */
    gz_mutex_unlock(&meta_lock);
    return index >= 0 || !create;
}
bool ce_file_set_times(const char *path, const uint8_t times[24])
{
    gz_mutex_lock(&meta_lock);
    meta_load();
    bool existed = meta_find(path, false) >= 0;
    int index = meta_find(path, true);
    bool ok = index >= 0;
    if (ok) {
        struct ce_file_metadata previous = metadata[index];
        if (existed && !memcmp(previous.times, times, 24) && !meta_dirty) {
            gz_mutex_unlock(&meta_lock);
            return true;
        }
        memcpy(metadata[index].times, times, 24);
        ok = meta_save();
        if (!ok) {
            metadata[index] = previous;
            if (!existed)
                memset(&metadata[index], 0, sizeof(metadata[index]));
        }
    }
    gz_mutex_unlock(&meta_lock);
    return ok;
}
bool ce_file_touch(const char *path, bool created)
{
    struct gz_datetime dt;
    if (gz_rtc_get(&dt))
        return false;
    gz_secs_to_dt(tz_local_to_utc(tz_current(), gz_dt_to_secs(&dt)), &dt);
    uint16_t st[] = {dt.year, dt.month, dt.weekday, dt.day, dt.hour, dt.minute, dt.second, 0};
    uint64_t ticks;
    if (!ce_datetime_ticks(st, &ticks))
        return false;
    uint8_t now[8];
    ce_put32(now, (uint32_t)ticks);
    ce_put32(now + 4, (uint32_t)(ticks >> 32));
    /* Buffer automatic write times like the file's own cached data. Original
     * CRT streams can issue thousands of tiny writes; rewriting the sidecar
     * for each one makes registration prohibitively expensive. */
    gz_mutex_lock(&meta_lock);
    meta_load();
    int index = meta_find(path, true);
    if (index >= 0) {
        uint8_t *times = metadata[index].times;
        if (created) {
            memcpy(times, now, 8);
            meta_dirty = true;
        }
        if (memcmp(times + 8, now, 8) || memcmp(times + 16, now, 8)) {
            memcpy(times + 8, now, 8);
            memcpy(times + 16, now, 8);
            meta_dirty = true;
        }
    }
    gz_mutex_unlock(&meta_lock);
    return index >= 0;
}
bool ce_file_move_times(const char *source, const char *destination)
{
    gz_mutex_lock(&meta_lock);
    meta_load();
    int index = meta_find(source, false), old = destination ? meta_find(destination, false) : -1;
    bool ok = true;
    if (index >= 0) {
        struct ce_file_metadata previous = metadata[index], replaced = {0};
        if (old >= 0)
            replaced = metadata[old];
        if (old >= 0 && old != index)
            memset(&metadata[old], 0, sizeof(metadata[old]));
        if (destination)
            strlcpy(metadata[index].path, destination, FS_PATH_MAX);
        else
            memset(&metadata[index], 0, sizeof(metadata[index]));
        ok = meta_save();
        if (!ok) {
            metadata[index] = previous;
            if (old >= 0)
                metadata[old] = replaced;
        }
    }
    gz_mutex_unlock(&meta_lock);
    return ok;
}
