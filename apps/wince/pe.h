/* The BE-300 PE32/MIPS subset. Byte reads avoid unaligned MIPS accesses.
 * Layouts: Microsoft PE/COFF specification (PE Format), optional header,
 * section table, imports, base relocations, and resource directory. */
#ifndef WINCE_PE_H
#define WINCE_PE_H

#include <glaze/types.h>

#define CE_PE_MAX_SIZE (4u * 1024u * 1024u)
#define CE_PE_SECTIONS 32
#define CE_PE_DIRECTORIES 16

struct ce_section {
    uint32_t rva, size, raw, bytes, flags;
};
struct ce_pe {
    const uint8_t *file;
    uint32_t bytes, base, size, entry, headers, count, stack;
    bool dll;
    struct ce_section section[CE_PE_SECTIONS];
    struct {
        uint32_t rva, size;
    } directory[CE_PE_DIRECTORIES];
};

uint16_t ce_u16(const void *);
uint32_t ce_u32(const void *);
void ce_put32(void *, uint32_t);
bool ce_range(uint32_t, uint32_t, uint32_t);
const char *ce_pe_parse(struct ce_pe *, const void *, uint32_t);
/* All loaded RVA access must pass through this function, including strings. */
void *ce_pe_at(const struct ce_pe *, void *image, uint32_t rva, uint32_t bytes);
const char *ce_pe_string(const struct ce_pe *, const void *image, uint32_t rva);
const char *ce_pe_copy(const struct ce_pe *, void *image);
const char *ce_pe_relocate(const struct ce_pe *, void *image, uint32_t base);
typedef uint32_t (*ce_resolver)(const char *dll, const char *name, uint16_t ordinal, void *);
const char *ce_pe_imports(const struct ce_pe *, void *image, ce_resolver, void *);
const void *ce_pe_resource(const struct ce_pe *, void *image, uint16_t type, uint16_t id, uint32_t *bytes);

#endif
