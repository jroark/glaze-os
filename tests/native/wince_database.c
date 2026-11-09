/* CE object-store databases: the dbstore model natively, then the CE API
 * glue (apps/wince/database.c) with guest pointers mapped to small words. */
#define GLAZE_SYS_H
#define GLAZE_THREAD_H
#include "../../apps/wince/runtime.h"
#include "../../apps/wince/dbstore.h"
#include <assert.h>
struct gz_mutex { int unused; };
#define GZ_MUTEX_INIT {0}
static void gz_mutex_lock(struct gz_mutex *m) { (void)m; }
static void gz_mutex_unlock(struct gz_mutex *m) { (void)m; }
const char *res_bundle(void) { return "/fixture.app"; }
static bool shared_mode;
bool ce_source_active(void) { return shared_mode; }
void ce_fatal(const char *message) { fprintf(stderr, "%s\n", message); abort(); }

/* A small file system: a few named files. */
static struct { char path[FS_PATH_MAX]; uint8_t *data; unsigned size; } files[8];
static bool fail_write;
static int file_slot(const char *path, bool create)
{
    for (unsigned i = 0; i < 8; i++) if (files[i].data && !strcmp(files[i].path, path)) return (int)i;
    if (!create) return -1;
    for (unsigned i = 0; i < 8; i++) if (!files[i].data) { strcpy(files[i].path, path); return (int)i; }
    abort();
}
int fs_stat(const char *path, struct fs_stat *out)
{
    int i = file_slot(path, false);
    if (i < 0) return GZ_ENOENT;
    *out = (struct fs_stat){FS_FILE, files[i].size};
    return 0;
}
int fs_read_file(const char *path, void *out, uint32_t size)
{
    int i = file_slot(path, false);
    if (i < 0) return GZ_ENOENT;
    if (size < files[i].size) return GZ_EINVAL;
    memcpy(out, files[i].data, files[i].size);
    return (int)files[i].size;
}
int fs_mkdirs(const char *path) { (void)path; return 0; }
int fs_remove(const char *path)
{
    int i = file_slot(path, false);
    if (i >= 0) { free(files[i].data); files[i].data = NULL; }
    return 0;
}
int fs_write_file(const char *path, const void *data, uint32_t size)
{
    if (fail_write) return GZ_ENOSPC;
    fs_remove(path);
    int i = file_slot(path, true);
    files[i].data = malloc(size ? size : 1);
    memcpy(files[i].data, data, size);
    files[i].size = size;
    return (int)size;
}
int fs_rename(const char *from, const char *to)
{
    int i = file_slot(from, false);
    assert(i >= 0);
    fs_remove(to);
    strcpy(files[i].path, to);
    return 0;
}

/* Guest words for host pointers, as the 32-bit runtime has natively. */
static const void *pointers[4096];
static unsigned pointer_count;
static CW db_word(const void *p)
{
    if (!p) return 0;
    for (unsigned i = 0; i < pointer_count; i++) if (pointers[i] == p) return i + 1;
    assert(pointer_count < 4096);
    pointers[pointer_count++] = p;
    return pointer_count;
}
static void *db_ptr(CW p) { assert(!p || p <= pointer_count); return p ? (void *)pointers[p - 1] : NULL; }
#undef WORD
#undef PTR
#define WORD(p) db_word((const void *)(p))
#define PTR(t, p) ((t *)db_ptr(p))
CW *ce_error_slot(void) { static CW error; return &error; }
unsigned ce_wlen(const WCHAR *p) { unsigned n = 0; while (p && p[n]) n++; return n; }
CE_API(LocalAlloc) { (void)a0; void *p = calloc(1, a1 + 4); *(uint32_t *)p = a1; return WORD((uint8_t *)p + 4); }
CE_API(LocalSize) { return *(uint32_t *)((uint8_t *)db_ptr(a0) - 4); }
CE_API(LocalReAlloc)
{
    uint8_t *old = (uint8_t *)db_ptr(a0) - 4, *p = calloc(1, a1 + 4);
    memcpy(p + 4, old + 4, *(uint32_t *)old);
    *(uint32_t *)p = a1;
    return WORD(p + 4);
}
CE_API(GetSystemTime) { memset(db_ptr(a0), 0, 16); return 0; }
CE_API(SystemTimeToFileTime) { memset(db_ptr(a1), 0x11, 8); return 1; }
#include "../../apps/wince/database.c"

static CW api(CW (*fn)(CW,CW,CW,CW,CW,CW,CW,CW,CW,CW,CW,CW), CW a, CW b, CW c, CW d, CW e, CW f)
{ return fn(a,b,c,d,e,f,0,0,0,0,0,0); }
static void cold(void)
{
    ce_db_store_clear(&store);
    for (unsigned i = 0; i < HANDLES; i++) { free(handles[i].order); memset(&handles[i], 0, sizeof(handles[i])); }
    loaded = dirty = false;
}
static const uint16_t *wstr(const char *s)
{
    static uint16_t buf[8][64];
    static unsigned next;
    uint16_t *w = buf[next++ % 8];
    unsigned i = 0;
    for (; s[i]; i++) w[i] = (uint8_t)s[i];
    w[i] = 0;
    return w;
}
#define NAME_ID (0x1000u << 16 | CEVT_LPWSTR)
#define AGE_ID (0x1001u << 16 | CEVT_I4)
#define WHEN_ID (0x1002u << 16 | CEVT_FILETIME)

static void store_model(void)
{
    struct ce_db_store s = {0};
    uint32_t specs[2][2] = {{NAME_ID, CEDB_SORT_CASEINSENSITIVE}, {AGE_ID, CEDB_SORT_DESCENDING | CEDB_SORT_UNKNOWNFIRST}};
    uint32_t oid, again, r[4];
    assert(!ce_db_create(&s, wstr("People"), 7, 2, (const uint32_t(*)[2])specs, &oid));
    assert(ce_db_create(&s, wstr("PEOPLE"), 7, 0, NULL, &again) == 183);
    struct ce_db *db = ce_db_find(&s, 0, wstr("people"));
    assert(db && db->oid == oid);
    const char *names[] = {"carol", "Alice", "bob", "dave"};
    int ages[] = {30, 41, 25, -1};
    for (unsigned i = 0; i < 4; i++) {
        uint8_t age[8] = {0};
        ce_put32(age, (uint32_t)ages[i]);
        const uint16_t *n = wstr(names[i]);
        struct ce_db_prop props[2] = {{NAME_ID, (ce_wlen(n) + 1) * 2, (uint8_t *)n}, {AGE_ID, 8, age}};
        assert(!ce_db_write(&s, db, 0, props, ages[i] < 0 ? 1 : 2, &r[i]));
    }
    unsigned order[4];
    ce_db_sort(db, NAME_ID, order); /* case-insensitive: Alice, bob, carol, dave */
    assert(order[0] == 1 && order[1] == 2 && order[2] == 0 && order[3] == 3);
    ce_db_sort(db, AGE_ID, order);  /* unknown first, then descending: dave, 41, 30, 25 */
    assert(order[0] == 3 && order[1] == 1 && order[2] == 0 && order[3] == 2);
    ce_db_sort(db, 0, order);
    assert(order[0] == 0 && order[3] == 3);
    /* Seeks by value in name order. */
    ce_db_sort(db, NAME_ID, order);
    const uint16_t *key = wstr("BOB");
    struct ce_db_prop k = {NAME_ID, (ce_wlen(key) + 1) * 2, (uint8_t *)key};
    assert(ce_db_seek(db, NAME_ID, order, 0, CEDB_SEEK_VALUEFIRSTEQUAL, 0, &k) == 1);
    assert(ce_db_seek(db, NAME_ID, order, 0, CEDB_SEEK_VALUEGREATER, 0, &k) == 2);
    assert(ce_db_seek(db, NAME_ID, order, 0, CEDB_SEEK_VALUESMALLER, 0, &k) == 0);
    assert(ce_db_seek(db, NAME_ID, order, 1, CEDB_SEEK_VALUENEXTEQUAL, 0, &k) == -1);
    assert(ce_db_seek(db, NAME_ID, order, 0, CEDB_SEEK_END, 0, NULL) == 3);
    assert(ce_db_seek(db, NAME_ID, order, 0, CEDB_SEEK_END, (uint32_t)-1, NULL) == 2);
    assert(ce_db_seek(db, NAME_ID, order, 2, CEDB_SEEK_CURRENT, (uint32_t)-2, NULL) == 0);
    assert(ce_db_seek(db, NAME_ID, order, 0, CEDB_SEEK_BEGINNING, 4, NULL) == -1);
    assert(ce_db_seek(db, NAME_ID, order, 0, CEDB_SEEK_CEOID, r[3], NULL) == 3);
    /* Merge, delete a property, delete a record. */
    uint8_t when[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    struct ce_db_prop update[2] = {{WHEN_ID, 8, when}, {AGE_ID, CE_DB_DELETE, NULL}};
    assert(!ce_db_write(&s, db, r[0], update, 2, &again) && again == r[0]);
    assert(ce_db_prop(&db->records[0], WHEN_ID) && !ce_db_prop(&db->records[0], AGE_ID));
    struct ce_db_prop bad = {0x2000u << 16 | 99, 8, when};
    assert(ce_db_write(&s, db, r[0], &bad, 1, &again) == 87);
    assert(ce_db_write(&s, db, 0x12345, update, 1, &again) == 87);
    assert(!ce_db_delete_record(db, r[1]) && db->count == 3 && ce_db_index(db, r[1]) < 0);
    int record;
    assert(ce_db_owner(&s, r[2], &record) == db && record == 1);
    /* The file round-trips; damaged files are rejected. */
    uint32_t size;
    uint8_t *file = ce_db_encode(&s, &size);
    struct ce_db_store t = {0};
    assert(file && ce_db_decode(&t, file, size));
    struct ce_db *copy = ce_db_find(&t, oid, NULL);
    assert(copy && copy->count == 3 && copy->sort_count == 2 && t.next_oid == s.next_oid);
    assert(!memcmp(ce_db_prop(&copy->records[0], WHEN_ID)->data, when, 8));
    for (uint32_t cut = 0; cut < size; cut += 7) assert(!ce_db_decode(&t, file, cut));
    file[size - 1] ^= 0xff; /* a string's last byte: still well-formed */
    assert(ce_db_decode(&t, file, size));
    ce_put32(file + 8, 999);
    assert(!ce_db_decode(&t, file, size) && ce_db_find(&t, oid, NULL)); /* unchanged on failure */
    free(file);
    assert(!ce_db_delete(&s, oid) && !ce_db_find(&s, oid, NULL));
    ce_db_store_clear(&s);
    ce_db_store_clear(&t);
}

/* CEPROPVAL helpers for the API test. */
static void propval(uint8_t *p, uint32_t id, const void *value, unsigned flags)
{
    memset(p, 0, 16);
    ce_put32(p, id);
    ce_put16(p + 6, (uint16_t)flags);
    if ((id & 0xffff) == CEVT_LPWSTR) ce_put32(p + 8, WORD(value));
    else if (value) memcpy(p + 8, value, 8);
}
static void api_glue(void)
{
    static const WCHAR name[] = {'A','l','a','r','m','s',0}, lower[] = {'a','l','a','r','m','s',0};
    CW oid = 0, sort[] = {NAME_ID, CEDB_SORT_CASEINSENSITIVE};
    assert(api(ce_CeOpenDatabase, WORD(&oid), WORD(name), 0, 0, 0, 0) == (CW)-1 && ce_error == 2);
    fail_write = true; /* the store stays in memory; saving fails quietly */
    CW created = api(ce_CeCreateDatabase, WORD(name), 123, 1, WORD(sort), 0, 0);
    assert(created);
    fail_write = false;
    assert(!api(ce_CeCreateDatabase, WORD(lower), 123, 0, 0, 0, 0) && ce_error == 52);
    assert(!api(ce_CeCreateDatabase, WORD(name), 0, 5, WORD(sort), 0, 0) && ce_error == 87);
    CW h = api(ce_CeOpenDatabase, WORD(&oid), WORD(lower), NAME_ID, 1, 0x1234, 0);
    assert(h != (CW)-1 && oid == created);
    assert(api(ce_CeOpenDatabase, WORD(&oid), 0, 0x999, 0, 0, 0) == (CW)-1 && ce_error == 87);
    /* Three records; reading follows the name order. */
    const char *names[] = {"zulu", "Alpha", "mike"};
    CW recs[3];
    for (unsigned i = 0; i < 3; i++) {
        uint8_t vals[32], n[8] = {0};
        ce_put32(n, i * 10);
        propval(vals, NAME_ID, wstr(names[i]), 0);
        propval(vals + 16, AGE_ID, n, 0);
        recs[i] = api(ce_CeWriteRecordProps, h, 0, 2, WORD(vals), 0, 0);
        assert(recs[i]);
    }
    uint8_t seek[16];
    propval(seek, NAME_ID, wstr("MIKE"), 0);
    CW index = 99;
    assert(api(ce_CeSeekDatabase, h, CEDB_SEEK_VALUEFIRSTEQUAL, WORD(seek), WORD(&index), 0, 0) == recs[2] && index == 1);
    assert(api(ce_CeSeekDatabase, h, CEDB_SEEK_BEGINNING, 0, 0, 0, 0) == recs[1]);
    /* CEDB_ALLOCATE: all properties, then auto-increment to the next record. */
    uint16_t count = 0;
    CW buffer = 0, size = 0;
    assert(api(ce_CeReadRecordProps, h, 1, WORD(&count), 0, WORD(&buffer), WORD(&size)) == recs[1]);
    assert(count == 2 && buffer && size >= 32 + 12);
    uint8_t *b = db_ptr(buffer);
    assert(ce_u32(b) == NAME_ID && !memcmp(db_ptr(ce_u32(b + 8)), wstr("Alpha"), 12));
    /* A caller's buffer that is too small; then a requested missing property. */
    CW props[2] = {AGE_ID, WHEN_ID};
    uint8_t small[8];
    CW smallp = WORD(small), smallsize = sizeof(small);
    count = 2;
    assert(!api(ce_CeReadRecordProps, h, 0, WORD(&count), WORD(props), WORD(&smallp), WORD(&smallsize)));
    assert(ce_error == 122 && smallsize == 32);
    uint8_t big[64];
    CW bigp = WORD(big), bigsize = sizeof(big);
    assert(api(ce_CeReadRecordProps, h, 0, WORD(&count), WORD(props), WORD(&bigp), WORD(&bigsize)) == recs[2]);
    assert(ce_u32(big + 8) == 20 && ce_u16(big + 16 + 6) == CEDB_PROPNOTFOUND);
    /* The position follows its record when an earlier one is deleted. */
    assert(api(ce_CeDeleteRecord, h, recs[1], 0, 0, 0, 0));
    count = 1;
    CW only[1] = {NAME_ID};
    assert(api(ce_CeReadRecordProps, h, 0, WORD(&count), WORD(only), WORD(&bigp), WORD(&bigsize)) == recs[0]);
    assert(!api(ce_CeReadRecordProps, h, 0, WORD(&count), WORD(only), WORD(&bigp), WORD(&bigsize)) && ce_error == 259);
    /* Object information. */
    uint8_t info[4 + 540];
    assert(api(ce_CeOidGetInfo, created, WORD(info), 0, 0, 0, 0) && ce_u16(info) == 3);
    assert(ce_u16(info + 4 + 72) == 2 && ce_u32(info + 4 + 68) == 123);
    assert(api(ce_CeOidGetInfo, recs[0], WORD(info), 0, 0, 0, 0) && ce_u16(info) == 4 && ce_u32(info + 4) == created);
    assert(!api(ce_CeOidGetInfo, 0x7777, WORD(info), 0, 0, 0, 0));
    /* Saved on close; a cold load (another process) sees the records. */
    bool handled;
    assert(ce_database_close(h, &handled) && handled);
    assert(!ce_database_close(h, &handled) && handled && ce_error == 6);
    assert(!ce_database_close(0x72000001, &handled) && !handled);
    cold();
    oid = created;
    h = api(ce_CeOpenDatabase, WORD(&oid), 0, 0, 0, 0, 0);
    assert(h != (CW)-1);
    assert(api(ce_CeSeekDatabase, h, CEDB_SEEK_END, 0, 0, 0, 0) == recs[0]);
    /* Enumeration and deletion. */
    CW find = api(ce_CeFindFirstDatabase, 123, 0, 0, 0, 0, 0);
    assert(api(ce_CeFindNextDatabase, find, 0, 0, 0, 0, 0) == created);
    assert(!api(ce_CeFindNextDatabase, find, 0, 0, 0, 0, 0) && ce_error == 18);
    assert(ce_database_close(find, &handled) && handled);
    assert(api(ce_CeDeleteDatabase, created, 0, 0, 0, 0, 0));
    assert(!api(ce_CeSeekDatabase, h, CEDB_SEEK_BEGINNING, 0, 0, 0, 0) && ce_error == 6);
    ce_database_close(h, &handled);
    cold();
    oid = 0;
    assert(api(ce_CeOpenDatabase, WORD(&oid), WORD(name), 0, 0, 0, 0) == (CW)-1);
    /* A packaged app's catalog-only CDB1 file is imported. */
    uint8_t legacy[8 + 16 * 108] = {0};
    memcpy(legacy, "CDB1", 4);
    ce_put32(legacy + 4, 16);
    ce_put32(legacy + 8, 1);
    ce_put32(legacy + 12, 55);
    for (unsigned i = 0; name[i]; i++) ce_put16(legacy + 8 + 44 + i * 2, name[i]);
    fs_remove("/fixture.app/data/databases.cdb");
    fs_write_file("/fixture.app/data/databases", legacy, sizeof(legacy));
    cold();
    oid = 0;
    h = api(ce_CeOpenDatabase, WORD(&oid), WORD(name), 0, 0, 0, 0);
    assert(h != (CW)-1 && oid);
    ce_database_close(h, &handled);
    cold();
}

int main(void)
{
    store_model();
    api_glue();
    for (unsigned i = 0; i < 8; i++) free(files[i].data);
    puts("WinCE databases: records, sort orders, seeks, persistence");
    return 0;
}
