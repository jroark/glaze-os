#ifndef CE_REGISTRY_H
#define CE_REGISTRY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Sized for third-party BE-300 images (BeeV2: 1069 keys, 2612 values). */
#define CE_REG_KEYS 2048
#define CE_REG_VALUES 4096
#define CE_REG_FILE_MAX (1024u * 1024u)
struct ce_reg_value {
    char *key; /* owned copy, below 256 bytes */
    char *name; /* owned, below 128 bytes */
    uint32_t type, size;
    uint8_t *bytes;
};
struct ce_registry {
    char *keys[CE_REG_KEYS]; /* owned paths, below 256 bytes */
    unsigned key_count, value_count;
    uint32_t encoded; /* ce_reg_encode's size less its 16 fixed bytes */
    struct ce_reg_value values[CE_REG_VALUES];
};
void ce_reg_clear(struct ce_registry *);
bool ce_reg_exists(const struct ce_registry *, const char *);
int ce_reg_create(struct ce_registry *, const char *);
int ce_reg_delete(struct ce_registry *, const char *);
int ce_reg_delete_tree(struct ce_registry *, const char *);
const char *ce_reg_child(const struct ce_registry *, const char *, unsigned);
struct ce_reg_value *ce_reg_value_at(struct ce_registry *, const char *, unsigned);
struct ce_reg_value *ce_reg_find(struct ce_registry *, const char *, const char *);
int ce_reg_set(struct ce_registry *, const char *, const char *, uint32_t, const void *, uint32_t);
int ce_reg_remove_value(struct ce_registry *, const char *, const char *);
bool ce_reg_decode(struct ce_registry *, const void *, size_t);
uint8_t *ce_reg_encode(const struct ce_registry *, uint32_t *);
/* Merges a Casio/CE registry backup (\Backup\System.reg) into a registry:
 * HKCR, HKCU and HKLM keys and values, without HKLM\Drivers. Entries beyond
 * the registry's limits are skipped; false if the stream is malformed. */
bool ce_reg_import_casio(struct ce_registry *, const void *, size_t);
#endif
