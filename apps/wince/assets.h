/* Readers for locally supplied CE NAND/CCFS assets. No vendor data is built in. */
#ifndef WINCE_ASSETS_H
#define WINCE_ASSETS_H
#include <glaze/types.h>
#define CE_ASSET_MAX (4u * 1024u * 1024u)
#define CE_ORDINAL_MAX (128u * 1024u)
#define CE_DICT_MAX (32u * 1024u)
struct ce_nk_assets {
    char *ordinals;
    uint32_t ordinal_bytes;
    uint8_t *dictionary;
    uint32_t dictionary_bytes;
    uint32_t checksum_mismatches; /* audited NK records; code is never executed */
};
/* Reader returns exactly size bytes, or a negative error. */
typedef int (*ce_asset_reader)(void *, uint32_t offset, void *, uint32_t size);
typedef void (*ce_asset_progress)(void *, uint32_t done, uint32_t total);
const char *ce_nk_recover(ce_asset_reader, void *, ce_asset_progress, struct ce_nk_assets *);
void ce_nk_assets_free(struct ce_nk_assets *);
const char *ce_cpk_unpack(const void *, uint32_t, const void *, uint32_t, void **, uint32_t *);
#endif
