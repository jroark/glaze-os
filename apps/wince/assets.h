/* Readers for locally supplied CE NAND/CCFS assets. No vendor data is built in. */
#ifndef WINCE_ASSETS_H
#define WINCE_ASSETS_H
#include <glaze/types.h>
#define CE_ASSET_MAX (4u * 1024u * 1024u)
#define CE_ORDINAL_MAX (128u * 1024u)
#define CE_DICT_MAX (32u * 1024u)
#define CE_ROM_MODULES_MAX (1024u * 1024u)
#define CE_ROM_RECORD_HEADER 128u
struct ce_nk_assets {
    char *ordinals;
    uint32_t ordinal_bytes;
    uint8_t *dictionary;
    uint32_t dictionary_bytes;
    /* Records of the application-level Casio ROM DLLs (pimdb, NFC, ...), as
     * stored in NK: e32/o32 fields and section bytes, still compressed. */
    uint8_t *modules;
    uint32_t module_bytes;
    uint32_t checksum_mismatches; /* audited NK records */
};
/* Reader returns exactly size bytes, or a negative error. */
typedef int (*ce_asset_reader)(void *, uint32_t offset, void *, uint32_t size);
typedef void (*ce_asset_progress)(void *, uint32_t done, uint32_t total);
const char *ce_nk_recover(ce_asset_reader, void *, ce_asset_progress, struct ce_nk_assets *);
void ce_nk_assets_free(struct ce_nk_assets *);
const char *ce_cpk_unpack(const void *, uint32_t, const void *, uint32_t, void **, uint32_t *);
/* Windows CE BinaryDecompress data (romimage's IMAGE_SCN_COMPRESSED sections):
 * a 24-bit size, 24-bit block end offsets, then LZ blocks of 4 KiB output. */
const char *ce_rom_decompress(const uint8_t *, uint32_t, uint8_t *, uint32_t capacity, uint32_t *produced);
/* An owned in-memory PE32/MIPS DLL built from the named cached ROM module,
 * at its ROM base address, so the ordinary loader maps and binds it. */
const char *ce_rom_module_pe(const uint8_t *modules, uint32_t bytes, const char *name, void **, uint32_t *);
#endif
