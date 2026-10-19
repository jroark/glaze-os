/* Static ELF32 little-endian MIPS loader. */

#include "kernel.h"
#include "proc.h"

struct elf32_ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf32_phdr {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

#define PT_LOAD 1
#define EM_MIPS 8

int elf_load(struct addrspace *as, const void *image, uint32_t size, uint32_t *entry)
{
    const struct elf32_ehdr *eh = image;

    if (size < sizeof(*eh) || memcmp(eh->ident, "\177ELF\1\1", 6) || eh->machine != EM_MIPS)
        return GZ_EINVAL;
    if (eh->phoff + eh->phnum * sizeof(struct elf32_phdr) > size)
        return GZ_EINVAL;

    const struct elf32_phdr *ph = (const void *)((const uint8_t *)image + eh->phoff);
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD || !ph[i].memsz)
            continue;
        if (ph[i].vaddr < USER_TEXT_BASE || ph[i].vaddr + ph[i].memsz > USER_MMAP_BASE ||
            ph[i].offset + ph[i].filesz > size || ph[i].filesz > ph[i].memsz)
            return GZ_EINVAL;
        if (as_alloc_region(as, ph[i].vaddr, ph[i].memsz, REGION_OWNED))
            return GZ_ENOMEM;
        if (copy_to_as(as, ph[i].vaddr, (const uint8_t *)image + ph[i].offset, ph[i].filesz))
            return GZ_EINVAL;
    }
    *entry = eh->entry;
    return 0;
}
