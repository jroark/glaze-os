#include "pe.h"
#include <string.h>

uint16_t ce_u16(const void *p)
{
    const uint8_t *b = p;
    return (uint16_t)(b[0] | b[1] << 8);
}
uint32_t ce_u32(const void *p)
{
    const uint8_t *b = p;
    return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
void ce_put16(void *p, uint16_t value)
{
    uint8_t *b = p;
    b[0] = (uint8_t)value;
    b[1] = (uint8_t)(value >> 8);
}
void ce_put32(void *p, uint32_t value)
{
    uint8_t *b = p;
    for (unsigned i = 0; i < 4; i++)
        b[i] = (uint8_t)(value >> (i * 8));
}
bool ce_range(uint32_t off, uint32_t size, uint32_t limit)
{
    return off <= limit && size <= limit - off;
}
static bool power2(uint32_t x)
{
    return x && !(x & (x - 1));
}

const char *ce_pe_parse(struct ce_pe *pe, const void *file, uint32_t size)
{
    const uint8_t *b = file;
    memset(pe, 0, sizeof(*pe));
    if (!b || size < 64 || size > CE_PE_MAX_SIZE || b[0] != 'M' || b[1] != 'Z')
        return "invalid DOS header";
    uint32_t off = ce_u32(b + 60);
    if (!ce_range(off, 24, size) || ce_u32(b + off) != 0x4550)
        return "invalid PE signature";
    const uint8_t *coff = b + off + 4;
    uint16_t machine = ce_u16(coff), count = ce_u16(coff + 2), optional = ce_u16(coff + 16);
    if (machine != 0x166 && machine != 0x169)
        return "requires a BE-300 MIPS executable";
    if (!(ce_u16(coff + 18) & 2))
        return "PE is not executable";
    pe->dll = (ce_u16(coff + 18) & 0x2000) != 0;
    if (!count || count > CE_PE_SECTIONS || optional < 96 || !ce_range(off + 24, optional, size))
        return "invalid PE optional header";
    const uint8_t *o = coff + 20;
    if (ce_u16(o) != 0x10b || ce_u16(o + 68) != 9)
        return "requires PE32 Windows CE GUI subsystem";
    uint32_t alignment = ce_u32(o + 32), file_alignment = ce_u32(o + 36);
    pe->file = file;
    pe->bytes = size;
    pe->base = ce_u32(o + 28);
    pe->entry = ce_u32(o + 16);
    pe->size = ce_u32(o + 56);
    pe->headers = ce_u32(o + 60);
    pe->stack = ce_u32(o + 72);
    pe->count = count;
    if (!power2(alignment) || alignment < 4096 || !power2(file_alignment) || file_alignment > alignment ||
        pe->base < 0x10000 || (pe->base & 4095) || !pe->size || pe->size > CE_PE_MAX_SIZE ||
        (pe->size & (alignment - 1)) || !ce_range(pe->base, pe->size, 0x80000000u) || !pe->headers ||
        pe->headers > size || pe->headers > pe->size || pe->entry >= pe->size)
        return "invalid PE memory layout";
    uint32_t dirs = ce_u32(o + 92);
    if (dirs > CE_PE_DIRECTORIES || dirs > (optional - 96u) / 8u)
        return "invalid PE directory count";
    for (unsigned i = 0; i < dirs; i++) {
        pe->directory[i].rva = ce_u32(o + 96 + i * 8);
        pe->directory[i].size = ce_u32(o + 100 + i * 8);
        if (pe->directory[i].size &&
            (!pe->directory[i].rva ||
             !ce_range(pe->directory[i].rva, pe->directory[i].size, i == 4 ? size : pe->size)))
            return "PE directory outside image";
    }
    if (!ce_range(pe->base, pe->size, (pe->dll && pe->directory[5].size) ? 0x80000000u : 0x0f000000u))
        return "invalid PE memory layout";
    if (pe->directory[9].size || pe->directory[13].size || pe->directory[14].size)
        return "PE TLS, delay imports, or managed code are unsupported";
    uint32_t sections = off + 24 + optional;
    if (!ce_range(sections, count * 40u, pe->headers))
        return "section table outside headers";
    bool entry = false;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *s = b + sections + i * 40;
        struct ce_section *d = &pe->section[i];
        d->size = ce_u32(s + 8);
        d->rva = ce_u32(s + 12);
        d->bytes = ce_u32(s + 16);
        d->raw = ce_u32(s + 20);
        d->flags = ce_u32(s + 36);
        uint32_t extent = MAX(d->size, d->bytes);
        if ((d->rva & (alignment - 1)) || d->rva < ALIGN_UP(pe->headers, alignment) ||
            !ce_range(d->rva, extent, pe->size) || !ce_range(d->raw, d->bytes, size) ||
            (d->bytes && (d->raw < pe->headers || (d->raw & (file_alignment - 1)))) ||
            (d->flags & 0xa0000000u) == 0xa0000000u)
            return "invalid PE section";
        for (unsigned j = 0; j < i; j++) {
            const struct ce_section *p = &pe->section[j];
            uint32_t end = p->rva + ALIGN_UP(MAX(p->size, p->bytes), alignment);
            if (extent && d->rva < end && d->rva + ALIGN_UP(extent, alignment) > p->rva)
                return "overlapping PE sections";
        }
        if ((d->flags & 0x20000000u) && pe->entry >= d->rva && pe->entry - d->rva < extent)
            entry = true;
    }
    return entry || (pe->dll && !pe->entry) ? NULL : "PE entry is not executable";
}

void *ce_pe_at(const struct ce_pe *pe, void *image, uint32_t rva, uint32_t size)
{
    if (!image || !ce_range(rva, size, pe->size))
        return NULL;
    if (ce_range(rva, size, pe->headers))
        return (uint8_t *)image + rva;
    for (unsigned i = 0; i < pe->count; i++) {
        const struct ce_section *s = &pe->section[i];
        if (rva >= s->rva && ce_range(rva - s->rva, size, MAX(s->size, s->bytes)))
            return (uint8_t *)image + rva;
    }
    return NULL;
}

const char *ce_pe_string(const struct ce_pe *pe, const void *image, uint32_t rva)
{
    const char *start = ce_pe_at(pe, (void *)image, rva, 1);
    if (!start)
        return NULL;
    for (unsigned i = 0; i < 256; i++) {
        const char *c = ce_pe_at(pe, (void *)image, rva + i, 1);
        if (!c)
            return NULL;
        if (!*c)
            return start;
        if ((uint8_t)*c < 32 || (uint8_t)*c > 126)
            return NULL;
    }
    return NULL;
}

const char *ce_pe_copy(const struct ce_pe *pe, void *image)
{
    if (!image)
        return "no image memory";
    memset(image, 0, pe->size);
    memcpy(image, pe->file, pe->headers);
    for (unsigned i = 0; i < pe->count; i++) {
        const struct ce_section *s = &pe->section[i];
        memcpy((uint8_t *)image + s->rva, pe->file + s->raw, s->bytes);
    }
    return NULL;
}

const char *ce_pe_relocate(const struct ce_pe *pe, void *image, uint32_t base)
{
    if (base == pe->base)
        return NULL;
    if (base & 4095 || !ce_range(base, pe->size, 0x0f000000))
        return "invalid PE relocation base";
    uint32_t delta = base - pe->base, pos = pe->directory[5].rva, left = pe->directory[5].size;
    if (!left)
        return "PE has no relocations; preferred address required";
    while (left) {
        uint8_t *block = ce_pe_at(pe, image, pos, left);
        if (!block || left < 8)
            return "invalid PE relocation block";
        uint32_t page = ce_u32(block), bytes = ce_u32(block + 4);
        if (bytes < 8 || bytes > left || (bytes & 1) || (page & 4095))
            return "invalid PE relocation size";
        for (uint32_t i = 8; i < bytes; i += 2) {
            uint32_t value = ce_u16(block + i), type = value >> 12, off = value & 4095;
            if (!type)
                continue;
            if (page > UINT32_MAX - off)
                return "PE relocation address overflow";
            if (type == 1 || type == 2 || type == 4) {
                uint8_t *target = ce_pe_at(pe, image, page + off, 2);
                if (!target || (off & 1))
                    return "PE relocation outside section";
                if (type == 4) {
                    if (i + 2 >= bytes)
                        return "invalid PE relocation size";
                    int16_t lo = (int16_t)ce_u16(block + i + 2);
                    i += 2;
                    uint32_t addr = ((uint32_t)ce_u16(target) << 16) + (uint32_t)(int32_t)lo + delta;
                    ce_put16(target, (uint16_t)((addr + 0x8000u) >> 16));
                } else if (type == 1) {
                    uint32_t addr = ((uint32_t)ce_u16(target) << 16) + delta;
                    ce_put16(target, (uint16_t)(addr >> 16));
                } else {
                    ce_put16(target, (uint16_t)(ce_u16(target) + delta));
                }
                continue;
            }
            if (type != 3 && type != 5)
                return "unsupported PE relocation type";
            uint8_t *target = ce_pe_at(pe, image, page + off, 4);
            if (!target || (off & 3))
                return "PE relocation outside section";
            uint32_t word = ce_u32(target);
            if (type == 3)
                ce_put32(target, word + delta);
            else {
                if ((word >> 26) != 2 && (word >> 26) != 3)
                    return "invalid MIPS jump relocation";
                uint32_t orig_pc = pe->base + page + off + 4;
                uint32_t address = ((orig_pc & 0xf0000000u) | ((word & 0x03ffffffu) << 2)) + delta;
                if ((base + page + off + 4) >> 28 != address >> 28)
                    return "MIPS jump relocation crosses region";
                ce_put32(target, (word & 0xfc000000u) | ((address >> 2) & 0x03ffffffu));
            }
        }
        pos += bytes;
        left -= bytes;
    }
    return NULL;
}

const char *ce_pe_imports(const struct ce_pe *pe, void *image, ce_resolver resolve, void *arg)
{
    uint32_t pos = pe->directory[1].rva, bytes = pe->directory[1].size;
    if (!bytes)
        return NULL;
    for (uint32_t i = 0; ce_range(i, 20, bytes); i += 20) {
        uint8_t *d = ce_pe_at(pe, image, pos + i, 20);
        if (!d)
            return "PE import directory outside section";
        uint32_t original = ce_u32(d), name = ce_u32(d + 12), first = ce_u32(d + 16);
        if (!(original | name | first | ce_u32(d + 4) | ce_u32(d + 8)))
            return NULL;
        const char *dll = ce_pe_string(pe, image, name);
        if (!dll || !first || (first & 3) || (original & 3))
            return "invalid PE import descriptor";
        uint32_t lookup = original ? original : first;
        bool terminated = false;
        for (unsigned j = 0; j < 4096; j++) {
            if (lookup > UINT32_MAX - j * 4u || first > UINT32_MAX - j * 4u)
                return "PE import address overflow";
            uint8_t *src = ce_pe_at(pe, image, lookup + j * 4, 4);
            uint8_t *dst = ce_pe_at(pe, image, first + j * 4, 4);
            if (!src || !dst)
                return "PE import thunk outside section";
            uint32_t value = ce_u32(src);
            if (!value) {
                terminated = true;
                break;
            }
            const char *symbol = NULL;
            uint16_t ordinal = 0;
            if (value & 0x80000000u) {
                if (value & 0x7fff0000u)
                    return "invalid PE import ordinal";
                ordinal = (uint16_t)value;
            } else {
                if (value > UINT32_MAX - 2 || !ce_pe_at(pe, image, value, 2) ||
                    !(symbol = ce_pe_string(pe, image, value + 2)))
                    return "invalid PE import name";
            }
            uint32_t address = resolve(dll, symbol, ordinal, arg);
            if (!address)
                return "unsupported WinCE import (see serial log)";
            ce_put32(dst, address);
        }
        if (!terminated)
            return "unterminated PE import thunks";
    }
    return "unterminated PE import directory";
}

/* The CE 3 MIPS CRT embeds an implicit TerminateProcess call instead of an
 * import: li r,-3078; [li a0,0x42]; jalr r. The matching local COREDLL export
 * uses that same -3078 trap address. Translate only these complete sequences
 * in executable sections of the private loaded image, before publication.
 * JAL preserves the original return address and delay slot. No source bytes
 * are changed; other implicit calls remain unsupported. */
const char *ce_pe_direct_calls(const struct ce_pe *pe, void *image, ce_resolver resolve, void *arg)
{
    uint32_t target = 0;
    for (unsigned s = 0; s < pe->count; s++) {
        const struct ce_section *section = &pe->section[s];
        if (!(section->flags & 0x20000000u))
            continue;
        uint32_t length = MIN(section->size, section->bytes);
        for (uint32_t off = 0; ce_range(off, 12, length); off += 4) {
            uint8_t *p = ce_pe_at(pe, image, section->rva + off, 12);
            if (!p)
                return "invalid direct-call section";
            uint32_t load = ce_u32(p), reg = (load >> 16) & 31;
            if (!reg || reg == 31 || (load & 0xffe0ffffu) != 0x2400f3fau)
                continue;
            uint32_t call = 4;
            if (ce_u32(p + 4) == 0x24040042u && reg != 4) {
                if (!ce_range(off, 16, length))
                    continue;
                call = 8;
            }
            if (ce_u32(p + call) != ((reg << 21) | 0x0000f809u))
                continue;
            if (!target)
                target = resolve("coredll.dll", "TerminateProcess", 0, arg);
            uint32_t pc = pe->base + section->rva + off + call;
            if (!target || (target & 3) || ((pc + 4) >> 28) != (target >> 28))
                return "cannot bridge direct WinCE termination call";
            ce_put32(p + call, 0x0c000000u | ((target >> 2) & 0x03ffffffu));
        }
    }
    return NULL;
}

const void *ce_pe_resource(const struct ce_pe *pe, void *image, uint16_t type, uint16_t id, uint32_t *bytes)
{
    uint32_t root = pe->directory[2].rva, limit = pe->directory[2].size, off = 0;
    *bytes = 0;
    for (unsigned depth = 0; depth < 3; depth++) {
        if (!ce_range(off, 16, limit))
            return NULL;
        uint8_t *dir = ce_pe_at(pe, image, root + off, 16);
        if (!dir)
            return NULL;
        uint32_t named = ce_u16(dir + 12), ids = ce_u16(dir + 14);
        if (!ce_range(off + 16, (named + ids) * 8, limit))
            return NULL;
        uint8_t *entries = ce_pe_at(pe, image, root + off + 16, (named + ids) * 8);
        if (!entries)
            return NULL;
        bool found = false;
        for (uint32_t i = named; i < named + ids; i++) {
            uint32_t key = ce_u32(entries + i * 8), value = ce_u32(entries + i * 8 + 4);
            if (depth < 2 && key != (depth == 0 ? type : id))
                continue;
            if (depth < 2 && !(value & 0x80000000u))
                return NULL;
            if (depth == 2 && (value & 0x80000000u))
                return NULL;
            off = value & 0x7fffffffu;
            found = true;
            break;
        }
        if (!found)
            return NULL;
    }
    if (!ce_range(off, 16, limit))
        return NULL;
    uint8_t *entry = ce_pe_at(pe, image, root + off, 16);
    if (!entry)
        return NULL;
    uint32_t size = ce_u32(entry + 4);
    const void *data = ce_pe_at(pe, image, ce_u32(entry), size);
    if (data)
        *bytes = size;
    return data;
}
