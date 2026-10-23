/* Exercise private database catalog persistence and explicit subset limits. */
#define GLAZE_SYS_H
#define GLAZE_THREAD_H
#include "../apps/wince/runtime.h"
#include <assert.h>
struct gz_mutex { int unused; };
#define GZ_MUTEX_INIT {0}
static void gz_mutex_lock(struct gz_mutex *m) { (void)m; }
static void gz_mutex_unlock(struct gz_mutex *m) { (void)m; }
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
    assert(strstr(from, "databases.tmp") && strstr(to, "databases"));
    free(saved);
    saved = staged; saved_size = staged_size; staged = NULL;
    return 0;
}
static const void *db_pointers[64];
static unsigned db_pointer_count;
static CW db_word(const void *p)
{
    if (!p) return 0;
    for (unsigned i = 0; i < db_pointer_count; i++) if (db_pointers[i] == p) return i + 1;
    assert(db_pointer_count < 64);
    db_pointers[db_pointer_count++] = p;
    return db_pointer_count;
}
static void *db_ptr(CW p) { assert(!p || p <= db_pointer_count); return p ? (void *)db_pointers[p - 1] : NULL; }
#undef WORD
#undef PTR
#define WORD(p) db_word((const void *)(p))
#define PTR(t, p) ((t *)db_ptr(p))
CW *ce_error_slot(void) { static CW error; return &error; }
unsigned ce_wlen(const WCHAR *p) { unsigned n = 0; while (p && p[n]) n++; return n; }
#include "../apps/wince/database.c"
static CW api(CW (*fn)(CW,CW,CW,CW,CW,CW,CW,CW,CW,CW,CW,CW), CW a, CW b, CW c, CW d, CW e, CW f)
{ return fn(a,b,c,d,e,f,0,0,0,0,0,0); }
static void cold(void)
{
    memset(databases, 0, sizeof(databases));
    memset(db_handles, 0, sizeof(db_handles));
    db_loaded = false;
}
int main(void)
{
    static const WCHAR name[] = {'A','l','a','r','m','s',0}, lower[] = {'a','l','a','r','m','s',0};
    CW oid = 0, handle, sort[] = {0x10013,0};
    assert(api(ce_CeOpenDatabase, WORD(&oid), WORD(name), 0, 0, 0, 0) == (CW)-1 && ce_error == 2);
    fail_write = true;
    assert(!api(ce_CeCreateDatabase, WORD(name), 123, 1, WORD(sort), 0, 0) && ce_error == 112);
    assert(!saved);
    fail_write = false;
    CW created = api(ce_CeCreateDatabase, WORD(name), 123, 1, WORD(sort), 0, 0);
    assert(created && saved);
    assert(!api(ce_CeCreateDatabase, WORD(lower), 123, 0, 0, 0, 0) && ce_error == 52);
    cold();
    assert(databases[0].oid == 0);
    handle = api(ce_CeOpenDatabase, WORD(&oid), WORD(lower), sort[0], 1, 0, 0);
    assert(handle != (CW)-1 && oid == created);
    assert(databases[created - 1].type == 123 && databases[created - 1].sorts[0] == sort[0]);
    uint16_t count = 5;
    CW buffer = 0, size = 0;
    assert(!api(ce_CeReadRecordProps, handle, 1, WORD(&count), 0, WORD(&buffer), WORD(&size)));
    assert(ce_error == 259 && !count && !buffer);
    assert(!api(ce_CeSeekDatabase, handle, 2, 0, 0, 0, 0) && ce_error == 259);
    bool handled;
    assert(ce_database_close(handle, &handled) && handled);
    assert(!api(ce_CeSeekDatabase, handle, 2, 0, 0, 0, 0) && ce_error == 6);
    assert(!ce_database_close(handle, &handled) && handled && ce_error == 6);
    assert(!ce_database_close(0x72000001, &handled) && !handled);
    assert(api(ce_CeOpenDatabase, WORD(&oid), 0, 0x999, 0, 0, 0) == (CW)-1 && ce_error == 87);
    assert(api(ce_CeOpenDatabase, WORD(&oid), 0, 0, 0, 12, 0) == (CW)-1 && ce_error == 50);
    assert(!api(ce_CeCreateDatabase, WORD(name), 0, 5, WORD(sort), 0, 0) && ce_error == 87);
    free(saved); free(staged);
    puts("WinCE empty database catalogs persist with explicit record/notification limits");
    return 0;
}
