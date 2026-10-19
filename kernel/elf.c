/* ELF process loading. Static programs retain their original entry ABI. */
#include "kernel.h"
#include "proc.h"
#include "hw.h"

int elf_load(struct addrspace *as, const void *image, uint32_t size, uint32_t *entry, struct gz_ld_start *start)
{
    struct gz_image_info main;
    memset(start, 0, sizeof(*start));
    int rv = image_map(as, image, size, &main, USER_TEXT_BASE);
    if (rv) return rv;
    const struct gz_elf_ehdr *eh = image;
    const struct gz_elf_phdr *ph = (const void *)((const uint8_t *)image + eh->phoff);
    const char *interp = NULL;
    bool executable_entry = false;
    for (unsigned i = 0; i < main.phnum; i++) {
        const struct gz_elf_phdr *p = &main.ph[i];
        if (p->type == GZ_PT_LOAD && (p->flags & GZ_PF_X) && main.entry >= main.bias + p->vaddr &&
            main.entry - main.bias - p->vaddr < p->memsz) executable_entry = true;
    }
    if (!executable_entry) return GZ_EINVAL;
    for (unsigned i = 0; i < eh->phnum; i++) {
        if (ph[i].type != GZ_PT_INTERP) continue;
        if (interp || !gz_elf_range(ph[i].offset, ph[i].filesz, size) || ph[i].filesz != sizeof("/lib/ld-glaze.so.1")) return GZ_EINVAL;
        interp = (const char *)image + ph[i].offset;
        if (memcmp(interp, "/lib/ld-glaze.so.1", sizeof("/lib/ld-glaze.so.1"))) return GZ_EINVAL;
    }
    *entry = main.entry;
    if (!interp) return eh->type == GZ_ET_EXEC && !main.dynamic ? 0 : GZ_EINVAL;
    if (eh->type != GZ_ET_DYN || !main.dynamic) return GZ_EINVAL;
    uint32_t isize;
    const void *idata = romfs_find(interp, &isize);
    if (!idata) return GZ_ENOENT;
    struct gz_image_info loader;
    rv = image_map(as, idata, isize, &loader, 0);
    if (rv || loader.base != GZ_LD_BASE || loader.dynamic) return GZ_EINVAL;
    start->abi = GZ_LD_ABI; start->main = main; *entry = loader.entry;
    return 0;
}
