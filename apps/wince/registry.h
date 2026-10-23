#ifndef CE_REGISTRY_H
#define CE_REGISTRY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CE_REG_KEYS 128
#define CE_REG_VALUES 128
#define CE_REG_FILE_MAX (256u * 1024u)
struct ce_reg_value {
    char key[256], name[128];
    uint32_t type, size;
    uint8_t *bytes;
};
struct ce_registry {
    char keys[CE_REG_KEYS][256];
    unsigned key_count, value_count;
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
#endif
