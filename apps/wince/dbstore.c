/* CE object-store databases; see dbstore.h. Records keep their storage
 * order and each open handle sorts indices by its sort property. */
#include "dbstore.h"
#include "pe.h"
#include <stdlib.h>
#include <string.h>

static unsigned fold(unsigned c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7))
        return c + 32;
    return c;
}
static bool name_equal(const uint16_t *a, const uint16_t *b)
{
    for (unsigned i = 0; i < 32; i++) {
        if (fold(a[i]) != fold(b[i]))
            return false;
        if (!a[i])
            return true;
    }
    return true;
}
static void free_record(struct ce_db_record *r)
{
    for (unsigned i = 0; i < r->count; i++)
        free(r->props[i].data);
    free(r->props);
    r->props = NULL;
    r->count = 0;
}
static void free_db(struct ce_db *db)
{
    if (!db)
        return;
    for (unsigned i = 0; i < db->count; i++)
        free_record(&db->records[i]);
    free(db->records);
    free(db);
}
void ce_db_store_clear(struct ce_db_store *s)
{
    for (unsigned i = 0; i < CE_DB_MAX; i++)
        free_db(s->dbs[i]);
    memset(s, 0, sizeof(*s));
}
struct ce_db *ce_db_find(struct ce_db_store *s, uint32_t oid, const uint16_t *name)
{
    for (unsigned i = 0; i < CE_DB_MAX; i++)
        if (s->dbs[i] && (oid ? s->dbs[i]->oid == oid : name && name_equal(s->dbs[i]->name, name)))
            return s->dbs[i];
    return NULL;
}
struct ce_db *ce_db_owner(struct ce_db_store *s, uint32_t oid, int *record)
{
    *record = -1;
    for (unsigned i = 0; oid && i < CE_DB_MAX; i++) {
        struct ce_db *db = s->dbs[i];
        if (!db)
            continue;
        if (db->oid == oid)
            return db;
        int at = ce_db_index(db, oid);
        if (at >= 0) {
            *record = at;
            return db;
        }
    }
    return NULL;
}
static uint32_t new_oid(struct ce_db_store *s)
{
    if (s->next_oid < 0x10000)
        s->next_oid = 0x10000;
    return s->next_oid++;
}
static bool valid_name(const uint16_t *name)
{
    if (!name || !name[0])
        return false;
    for (unsigned i = 0; i < 32; i++)
        if (!name[i])
            return true;
    return false;
}
int ce_db_create(struct ce_db_store *s, const uint16_t *name, uint32_t type, unsigned sorts,
                 const uint32_t (*specs)[2], uint32_t *oid)
{
    if (!valid_name(name) || sorts > CE_DB_SORTS || (sorts && !specs))
        return 87;
    if (ce_db_find(s, 0, name))
        return 183;
    unsigned slot = CE_DB_MAX;
    for (unsigned i = 0; i < CE_DB_MAX && slot == CE_DB_MAX; i++)
        if (!s->dbs[i])
            slot = i;
    if (slot == CE_DB_MAX)
        return 8;
    struct ce_db *db = calloc(1, sizeof(*db));
    if (!db)
        return 8;
    for (unsigned i = 0; i < 32 && name[i]; i++)
        db->name[i] = name[i];
    db->type = type;
    db->sort_count = sorts;
    for (unsigned i = 0; i < sorts; i++) {
        db->sorts[i][0] = specs[i][0];
        db->sorts[i][1] = specs[i][1];
    }
    db->oid = new_oid(s);
    s->dbs[slot] = db;
    *oid = db->oid;
    return 0;
}
int ce_db_delete(struct ce_db_store *s, uint32_t oid)
{
    for (unsigned i = 0; i < CE_DB_MAX; i++)
        if (s->dbs[i] && s->dbs[i]->oid == oid) {
            free_db(s->dbs[i]);
            s->dbs[i] = NULL;
            return 0;
        }
    return 87;
}
int ce_db_rename(struct ce_db_store *s, struct ce_db *db, const uint16_t *name)
{
    if (!valid_name(name))
        return 87;
    struct ce_db *other = ce_db_find(s, 0, name);
    if (other && other != db)
        return 183;
    memset(db->name, 0, sizeof(db->name));
    for (unsigned i = 0; i < 32 && name[i]; i++)
        db->name[i] = name[i];
    db->version++;
    return 0;
}
int ce_db_index(const struct ce_db *db, uint32_t oid)
{
    for (unsigned i = 0; oid && i < db->count; i++)
        if (db->records[i].oid == oid)
            return (int)i;
    return -1;
}
const struct ce_db_prop *ce_db_prop(const struct ce_db_record *r, uint32_t id)
{
    for (unsigned i = 0; i < r->count; i++)
        if (r->props[i].id == id)
            return &r->props[i];
    return NULL;
}
uint32_t ce_db_record_bytes(const struct ce_db_record *r)
{
    uint32_t n = 0;
    for (unsigned i = 0; i < r->count; i++)
        n += 16 + r->props[i].size;
    return n;
}
static bool known_type(uint32_t id)
{
    switch (id & 0xffff) {
    case CEVT_I2: case CEVT_I4: case CEVT_R8: case CEVT_BOOL: case CEVT_UI2: case CEVT_UI4:
    case CEVT_LPWSTR: case CEVT_FILETIME: case CEVT_BLOB:
        return true;
    }
    return false;
}
static int set_prop(struct ce_db_record *r, const struct ce_db_prop *p)
{
    struct ce_db_prop *old = (struct ce_db_prop *)ce_db_prop(r, p->id);
    if (p->size & CE_DB_DELETE) {
        if (old) {
            free(old->data);
            *old = r->props[--r->count];
        }
        return 0;
    }
    uint32_t type = p->id & 0xffff;
    bool scalar = type != CEVT_LPWSTR && type != CEVT_BLOB;
    if (!known_type(p->id) || (scalar && p->size != 8) || p->size > CE_DB_PROP_BYTES || (p->size && !p->data) ||
        (type == CEVT_LPWSTR && (p->size < 2 || (p->size & 1))))
        return 87;
    uint8_t *copy = malloc(p->size ? p->size : 1);
    if (!copy)
        return 8;
    if (p->size)
        memcpy(copy, p->data, p->size);
    if (old) {
        free(old->data);
        old->data = copy;
        old->size = p->size;
        return 0;
    }
    if (r->count == CE_DB_PROPS) {
        free(copy);
        return 87;
    }
    struct ce_db_prop *grown = realloc(r->props, (r->count + 1) * sizeof(*grown));
    if (!grown) {
        free(copy);
        return 8;
    }
    r->props = grown;
    r->props[r->count++] = (struct ce_db_prop){p->id, p->size, copy};
    return 0;
}
int ce_db_write(struct ce_db_store *s, struct ce_db *db, uint32_t oid, const struct ce_db_prop *props,
                unsigned count, uint32_t *written)
{
    for (unsigned i = 0; i < count; i++)
        if (!known_type(props[i].id))
            return 87;
    struct ce_db_record *r;
    if (!oid) {
        if (db->count == db->capacity) {
            unsigned grow = db->capacity ? db->capacity * 2 : 16;
            struct ce_db_record *more = realloc(db->records, grow * sizeof(*more));
            if (!more)
                return 8;
            db->records = more;
            db->capacity = grow;
        }
        r = &db->records[db->count++];
        *r = (struct ce_db_record){new_oid(s), 0, NULL};
    } else {
        int at = ce_db_index(db, oid);
        if (at < 0)
            return 87;
        r = &db->records[at];
    }
    for (unsigned i = 0; i < count; i++) {
        int error = set_prop(r, &props[i]);
        if (error)
            return error;
    }
    db->version++;
    *written = r->oid;
    return 0;
}
int ce_db_delete_record(struct ce_db *db, uint32_t oid)
{
    int at = ce_db_index(db, oid);
    if (at < 0)
        return 87;
    free_record(&db->records[at]);
    memmove(&db->records[at], &db->records[at + 1], (db->count - (unsigned)at - 1) * sizeof(db->records[0]));
    db->count--;
    db->version++;
    return 0;
}

/* ---- ordering ----------------------------------------------------------- */

static uint32_t sort_flags(const struct ce_db *db, uint32_t sort)
{
    for (unsigned i = 0; i < db->sort_count; i++)
        if (db->sorts[i][0] == sort)
            return db->sorts[i][1];
    return 0;
}
static uint64_t u64(const uint8_t *p)
{
    return (uint64_t)ce_u32(p) | (uint64_t)ce_u32(p + 4) << 32;
}
/* IEEE doubles as unsigned integers that order the same way. */
static uint64_t double_key(const uint8_t *p)
{
    uint64_t bits = u64(p);
    return bits >> 63 ? ~bits : bits | (uint64_t)1 << 63;
}
static int sign(int64_t v)
{
    return v < 0 ? -1 : v > 0;
}
static int compare_props(uint32_t id, const struct ce_db_prop *a, const struct ce_db_prop *b, bool fold_case)
{
    switch (id & 0xffff) {
    case CEVT_I2:
        return sign((int16_t)ce_u16(a->data) - (int16_t)ce_u16(b->data));
    case CEVT_UI2:
        return sign((int)ce_u16(a->data) - (int)ce_u16(b->data));
    case CEVT_I4:
        return sign((int64_t)(int32_t)ce_u32(a->data) - (int32_t)ce_u32(b->data));
    case CEVT_UI4:
    case CEVT_BOOL:
        return sign((int64_t)ce_u32(a->data) - (int64_t)ce_u32(b->data));
    case CEVT_FILETIME: {
        uint64_t x = u64(a->data), y = u64(b->data);
        return x < y ? -1 : x > y;
    }
    case CEVT_R8: {
        uint64_t x = double_key(a->data), y = double_key(b->data);
        return x < y ? -1 : x > y;
    }
    case CEVT_LPWSTR:
        for (uint32_t i = 0; i + 1 < a->size && i + 1 < b->size; i += 2) {
            unsigned x = ce_u16(a->data + i), y = ce_u16(b->data + i);
            if (fold_case) {
                x = fold(x);
                y = fold(y);
            }
            if (x != y)
                return x < y ? -1 : 1;
            if (!x)
                return 0;
        }
        return sign((int64_t)a->size - b->size);
    default: {
        int c = memcmp(a->data, b->data, a->size < b->size ? a->size : b->size);
        return c ? sign(c) : sign((int64_t)a->size - b->size);
    }
    }
}
static int compare(const struct ce_db *db, uint32_t sort, const struct ce_db_prop *a, const struct ce_db_prop *b)
{
    uint32_t flags = sort_flags(db, sort);
    if (!a || !b) {
        if (!a && !b)
            return 0;
        int unknown = flags & CEDB_SORT_UNKNOWNFIRST ? -1 : 1;
        return a ? -unknown : unknown;
    }
    int c = compare_props(sort, a, b, flags & CEDB_SORT_CASEINSENSITIVE);
    return flags & CEDB_SORT_DESCENDING ? -c : c;
}
int ce_db_compare_value(const struct ce_db *db, uint32_t sort, const struct ce_db_record *r,
                        const struct ce_db_prop *value)
{
    return compare(db, sort, ce_db_prop(r, sort), value);
}
/* A stable merge sort of record indices. */
void ce_db_sort(const struct ce_db *db, uint32_t sort, unsigned *order)
{
    unsigned n = db->count;
    for (unsigned i = 0; i < n; i++)
        order[i] = i;
    if (!sort || n < 2)
        return;
    unsigned *tmp = malloc(n * sizeof(*tmp));
    if (!tmp)
        return; /* storage order */
    for (unsigned width = 1; width < n; width *= 2) {
        for (unsigned lo = 0; lo < n; lo += 2 * width) {
            unsigned mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
            unsigned i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                const struct ce_db_prop *a = ce_db_prop(&db->records[order[i]], sort),
                                        *b = ce_db_prop(&db->records[order[j]], sort);
                tmp[k++] = compare(db, sort, a, b) <= 0 ? order[i++] : order[j++];
            }
            while (i < mid)
                tmp[k++] = order[i++];
            while (j < hi)
                tmp[k++] = order[j++];
        }
        memcpy(order, tmp, n * sizeof(*tmp));
    }
    free(tmp);
}
int ce_db_seek(const struct ce_db *db, uint32_t sort, const unsigned *order, int position, uint32_t type,
               uint32_t value, const struct ce_db_prop *key)
{
    int n = (int)db->count, at = -1;
    switch (type) {
    case CEDB_SEEK_CEOID:
        for (int i = 0; i < n; i++)
            if (db->records[order[i]].oid == value)
                at = i;
        break;
    case CEDB_SEEK_BEGINNING:
        at = (int32_t)value;
        break;
    case CEDB_SEEK_CURRENT:
        at = position + (int32_t)value;
        break;
    case CEDB_SEEK_END:
        /* dwValue counts back from the last record (0 or negative in CE's
         * samples; a positive count is taken the same way). */
        at = (int32_t)value <= 0 ? n - 1 + (int32_t)value : n - 1 - (int32_t)value;
        break;
    case CEDB_SEEK_VALUESMALLER:
    case CEDB_SEEK_VALUEFIRSTEQUAL:
    case CEDB_SEEK_VALUENEXTEQUAL:
    case CEDB_SEEK_VALUEGREATER:
    case CEDB_SEEK_VALUEGREATEREQUAL:
        if (!sort || !key || (key->id & 0xffff) != (sort & 0xffff))
            return -1;
        for (int i = type == CEDB_SEEK_VALUENEXTEQUAL ? position + 1 : 0; i < n; i++) {
            int c = ce_db_compare_value(db, sort, &db->records[order[i]], key);
            if (type == CEDB_SEEK_VALUESMALLER) {
                if (c < 0)
                    at = i;
                continue;
            }
            if (type == CEDB_SEEK_VALUENEXTEQUAL) {
                at = c ? -1 : i;
                break;
            }
            if ((type == CEDB_SEEK_VALUEFIRSTEQUAL && !c) || (type == CEDB_SEEK_VALUEGREATER && c > 0) ||
                (type == CEDB_SEEK_VALUEGREATEREQUAL && c >= 0)) {
                at = i;
                break;
            }
        }
        break;
    default:
        return -1;
    }
    return at >= 0 && at < n ? at : -1;
}

/* ---- file format: "CDB2", next OID, then each database ----------------- */

uint8_t *ce_db_encode(const struct ce_db_store *s, uint32_t *size)
{
    uint32_t n = 12;
    for (unsigned i = 0; i < CE_DB_MAX; i++) {
        const struct ce_db *db = s->dbs[i];
        if (!db)
            continue;
        n += 32 + CE_DB_SORTS * 8 + 64 + 4;
        for (unsigned j = 0; j < db->count; j++) {
            n += 8;
            for (unsigned k = 0; k < db->records[j].count; k++)
                n += 8 + db->records[j].props[k].size;
        }
    }
    uint8_t *p = malloc(n), *q = p;
    if (!p)
        return NULL;
    unsigned dbs = 0;
    for (unsigned i = 0; i < CE_DB_MAX; i++)
        dbs += s->dbs[i] != NULL;
    memcpy(q, "CDB2", 4);
    ce_put32(q + 4, s->next_oid);
    ce_put32(q + 8, dbs);
    q += 12;
    for (unsigned i = 0; i < CE_DB_MAX; i++) {
        const struct ce_db *db = s->dbs[i];
        if (!db)
            continue;
        ce_put32(q, db->oid);
        ce_put32(q + 4, db->type);
        ce_put32(q + 8, db->sort_count);
        ce_put32(q + 12, (uint32_t)db->modified);
        ce_put32(q + 16, (uint32_t)(db->modified >> 32));
        ce_put32(q + 20, 0);
        ce_put32(q + 24, 0);
        ce_put32(q + 28, 0);
        q += 32;
        for (unsigned k = 0; k < CE_DB_SORTS; k++) {
            ce_put32(q, db->sorts[k][0]);
            ce_put32(q + 4, db->sorts[k][1]);
            q += 8;
        }
        for (unsigned k = 0; k < 32; k++)
            ce_put16(q + k * 2, db->name[k]);
        q += 64;
        ce_put32(q, db->count);
        q += 4;
        for (unsigned j = 0; j < db->count; j++) {
            const struct ce_db_record *r = &db->records[j];
            ce_put32(q, r->oid);
            ce_put32(q + 4, r->count);
            q += 8;
            for (unsigned k = 0; k < r->count; k++) {
                ce_put32(q, r->props[k].id);
                ce_put32(q + 4, r->props[k].size);
                memcpy(q + 8, r->props[k].data, r->props[k].size);
                q += 8 + r->props[k].size;
            }
        }
    }
    *size = n;
    return p;
}
bool ce_db_decode(struct ce_db_store *out, const uint8_t *p, size_t size)
{
    struct ce_db_store s;
    memset(&s, 0, sizeof(s));
    if (size < 12 || size > CE_DB_FILE_MAX || memcmp(p, "CDB2", 4) || ce_u32(p + 8) > CE_DB_MAX)
        return false;
    s.next_oid = ce_u32(p + 4);
    unsigned dbs = ce_u32(p + 8);
    size_t pos = 12;
    for (unsigned i = 0; i < dbs; i++) {
        if (size - pos < 32 + CE_DB_SORTS * 8 + 64 + 4)
            goto bad;
        struct ce_db *db = s.dbs[i] = calloc(1, sizeof(*db));
        if (!db)
            goto bad;
        db->oid = ce_u32(p + pos);
        db->type = ce_u32(p + pos + 4);
        db->sort_count = ce_u32(p + pos + 8);
        db->modified = u64(p + pos + 12);
        pos += 32;
        for (unsigned k = 0; k < CE_DB_SORTS; k++, pos += 8) {
            db->sorts[k][0] = ce_u32(p + pos);
            db->sorts[k][1] = ce_u32(p + pos + 4);
        }
        for (unsigned k = 0; k < 32; k++)
            db->name[k] = ce_u16(p + pos + k * 2);
        pos += 64;
        uint32_t records = ce_u32(p + pos);
        pos += 4;
        if (!db->oid || db->sort_count > CE_DB_SORTS || !valid_name(db->name) || records > (size - pos) / 8 ||
            (ce_db_find(&s, db->oid, NULL) != db))
            goto bad;
        db->records = calloc(records ? records : 1, sizeof(*db->records));
        if (!db->records)
            goto bad;
        db->capacity = records;
        for (uint32_t j = 0; j < records; j++) {
            if (size - pos < 8)
                goto bad;
            struct ce_db_record *r = &db->records[db->count++];
            r->oid = ce_u32(p + pos);
            uint32_t props = ce_u32(p + pos + 4);
            pos += 8;
            if (!r->oid || props > CE_DB_PROPS || props > (size - pos) / 8)
                goto bad;
            r->props = calloc(props ? props : 1, sizeof(*r->props));
            if (!r->props)
                goto bad;
            for (uint32_t k = 0; k < props; k++) {
                if (size - pos < 8)
                    goto bad;
                struct ce_db_prop prop = {ce_u32(p + pos), ce_u32(p + pos + 4), (uint8_t *)p + pos + 8};
                pos += 8;
                if (prop.size > size - pos || set_prop(r, &prop))
                    goto bad;
                pos += prop.size;
            }
        }
    }
    if (pos != size)
        goto bad;
    ce_db_store_clear(out);
    *out = s;
    return true;
bad:
    ce_db_store_clear(&s);
    return false;
}
