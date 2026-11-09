/* The Windows CE object-store database model behind apps/wince/database.c:
 * databases of records holding typed properties, sort orders, seeks and the
 * on-disk format. Pure C over host pointers, so it is tested natively.
 * CE 3.0 semantics: CeCreateDatabase, CeWriteRecordProps, CeSeekDatabase
 * (Platform Builder 3.0 "Database Functions"; PROPID = id << 16 | CEVT_*). */
#ifndef WINCE_DBSTORE_H
#define WINCE_DBSTORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CE_DB_MAX 64
#define CE_DB_SORTS 4
#define CE_DB_PROPS 256          /* properties per record */
#define CE_DB_PROP_BYTES 65536u  /* one string or blob */
#define CE_DB_FILE_MAX (8u * 1024u * 1024u)

/* CEVT_* property types, the PROPID's low word. */
enum { CEVT_I2 = 2, CEVT_I4 = 3, CEVT_R8 = 5, CEVT_BOOL = 11, CEVT_UI2 = 18, CEVT_UI4 = 19,
       CEVT_LPWSTR = 31, CEVT_FILETIME = 64, CEVT_BLOB = 65 };
/* SORTORDERSPEC flags. */
enum { CEDB_SORT_DESCENDING = 1, CEDB_SORT_CASEINSENSITIVE = 2, CEDB_SORT_UNKNOWNFIRST = 4 };
/* CeSeekDatabase types. */
enum { CEDB_SEEK_CEOID = 1, CEDB_SEEK_BEGINNING = 2, CEDB_SEEK_END = 4, CEDB_SEEK_CURRENT = 8,
       CEDB_SEEK_VALUESMALLER = 0x10, CEDB_SEEK_VALUEFIRSTEQUAL = 0x20, CEDB_SEEK_VALUEGREATER = 0x40,
       CEDB_SEEK_VALUENEXTEQUAL = 0x80, CEDB_SEEK_VALUEGREATEREQUAL = 0x100 };

/* A property: scalars keep CEPROPVAL's 8-byte value; a string its UTF-16
 * units with the terminator; a blob its bytes. */
struct ce_db_prop {
    uint32_t id, size;
    uint8_t *data;
};
struct ce_db_record {
    uint32_t oid;
    unsigned count;
    struct ce_db_prop *props;
};
struct ce_db {
    uint32_t oid, type;
    uint16_t name[32];
    unsigned sort_count;
    uint32_t sorts[CE_DB_SORTS][2]; /* propid, flags */
    uint64_t modified;              /* FILETIME */
    unsigned count, capacity;
    struct ce_db_record *records;
    uint32_t version;               /* bumped on every change */
};
struct ce_db_store {
    struct ce_db *dbs[CE_DB_MAX];
    uint32_t next_oid;
};

void ce_db_store_clear(struct ce_db_store *);
struct ce_db *ce_db_find(struct ce_db_store *, uint32_t oid, const uint16_t *name);
/* The database or record with this OID; *record is -1 for a database. */
struct ce_db *ce_db_owner(struct ce_db_store *, uint32_t oid, int *record);
/* Win32 error codes: 0, 87 invalid, 183 exists (CE reports 52), 8 memory. */
int ce_db_create(struct ce_db_store *, const uint16_t *name, uint32_t type, unsigned sorts,
                 const uint32_t (*specs)[2], uint32_t *oid);
int ce_db_delete(struct ce_db_store *, uint32_t oid);
int ce_db_rename(struct ce_db_store *, struct ce_db *, const uint16_t *name);
/* `props` carries CEDB_PROPDELETE (0x200) in a prop's size high bit instead
 * of data: see CE_DB_DELETE. A zero `oid` adds a record. */
#define CE_DB_DELETE 0x80000000u
int ce_db_write(struct ce_db_store *, struct ce_db *, uint32_t oid, const struct ce_db_prop *props,
                unsigned count, uint32_t *written);
int ce_db_delete_record(struct ce_db *, uint32_t oid);
int ce_db_index(const struct ce_db *, uint32_t oid);
const struct ce_db_prop *ce_db_prop(const struct ce_db_record *, uint32_t id);
uint32_t ce_db_record_bytes(const struct ce_db_record *);
/* Records' indices in the order of sort property `sort` (0: as stored),
 * with that sort's flags; `order` holds db->count entries. */
void ce_db_sort(const struct ce_db *, uint32_t sort, unsigned *order);
/* Compares a record's value of `sort` with a value (scalar/string/blob as
 * stored); missing or mistyped values are "unknown". */
int ce_db_compare_value(const struct ce_db *, uint32_t sort, const struct ce_db_record *,
                        const struct ce_db_prop *value);
/* The new position after a seek, or -1; `position` is the current one. */
int ce_db_seek(const struct ce_db *, uint32_t sort, const unsigned *order, int position, uint32_t type,
               uint32_t value, const struct ce_db_prop *key);

uint8_t *ce_db_encode(const struct ce_db_store *, uint32_t *size);
bool ce_db_decode(struct ce_db_store *, const uint8_t *, size_t);
#endif
