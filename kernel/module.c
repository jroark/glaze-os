/* Load trusted, non-PIC ELF32/MIPS ET_REL drivers from romfs. No user-provided
 * paths or executable bytes enter this loader. All failures release the image.
 * Relocation formulas follow the MIPS ELF ABI (REL implicit addends).
 */
#include "kernel.h"
#include "module.h"
#include "drivers.h"
#include <glaze/elf.h>

#define SECTIONS 64u
#define IMAGE_MAX (128u * 1024u)
#define FILE_MAX (256u * 1024u)
#define SH_ALLOC 2u
#define SH_EXEC 4u
struct section {
    uint32_t name, type, flags, addr, offset, size, link, info, align, entsize;
};
/* Compiler helpers are shared with the kernel, including VR41xx errata builds. */
extern uint32_t __udivsi3(uint32_t, uint32_t), __umodsi3(uint32_t, uint32_t);
extern int32_t __divsi3(int32_t, int32_t), __modsi3(int32_t, int32_t);
extern int32_t __vr4120_divsi3(int32_t, int32_t), __vr4120_modsi3(int32_t, int32_t);
extern uint64_t __udivdi3(uint64_t, uint64_t), __umoddi3(uint64_t, uint64_t);
extern int64_t __divdi3(int64_t, int64_t), __moddi3(int64_t, int64_t);
#ifndef MODULE_TEST
static uintptr_t module_resolve(const char *name)
{
#define EXPORT(f) if (!strcmp(name, #f)) return (uintptr_t)(f)
    EXPORT(memcpy); EXPORT(memmove); EXPORT(memset); EXPORT(memcmp);
    EXPORT(kputs); EXPORT(kprintf); EXPORT(time_us);
    EXPORT(romfs_lookup); EXPORT(romfs_read);
    EXPORT(copy_to_as); EXPORT(copy_from_as); EXPORT(ipc_notify);
    EXPORT(pcmcia_node_id); EXPORT(net_set_mac); EXPORT(net_receive);
    EXPORT(net_rx_drop); EXPORT(net_overrun);
    EXPORT(__udivsi3); EXPORT(__umodsi3); EXPORT(__divsi3); EXPORT(__modsi3);
    EXPORT(__vr4120_divsi3); EXPORT(__vr4120_modsi3);
    EXPORT(__udivdi3); EXPORT(__umoddi3); EXPORT(__divdi3); EXPORT(__moddi3);
#undef EXPORT
    return 0;
}
#endif

bool module_contains(const struct module *m, const void *p, uint32_t size)
{
    uintptr_t start = (uintptr_t)m->base, at = (uintptr_t)p;
    return m->base && at >= start && at - start <= m->bytes && size <= m->bytes - (at - start);
}

bool module_executable(const struct module *m, const void *p)
{
    uintptr_t offset = (uintptr_t)p - (uintptr_t)m->base;
    return !((uintptr_t)p & 3u) && module_contains(m, p, 4) &&
        offset <= m->text_bytes && 4 <= m->text_bytes - offset;
}

void module_unload(struct module *m)
{
    if (m->base) {
        memset(m->base, 0, m->pages * PAGE_SIZE);
        for (uint32_t i = 0; i < m->pages; i++)
            page_free((uint8_t *)m->base + i * PAGE_SIZE);
    }
    memset(m, 0, sizeof(*m));
}

static bool symbol_value(const struct gz_elf_sym *sym, const struct section *sh, unsigned count,
                         const uint32_t *offset, const char *strings, unsigned string_size,
                         const struct module *m, uint32_t *value)
{
    if (sym->shndx == 0xfff1u) { *value = sym->value; return true; } /* SHN_ABS */
    if (!sym->shndx) {
        if (sym->name >= string_size || !memchr(strings + sym->name, 0, string_size - sym->name))
            return false;
        if (!sym->name) { *value = 0; return true; }
        uintptr_t address = module_resolve(strings + sym->name);
        if (!address && (sym->info >> 4) != 2) {
            kprintf("module: unresolved import %s\n", strings + sym->name);
            return false;
        }
        *value = (uint32_t)address;
        return true;
    }
    if (sym->shndx >= count || !(sh[sym->shndx].flags & SH_ALLOC) ||
        sym->value > sh[sym->shndx].size || sym->size > sh[sym->shndx].size - sym->value)
        return false;
    *value = (uint32_t)(uintptr_t)m->base + offset[sym->shndx] + sym->value;
    return true;
}

static bool relocate(uint32_t *at, uint32_t type, uint32_t value, uint32_t low)
{
    uint32_t word = *at, target;
    switch (type) {
    case 0: return true;
    case 2: *at = word + value; return true;                  /* R_MIPS_32 */
    case 4:                                                  /* R_MIPS_26 */
        target = ((word & 0x03ffffffu) << 2) + value;
        if ((target & 3u) || ((target ^ ((uint32_t)(uintptr_t)at + 4)) & 0xf0000000u))
            return false;
        *at = (word & 0xfc000000u) | ((target >> 2) & 0x03ffffffu);
        return true;
    case 5:                                                  /* R_MIPS_HI16 */
        target = ((word & 0xffffu) << 16) + (int16_t)low + value;
        *at = (word & 0xffff0000u) | (((target + 0x8000u) >> 16) & 0xffffu);
        return true;
    case 6:                                                  /* R_MIPS_LO16 */
        *at = (word & 0xffff0000u) | (((int16_t)word + value) & 0xffffu);
        return true;
    default: return false;
    }
}

int module_load(const char *path, const char *entry_name, struct module *m)
{
    const struct romfs_entry *rom = romfs_lookup(path);
    uint8_t *file = NULL;
    uint32_t file_pages = 0, offsets[SECTIONS] = {0}, bytes = 0;
    struct section sh[SECTIONS];
    int result = GZ_ECORRUPT;
    if (m->base) return GZ_EBUSY;
    if (!rom) return GZ_ENOENT;
    if (rom->size < sizeof(struct gz_elf_ehdr) || rom->size > FILE_MAX) return GZ_ECORRUPT;
    file_pages = ALIGN_UP(rom->size, PAGE_SIZE) / PAGE_SIZE;
    file = page_alloc_n(file_pages);
    if (!file) return GZ_ENOMEM;
    if (romfs_read(rom, 0, file, rom->size)) goto out;
    const struct gz_elf_ehdr *e = (const void *)file;
    if (memcmp(e->ident, "\177ELF\1\1\1", 7) || e->type != 1 || e->machine != 8 || e->version != 1 ||
        e->ehsize != sizeof(*e) || e->phnum || e->shentsize != sizeof(sh[0]) || !e->shnum ||
        e->shnum > SECTIONS || (e->shoff & 3u) ||
        !gz_elf_range(e->shoff, e->shnum * sizeof(sh[0]), rom->size) ||
        (e->flags & 0xf0000000u) > 0x20000000u || (e->flags & 0x06000066u) ||
        ((e->flags & 0xf000u) && (e->flags & 0xf000u) != 0x1000u)) goto out;
    memcpy(sh, file + e->shoff, e->shnum * sizeof(sh[0]));
    int symtab = -1;
    for (unsigned i = 0; i < e->shnum; i++) {
        struct section *s = &sh[i];
        if (s->type != 8 && !gz_elf_range(s->offset, s->size, rom->size)) goto out;
        if (s->type == 2) { if (symtab >= 0) goto out; symtab = (int)i; }
        if ((s->flags & SH_ALLOC) &&
            ((s->type != 1 && s->type != 8) || s->align > PAGE_SIZE ||
             (s->align && (s->align & (s->align - 1))) || s->size > IMAGE_MAX ||
             (s->type == 8 && (s->flags & SH_EXEC)))) goto out;
    }
    /* Code first: callbacks must land inside text, not just allocated data. */
    for (unsigned phase = 0; phase < 2; phase++) {
        for (unsigned i = 0; i < e->shnum; i++) {
            const struct section *s = &sh[i];
            if (!(s->flags & SH_ALLOC) || !!(s->flags & SH_EXEC) != !phase) continue;
            bytes = ALIGN_UP(bytes, MAX(s->align, 1u));
            if (bytes > IMAGE_MAX - s->size) goto out;
            offsets[i] = bytes;
            bytes += s->size;
        }
        if (!phase) m->text_bytes = bytes;
    }
    if (symtab < 0 || !bytes) goto out;
    const struct section *symbols = &sh[symtab];
    if (symbols->entsize != sizeof(struct gz_elf_sym) || (symbols->offset & 3u) ||
        symbols->size % sizeof(struct gz_elf_sym) || symbols->link >= e->shnum ||
        sh[symbols->link].type != 3) goto out;
    const struct gz_elf_sym *syms = (const void *)(file + symbols->offset);
    unsigned nsym = symbols->size / sizeof(*syms);
    const char *strings = (const void *)(file + sh[symbols->link].offset);
    unsigned string_size = sh[symbols->link].size;
    if (!nsym || nsym > 4096 || !string_size) goto out;
    m->bytes = bytes;
    m->pages = ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE;
    m->base = page_alloc_n(m->pages);
    if (!m->base) { result = GZ_ENOMEM; goto out; }
    for (unsigned i = 0; i < e->shnum; i++)
        if ((sh[i].flags & SH_ALLOC) && sh[i].type == 1)
            memcpy((uint8_t *)m->base + offsets[i], file + sh[i].offset, sh[i].size);
    for (unsigned i = 0; i < e->shnum; i++) {
        const struct section *r = &sh[i];
        if (r->type == 4) goto out;                           /* no RELA */
        if (r->type != 9) continue;
        if (r->info >= e->shnum || r->link != (uint32_t)symtab ||
            r->entsize != sizeof(struct gz_elf_rel) || (r->offset & 3u) || r->size % r->entsize) goto out;
        if (!(sh[r->info].flags & SH_ALLOC)) continue;
        const struct gz_elf_rel *rel = (const void *)(file + r->offset);
        unsigned count = r->size / sizeof(*rel);
        if (count > 8192) goto out;
        for (unsigned j = 0; j < count; j++) {
            unsigned symbol = rel[j].info >> 8, type = rel[j].info & 255;
            uint32_t value, low = 0;
            if (symbol >= nsym || ((offsets[r->info] + rel[j].offset) & 3u) ||
                !gz_elf_range(rel[j].offset, 4, sh[r->info].size) ||
                !symbol_value(&syms[symbol], sh, e->shnum, offsets, strings, string_size, m, &value)) goto out;
            /* Several HI16 records may share a later LO16; never consume it. */
            if (type == 5) {
                unsigned k;
                for (k = j + 1; k < count; k++)
                    if ((rel[k].info & 255) == 6 && (rel[k].info >> 8) == symbol) break;
                if (k == count || (rel[k].offset & 3u) || !gz_elf_range(rel[k].offset, 4, sh[r->info].size)) goto out;
                memcpy(&low, (uint8_t *)m->base + offsets[r->info] + rel[k].offset, 4);
            }
            if (!relocate((void *)((uint8_t *)m->base + offsets[r->info] + rel[j].offset), type, value, low)) goto out;
        }
    }
    for (unsigned i = 1; i < nsym; i++) {
        if (syms[i].name >= string_size || !memchr(strings + syms[i].name, 0, string_size - syms[i].name)) goto out;
        if (!strcmp(strings + syms[i].name, entry_name)) {
            uint32_t value;
            if (m->entry || !syms[i].shndx || syms[i].shndx >= e->shnum ||
                !(sh[syms[i].shndx].flags & SH_ALLOC) ||
                !symbol_value(&syms[i], sh, e->shnum, offsets, strings, string_size, m, &value)) goto out;
            m->entry = (void *)(uintptr_t)value;
        }
    }
    if (!m->entry || ((uintptr_t)m->entry & 3u)) goto out;
    mm_sync_code(m->base, m->pages * PAGE_SIZE);
    result = 0;
    kprintf("module: loaded %s (%u pages)\n", path, m->pages);
out:
    if (result) module_unload(m);
    for (uint32_t i = 0; i < file_pages; i++) page_free(file + i * PAGE_SIZE);
    return result;
}
