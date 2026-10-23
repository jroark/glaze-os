/* Private CE database catalog. These databases are empty until record-writing
 * support is added; no record-write API is exported. This lets applications
 * inspect/create their alarm catalogs without inventing system alarm records.
 * CeOpenDatabase / CeCreateDatabase contracts:
 * https://learn.microsoft.com/en-us/previous-versions/ms933221(v=msdn.10)
 * https://learn.microsoft.com/en-us/previous-versions/windows/embedded/aa517213(v=msdn.10)
 */
#include "runtime.h"
#include <glaze/thread.h>
#define DATABASES 16
#define DB_ENTRY 108u
static struct {
    CW oid, type, count, sorts[8];
    WCHAR name[32];
} databases[DATABASES];
static struct { CW handle; unsigned database; } db_handles[16];
static CW db_serial;
static bool db_loaded;
static struct gz_mutex db_lock = GZ_MUTEX_INIT;

static void db_path(char path[FS_PATH_MAX], bool temporary)
{
    snprintf(path, FS_PATH_MAX, "%s/data/databases%s", res_bundle(), temporary ? ".tmp" : "");
}
static bool db_name_equal(const WCHAR *a, const WCHAR *b)
{
    for (unsigned i = 0; i < 32; i++) {
        unsigned x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
        if (!x) return true;
    }
    return false;
}
static void db_load(void)
{
    if (db_loaded) return;
    char path[FS_PATH_MAX];
    db_path(path, false);
    struct fs_stat st;
    if (fs_stat(path, &st)) { db_loaded = true; return; }
    if (st.size != 8 + DATABASES * DB_ENTRY) ce_fatal("Invalid CE database catalog");
    uint8_t data[8 + DATABASES * DB_ENTRY];
    if (fs_read_file(path, data, sizeof(data)) != sizeof(data) ||
        memcmp(data, "CDB1", 4) || ce_u32(data + 4) != DATABASES)
        ce_fatal("Cannot read CE database catalog");
    for (unsigned i = 0; i < DATABASES; i++) {
        const uint8_t *p = data + 8 + i * DB_ENTRY;
        databases[i].oid = ce_u32(p);
        databases[i].type = ce_u32(p + 4);
        databases[i].count = ce_u32(p + 8);
        for (unsigned j = 0; j < 8; j++) databases[i].sorts[j] = ce_u32(p + 12 + j * 4);
        for (unsigned j = 0; j < 32; j++) databases[i].name[j] = ce_u16(p + 44 + j * 2);
        if (databases[i].count > 4 || databases[i].name[31] ||
            (databases[i].oid && (databases[i].oid != i + 1 || !databases[i].name[0])))
            ce_fatal("Invalid CE database catalog entry");
        for (unsigned j = 0; j < i; j++)
            if (databases[i].oid && databases[j].oid && db_name_equal(databases[i].name, databases[j].name))
                ce_fatal("Duplicate CE database name");
    }
    db_loaded = true;
}
static bool db_save(void)
{
    uint8_t data[8 + DATABASES * DB_ENTRY] = {0};
    memcpy(data, "CDB1", 4); ce_put32(data + 4, DATABASES);
    for (unsigned i = 0; i < DATABASES; i++) {
        uint8_t *p = data + 8 + i * DB_ENTRY;
        ce_put32(p, databases[i].oid); ce_put32(p + 4, databases[i].type); ce_put32(p + 8, databases[i].count);
        for (unsigned j = 0; j < 8; j++) ce_put32(p + 12 + j * 4, databases[i].sorts[j]);
        for (unsigned j = 0; j < 32; j++) ce_put16(p + 44 + j * 2, databases[i].name[j]);
    }
    char folder[FS_PATH_MAX], path[FS_PATH_MAX], temporary[FS_PATH_MAX];
    snprintf(folder, sizeof(folder), "%s/data", res_bundle());
    db_path(path, false); db_path(temporary, true);
    return !fs_mkdirs(folder) && fs_write_file(temporary, data, sizeof(data)) == sizeof(data) &&
        !fs_rename(temporary, path);
}
CE_API(CeCreateDatabase)
{
    if (!a0 || !*PTR(WCHAR, a0) || a2 > 4 || (a2 && !a3)) { ce_error = 87; return 0; }
    WCHAR name[32] = {0};
    unsigned n = MIN(ce_wlen(PTR(WCHAR, a0)), 31u);
    memcpy(name, PTR(void, a0), n * 2);
    gz_mutex_lock(&db_lock); db_load();
    unsigned empty = DATABASES;
    for (unsigned i = 0; i < DATABASES; i++) {
        if (!databases[i].oid) empty = i;
        else if (db_name_equal(name, databases[i].name)) {
            gz_mutex_unlock(&db_lock); ce_error = 52; return 0;
        }
    }
    if (empty == DATABASES) { gz_mutex_unlock(&db_lock); ce_error = 112; return 0; }
    databases[empty].oid = empty + 1; databases[empty].type = a1; databases[empty].count = a2;
    memcpy(databases[empty].name, name, sizeof(name));
    if (a2) memcpy(databases[empty].sorts, PTR(void, a3), a2 * 8);
    bool ok = db_save();
    if (!ok) memset(&databases[empty], 0, sizeof(databases[empty]));
    gz_mutex_unlock(&db_lock);
    ce_error = ok ? 0 : 112;
    return ok ? empty + 1 : 0;
}
CE_API(CeOpenDatabase)
{
    if (!a0 || (!*PTR(CW, a0) && !a1)) { ce_error = 87; return (CW)-1; }
    /* CEDB_AUTOINCREMENT is the only supported flag; notifications require
     * record mutation and are not available in this catalog-only subset. */
    if ((a3 & ~1u) || a4) { ce_error = 50; return (CW)-1; }
    gz_mutex_lock(&db_lock); db_load();
    CW oid = *PTR(CW, a0);
    for (unsigned i = 0; i < DATABASES; i++) {
        if (!databases[i].oid || (oid ? oid != databases[i].oid :
                !db_name_equal(PTR(WCHAR, a1), databases[i].name))) continue;
        bool sorted = !a2;
        for (unsigned j = 0; j < databases[i].count; j++) sorted |= a2 == databases[i].sorts[j * 2];
        if (!sorted) { gz_mutex_unlock(&db_lock); ce_error = 87; return (CW)-1; }
        for (unsigned j = 0; j < ARRAY_SIZE(db_handles); j++) if (!db_handles[j].handle) {
            CW h = 0x7c000000u | (++db_serial & 0xffffffu);
            db_handles[j].handle = h; db_handles[j].database = i;
            *PTR(CW, a0) = databases[i].oid;
            gz_mutex_unlock(&db_lock); return h;
        }
        gz_mutex_unlock(&db_lock); ce_error = 8; return (CW)-1;
    }
    gz_mutex_unlock(&db_lock); ce_error = oid ? 87 : 2; return (CW)-1;
}
static bool db_handle(CW h)
{
    for (unsigned i = 0; i < ARRAY_SIZE(db_handles); i++) if (db_handles[i].handle == h && h) return true;
    return false;
}
bool ce_database_close(CW h, bool *handled)
{
    *handled = (h & 0xff000000u) == 0x7c000000u;
    if (!*handled) return false;
    gz_mutex_lock(&db_lock);
    for (unsigned i = 0; i < ARRAY_SIZE(db_handles); i++) if (db_handles[i].handle == h) {
        db_handles[i].handle = 0; gz_mutex_unlock(&db_lock); return true;
    }
    gz_mutex_unlock(&db_lock); ce_error = 6; return false;
}
CE_API(CeReadRecordProps)
{
    gz_mutex_lock(&db_lock); bool valid = db_handle(a0); gz_mutex_unlock(&db_lock);
    if (!valid) { ce_error = 6; return 0; }
    if (!a2 || !a4 || !a5) { ce_error = 87; return 0; }
    *PTR(uint16_t, a2) = 0;
    ce_error = 259; /* ERROR_NO_MORE_ITEMS: no records were written. */
    return 0;
}
CE_API(CeSeekDatabase)
{
    gz_mutex_lock(&db_lock); bool valid = db_handle(a0); gz_mutex_unlock(&db_lock);
    if (!valid) { ce_error = 6; return 0; }
    ce_error = 259;
    return 0;
}
void *ce_resolve_database(const char *name)
{
    static const struct ce_export api[] = {CE_EXPORT(CeCreateDatabase), CE_EXPORT(CeOpenDatabase),
        CE_EXPORT(CeReadRecordProps), CE_EXPORT(CeSeekDatabase)};
    for (unsigned i = 0; i < ARRAY_SIZE(api); i++) if (!strcmp(name, api[i].name)) return api[i].address;
    return NULL;
}
