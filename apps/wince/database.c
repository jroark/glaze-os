/* Windows CE object-store databases (apps/wince/dbstore.c holds the model).
 * Direct NAND launches share one store, /Library/WinCE/databases.cdb, as CE
 * apps share the object store: Calendar's alarms, pimdb's and GetDisk's
 * databases. A generation file tells other CE processes to reload. Packaged
 * apps keep a private store; their earlier catalog-only file (CDB1) is
 * imported. Writes are saved when the app next waits for input, closes a
 * handle or exits. Contracts: CE 3.0 CeCreateDatabase, CeOpenDatabase,
 * CeSeekDatabase, CeReadRecordProps, CeWriteRecordProps, CeOidGetInfo
 * (https://learn.microsoft.com/en-us/previous-versions/ms933221(v=msdn.10)).
 * Change notifications to hwndNotify are not sent. */
#include "runtime.h"
#include "dbstore.h"
#include "source.h"
#include <glaze/thread.h>

#define SHARED_STORE "/Library/WinCE/databases.cdb"
#define SHARED_GEN "/Library/WinCE/databases.gen"
#define HANDLES 32
#define CEDB_AUTOINCREMENT 1
#define CEDB_ALLOCATE 1
#define CEDB_PROPNOTFOUND 0x400
#define CEDB_PROPDELETE 0x200
CE_API(LocalAlloc);
CE_API(LocalReAlloc);
CE_API(LocalSize);
CE_API(GetSystemTime);
CE_API(SystemTimeToFileTime);

static struct ce_db_store store;
static bool loaded, dirty;
static uint32_t generation, serial;
static struct gz_mutex db_lock = GZ_MUTEX_INIT;
static struct db_handle {
    CW handle;
    uint32_t oid, sort, current, version, serial;
    bool find, autoincrement;
    unsigned type, next; /* CeFindFirstDatabase: type and position */
    int position;
    unsigned *order, order_count;
} handles[HANDLES];
static CW handle_serial;

static bool shared(void)
{
    return ce_source_active();
}
static void store_path(char path[FS_PATH_MAX], const char *suffix)
{
    if (shared())
        snprintf(path, FS_PATH_MAX, "%s%s", SHARED_STORE, suffix);
    else
        snprintf(path, FS_PATH_MAX, "%s/data/databases.cdb%s", res_bundle(), suffix);
}
static uint32_t shared_generation(void)
{
    uint8_t word[4];
    return shared() && fs_read_file(SHARED_GEN, word, 4) == 4 ? ce_u32(word) : 0;
}
/* The catalog-only format earlier packaged apps saved: 16 entries of OID,
 * type, sort count, 4 sort specs and a 32-unit name. */
static void import_catalog(void)
{
    char path[FS_PATH_MAX];
    uint8_t data[8 + 16 * 108];
    snprintf(path, sizeof(path), "%s/data/databases", res_bundle());
    if (fs_read_file(path, data, sizeof(data)) != (int)sizeof(data) || memcmp(data, "CDB1", 4))
        return;
    for (unsigned i = 0; i < 16; i++) {
        const uint8_t *p = data + 8 + i * 108;
        uint16_t name[32];
        uint32_t specs[4][2], oid;
        for (unsigned j = 0; j < 32; j++)
            name[j] = ce_u16(p + 44 + j * 2);
        for (unsigned j = 0; j < 4; j++) {
            specs[j][0] = ce_u32(p + 12 + j * 8);
            specs[j][1] = ce_u32(p + 16 + j * 8);
        }
        if (ce_u32(p) && !ce_db_create(&store, name, ce_u32(p + 4), MIN(ce_u32(p + 8), 4u),
                                       (const uint32_t(*)[2])specs, &oid))
            dirty = true;
    }
}
static void flush(void)
{
    if (!dirty)
        return;
    uint32_t size = 0;
    uint8_t *data = ce_db_encode(&store, &size);
    char path[FS_PATH_MAX], tmp[FS_PATH_MAX];
    store_path(path, "");
    store_path(tmp, ".tmp");
    char folder[FS_PATH_MAX];
    strlcpy(folder, path, sizeof(folder));
    *strrchr(folder, '/') = 0;
    bool ok = data && !fs_mkdirs(folder) && fs_write_file(tmp, data, size) == (int)size && !fs_rename(tmp, path);
    free(data);
    if (!ok) {
        fs_remove(tmp);
        printf("wince: cannot save CE databases\n");
        return;
    }
    dirty = false;
    if (shared()) {
        uint8_t word[4];
        ce_put32(word, generation = shared_generation() + 1);
        fs_write_file(SHARED_GEN, word, 4);
    }
}
static void load(void)
{
    if (loaded && (!shared() || shared_generation() == generation))
        return;
    flush(); /* ours first: another app's newer copy is merged by replacing */
    if (loaded && shared() && shared_generation() == generation)
        return;
    generation = shared_generation();
    loaded = true;
    serial++;
    char path[FS_PATH_MAX];
    store_path(path, "");
    struct fs_stat st;
    ce_db_store_clear(&store);
    if (fs_stat(path, &st) || st.type != FS_FILE) {
        if (!shared())
            import_catalog();
        return;
    }
    uint8_t *data = st.size <= CE_DB_FILE_MAX ? malloc(st.size) : NULL;
    if (!data || fs_read_file(path, data, st.size) != (int)st.size || !ce_db_decode(&store, data, st.size))
        printf("wince: the CE database store is unreadable; starting empty\n");
    free(data);
}
/* Saves pending changes; the message loop calls this before it waits. */
void ce_database_flush(void)
{
    gz_mutex_lock(&db_lock);
    flush();
    gz_mutex_unlock(&db_lock);
}
static uint64_t now(void)
{
    uint16_t st[8];
    uint8_t ft[8];
    ce_GetSystemTime(WORD(st), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (!ce_SystemTimeToFileTime(WORD(st), WORD(ft), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
        return 0;
    return (uint64_t)ce_u32(ft) | (uint64_t)ce_u32(ft + 4) << 32;
}
static void changed(struct ce_db *db)
{
    db->modified = now();
    db->version++;
    dirty = true;
}

/* ---- handles ------------------------------------------------------------ */

static struct db_handle *find_handle(CW h, bool find)
{
    for (unsigned i = 0; h && i < HANDLES; i++)
        if (handles[i].handle == h && handles[i].find == find)
            return &handles[i];
    return NULL;
}
static struct db_handle *new_handle(bool find)
{
    for (unsigned i = 0; i < HANDLES; i++)
        if (!handles[i].handle) {
            memset(&handles[i], 0, sizeof(handles[i]));
            handles[i].handle = (find ? 0x7d000000u : 0x7c000000u) | (++handle_serial & 0xffffffu);
            handles[i].find = find;
            return &handles[i];
        }
    return NULL;
}
/* The open database, with this handle's order current and its position on
 * the current record (or, if that was deleted, the one that followed). */
static struct ce_db *handle_db(struct db_handle *h)
{
    struct ce_db *db = ce_db_find(&store, h->oid, NULL);
    if (!db)
        return NULL;
    if (h->version != db->version || h->serial != serial || h->order_count != db->count) {
        unsigned *order = realloc(h->order, (db->count ? db->count : 1) * sizeof(*order));
        if (!order)
            return NULL;
        h->order = order;
        h->order_count = db->count;
        ce_db_sort(db, h->sort, order);
        h->version = db->version;
        h->serial = serial;
        for (unsigned i = 0; h->current && i < db->count; i++)
            if (db->records[order[i]].oid == h->current)
                h->position = (int)i;
    }
    if (h->position >= 0 && h->position < (int)db->count)
        h->current = db->records[h->order[h->position]].oid;
    else
        h->current = 0;
    return db;
}
bool ce_database_close(CW h, bool *handled)
{
    *handled = (h & 0xfe000000u) == 0x7c000000u;
    if (!*handled)
        return false;
    gz_mutex_lock(&db_lock);
    struct db_handle *d = find_handle(h, (h >> 24) == 0x7d);
    if (d) {
        free(d->order);
        memset(d, 0, sizeof(*d));
        flush();
    }
    gz_mutex_unlock(&db_lock);
    if (!d)
        ce_error = 6;
    return d != NULL;
}

/* ---- CEPROPVAL: propid, wLenData, wFlags, then an 8-byte value ---------- */

static bool guest_prop(const uint8_t *p, struct ce_db_prop *out)
{
    uint32_t id = ce_u32(p), type = id & 0xffff;
    unsigned flags = ce_u16(p + 6);
    const uint8_t *val = p + 8;
    if (flags & CEDB_PROPDELETE) {
        *out = (struct ce_db_prop){id, CE_DB_DELETE, NULL};
        return true;
    }
    if (type == CEVT_LPWSTR) {
        const WCHAR *s = PTR(WCHAR, ce_u32(val));
        unsigned n = s ? ce_wlen(s) : 0;
        if (!s || n >= CE_DB_PROP_BYTES / 2)
            return false;
        *out = (struct ce_db_prop){id, (n + 1) * 2, (uint8_t *)s};
    } else if (type == CEVT_BLOB) {
        uint32_t count = ce_u32(val);
        if (count > CE_DB_PROP_BYTES || (count && !ce_u32(val + 4)))
            return false;
        *out = (struct ce_db_prop){id, count, PTR(uint8_t, ce_u32(val + 4))};
    } else {
        *out = (struct ce_db_prop){id, 8, (uint8_t *)val};
    }
    return true;
}

/* ---- API ---------------------------------------------------------------- */

CE_API(CeCreateDatabase)
{
    if (!a0 || !*PTR(WCHAR, a0) || a2 > CE_DB_SORTS || (a2 && !a3)) {
        ce_error = 87;
        return 0;
    }
    uint16_t name[32] = {0};
    memcpy(name, PTR(void, a0), MIN(ce_wlen(PTR(WCHAR, a0)), 31u) * 2);
    uint32_t specs[CE_DB_SORTS][2] = {{0}};
    for (unsigned i = 0; i < a2; i++) {
        specs[i][0] = ce_u32(PTR(uint8_t, a3) + i * 8);
        specs[i][1] = ce_u32(PTR(uint8_t, a3) + i * 8 + 4);
    }
    gz_mutex_lock(&db_lock);
    load();
    uint32_t oid = 0;
    int error = ce_db_create(&store, name, a1, a2, (const uint32_t(*)[2])specs, &oid);
    if (!error) {
        changed(ce_db_find(&store, oid, NULL));
        flush();
    }
    gz_mutex_unlock(&db_lock);
    ce_error = error == 183 ? 52 : error == 8 ? 112 : (CW)error;
    return error ? 0 : oid;
}
CE_API(CeOpenDatabase)
{
    if (!a0 || (!*PTR(CW, a0) && (!a1 || !*PTR(WCHAR, a1)))) {
        ce_error = 87;
        return (CW)-1;
    }
    uint16_t name[32] = {0};
    if (!*PTR(CW, a0))
        memcpy(name, PTR(void, a1), MIN(ce_wlen(PTR(WCHAR, a1)), 31u) * 2);
    gz_mutex_lock(&db_lock);
    load();
    struct ce_db *db = ce_db_find(&store, *PTR(CW, a0), name);
    if (!db) {
        gz_mutex_unlock(&db_lock);
        ce_error = *PTR(CW, a0) ? 87 : 2;
        return (CW)-1;
    }
    /* A zero propid traverses in the first sort order, if there is one. */
    uint32_t sort = a2 ? a2 : db->sort_count ? db->sorts[0][0] : 0;
    bool known = !sort;
    for (unsigned i = 0; i < db->sort_count; i++)
        known |= db->sorts[i][0] == sort;
    struct db_handle *h = known ? new_handle(false) : NULL;
    if (!h) {
        gz_mutex_unlock(&db_lock);
        ce_error = known ? 8 : 87;
        return (CW)-1;
    }
    h->oid = db->oid;
    h->sort = sort;
    h->autoincrement = a3 & CEDB_AUTOINCREMENT;
    h->version = db->version - 1; /* sort on first use */
    *PTR(CW, a0) = db->oid;
    CW handle = h->handle;
    gz_mutex_unlock(&db_lock);
    return handle;
}
CE_API(CeDeleteDatabase)
{
    gz_mutex_lock(&db_lock);
    load();
    int error = ce_db_delete(&store, a0);
    if (!error) {
        dirty = true;
        flush();
    }
    gz_mutex_unlock(&db_lock);
    ce_error = (CW)error;
    return !error;
}
CE_API(CeFindFirstDatabase)
{
    gz_mutex_lock(&db_lock);
    struct db_handle *h = new_handle(true);
    if (h)
        h->type = a0;
    CW handle = h ? h->handle : (CW)-1;
    gz_mutex_unlock(&db_lock);
    if (!h)
        ce_error = 8;
    return handle;
}
CE_API(CeFindNextDatabase)
{
    gz_mutex_lock(&db_lock);
    load();
    struct db_handle *h = find_handle(a0, true);
    CW oid = 0;
    while (h && h->next < CE_DB_MAX && !oid) {
        struct ce_db *db = store.dbs[h->next++];
        if (db && (!h->type || db->type == h->type))
            oid = db->oid;
    }
    gz_mutex_unlock(&db_lock);
    ce_error = !h ? 6 : oid ? 0 : 18; /* ERROR_NO_MORE_FILES */
    return oid;
}
/* CEDBASEINFO: flags, name[32], type, record count (WORD), sort count
 * (WORD), size, FILETIME modified, SORTORDERSPEC[4]: 120 bytes. */
static void database_info(uint8_t *p, const struct ce_db *db)
{
    memset(p, 0, 120);
    ce_put32(p, 0xf); /* CEDB_VALIDNAME | VALIDTYPE | VALIDSORTSPEC | VALIDMODTIME */
    memcpy(p + 4, db->name, 64);
    ce_put32(p + 68, db->type);
    ce_put16(p + 72, (uint16_t)MIN(db->count, 0xffffu));
    ce_put16(p + 74, (uint16_t)db->sort_count);
    uint32_t size = 0;
    for (unsigned i = 0; i < db->count; i++)
        size += ce_db_record_bytes(&db->records[i]);
    ce_put32(p + 76, size);
    ce_put32(p + 80, (uint32_t)db->modified);
    ce_put32(p + 84, (uint32_t)(db->modified >> 32));
    for (unsigned i = 0; i < CE_DB_SORTS; i++) {
        ce_put32(p + 88 + i * 8, db->sorts[i][0]);
        ce_put32(p + 92 + i * 8, db->sorts[i][1]);
    }
}
/* CEOIDINFO: wObjType, wPad, then the object's info (OBJTYPE_DATABASE 3:
 * CEDBASEINFO; OBJTYPE_RECORD 4: the parent database's OID). */
CE_API(CeOidGetInfo)
{
    if (!a1) {
        ce_error = 87;
        return 0;
    }
    gz_mutex_lock(&db_lock);
    load();
    int record;
    struct ce_db *db = ce_db_owner(&store, a0, &record);
    uint8_t *p = PTR(uint8_t, a1);
    if (db && record < 0) {
        ce_put32(p, 3);
        database_info(p + 4, db);
    } else if (db) {
        ce_put32(p, 4);
        ce_put32(p + 4, db->oid);
    }
    gz_mutex_unlock(&db_lock);
    ce_error = db ? 0 : 87;
    return db != NULL;
}
CE_API(CeSetDatabaseInfo)
{
    if (!a1) {
        ce_error = 87;
        return 0;
    }
    const uint8_t *p = PTR(uint8_t, a1);
    uint32_t flags = ce_u32(p);
    gz_mutex_lock(&db_lock);
    load();
    struct ce_db *db = ce_db_find(&store, a0, NULL);
    int error = db ? 0 : 87;
    if (!error && (flags & 1)) {
        uint16_t name[32] = {0};
        for (unsigned i = 0; i < 31 && ce_u16(p + 4 + i * 2); i++)
            name[i] = ce_u16(p + 4 + i * 2);
        error = ce_db_rename(&store, db, name);
    }
    if (!error && (flags & 4) && ce_u16(p + 74) > CE_DB_SORTS)
        error = 87;
    if (!error) {
        if (flags & 2)
            db->type = ce_u32(p + 68);
        if (flags & 4) {
            db->sort_count = ce_u16(p + 74);
            for (unsigned i = 0; i < CE_DB_SORTS; i++) {
                db->sorts[i][0] = i < db->sort_count ? ce_u32(p + 88 + i * 8) : 0;
                db->sorts[i][1] = i < db->sort_count ? ce_u32(p + 92 + i * 8) : 0;
            }
        }
        changed(db);
        if (flags & 8)
            db->modified = (uint64_t)ce_u32(p + 80) | (uint64_t)ce_u32(p + 84) << 32;
        flush();
    }
    gz_mutex_unlock(&db_lock);
    ce_error = error == 183 ? 52 : (CW)error;
    return !error;
}
CE_API(CeSeekDatabase)
{
    gz_mutex_lock(&db_lock);
    load();
    struct db_handle *h = find_handle(a0, false);
    struct ce_db *db = h ? handle_db(h) : NULL;
    if (!db) {
        gz_mutex_unlock(&db_lock);
        ce_error = 6;
        return 0;
    }
    struct ce_db_prop key = {0, 0, NULL};
    bool by_value = a1 >= CEDB_SEEK_VALUESMALLER && a1 <= CEDB_SEEK_VALUEGREATEREQUAL;
    if (by_value && (!a2 || !guest_prop(PTR(uint8_t, a2), &key))) {
        gz_mutex_unlock(&db_lock);
        ce_error = 87;
        return 0;
    }
    int at = ce_db_seek(db, h->sort, h->order, h->position, a1, a2, by_value ? &key : NULL);
    CW oid = 0;
    if (at >= 0) {
        h->position = at;
        oid = h->current = db->records[h->order[at]].oid;
        if (a3)
            *PTR(CW, a3) = (CW)at;
    }
    gz_mutex_unlock(&db_lock);
    ce_error = oid ? 0 : 259; /* ERROR_NO_MORE_ITEMS */
    return oid;
}
/* The current record's properties (all, or the listed PROPIDs) as CE lays
 * them out: CEPROPVALs, then their strings and blobs in the same buffer. */
CE_API(CeReadRecordProps)
{
    if (!a2 || !a4 || !a5) {
        ce_error = 87;
        return 0;
    }
    gz_mutex_lock(&db_lock);
    load();
    struct db_handle *h = find_handle(a0, false);
    struct ce_db *db = h ? handle_db(h) : NULL;
    if (!db || h->position < 0 || h->position >= (int)db->count) {
        gz_mutex_unlock(&db_lock);
        if (db)
            *PTR(uint16_t, a2) = 0;
        ce_error = db ? 259 : 6;
        return 0;
    }
    const struct ce_db_record *r = &db->records[h->order[h->position]];
    unsigned n = a3 ? *PTR(uint16_t, a2) : r->count;
    uint32_t need = n * 16;
    for (unsigned i = 0; i < n; i++) {
        const struct ce_db_prop *p = a3 ? ce_db_prop(r, PTR(CW, a3)[i]) : &r->props[i];
        uint32_t type = p ? p->id & 0xffff : 0;
        if (type == CEVT_LPWSTR || type == CEVT_BLOB)
            need += ALIGN_UP(p->size, 4u);
    }
    CW *buffer = PTR(CW, a4), *size = PTR(CW, a5);
    if (a1 & CEDB_ALLOCATE) {
        CW block = *buffer;
        if (!block)
            block = ce_LocalAlloc(0, need, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        else if (ce_LocalSize(block, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) < need)
            block = ce_LocalReAlloc(block, need, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        if (!block) {
            gz_mutex_unlock(&db_lock);
            ce_error = 8;
            return 0;
        }
        *buffer = block;
    } else if (!*buffer || *size < need) {
        *size = need;
        gz_mutex_unlock(&db_lock);
        ce_error = 122; /* ERROR_INSUFFICIENT_BUFFER */
        return 0;
    }
    *size = need;
    uint8_t *out = PTR(uint8_t, *buffer), *data = out + n * 16;
    for (unsigned i = 0; i < n; i++) {
        uint8_t *v = out + i * 16;
        const struct ce_db_prop *p = a3 ? ce_db_prop(r, PTR(CW, a3)[i]) : &r->props[i];
        memset(v, 0, 16);
        ce_put32(v, a3 ? PTR(CW, a3)[i] : p->id);
        if (!p) {
            ce_put16(v + 6, CEDB_PROPNOTFOUND);
            continue;
        }
        uint32_t type = p->id & 0xffff;
        if (type == CEVT_LPWSTR || type == CEVT_BLOB) {
            memcpy(data, p->data, p->size);
            if (type == CEVT_LPWSTR) {
                ce_put32(v + 8, WORD(data));
            } else {
                ce_put32(v + 8, p->size);
                ce_put32(v + 12, WORD(data));
            }
            ce_put16(v + 4, (uint16_t)MIN(p->size, 0xffffu));
            data += ALIGN_UP(p->size, 4u);
        } else {
            memcpy(v + 8, p->data, 8);
        }
    }
    *PTR(uint16_t, a2) = (uint16_t)n;
    CW oid = r->oid;
    if (h->autoincrement) {
        h->position++;
        h->current = h->position < (int)db->count ? db->records[h->order[h->position]].oid : 0;
    }
    gz_mutex_unlock(&db_lock);
    ce_error = 0;
    return oid;
}
CE_API(CeWriteRecordProps)
{
    if (a2 > CE_DB_PROPS || (a2 && !a3)) {
        ce_error = 87;
        return 0;
    }
    struct ce_db_prop *props = calloc(a2 ? a2 : 1, sizeof(*props));
    if (!props) {
        ce_error = 8;
        return 0;
    }
    for (unsigned i = 0; i < a2; i++)
        if (!guest_prop(PTR(uint8_t, a3) + i * 16, &props[i])) {
            free(props);
            ce_error = 87;
            return 0;
        }
    gz_mutex_lock(&db_lock);
    load();
    struct db_handle *h = find_handle(a0, false);
    struct ce_db *db = h ? handle_db(h) : NULL;
    uint32_t oid = 0;
    int error = db ? ce_db_write(&store, db, a1, props, a2, &oid) : 6;
    if (db && oid)
        changed(db);
    gz_mutex_unlock(&db_lock);
    free(props);
    ce_error = (CW)error;
    return error ? 0 : oid;
}
CE_API(CeDeleteRecord)
{
    gz_mutex_lock(&db_lock);
    load();
    struct db_handle *h = find_handle(a0, false);
    struct ce_db *db = h ? handle_db(h) : NULL;
    int error = db ? ce_db_delete_record(db, a1) : 6;
    if (!error)
        changed(db);
    gz_mutex_unlock(&db_lock);
    ce_error = (CW)error;
    return !error;
}
/* Object-store change notifications for a window: none are sent. */
CE_API(CeRegisterFileSystemNotification)
{
    (void)a0;
    return 1;
}
void *ce_resolve_database(const char *name)
{
    static const struct ce_export api[] = {
        CE_EXPORT(CeCreateDatabase), CE_EXPORT(CeOpenDatabase), CE_EXPORT(CeDeleteDatabase),
        CE_EXPORT(CeFindFirstDatabase), CE_EXPORT(CeFindNextDatabase), CE_EXPORT(CeOidGetInfo),
        CE_EXPORT(CeSetDatabaseInfo), CE_EXPORT(CeSeekDatabase), CE_EXPORT(CeReadRecordProps),
        CE_EXPORT(CeWriteRecordProps), CE_EXPORT(CeDeleteRecord), CE_EXPORT(CeRegisterFileSystemNotification)};
    for (unsigned i = 0; i < ARRAY_SIZE(api); i++)
        if (!strcmp(name, api[i].name))
            return api[i].address;
    return NULL;
}
