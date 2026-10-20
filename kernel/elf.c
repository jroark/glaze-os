/* ELF process loading. Static programs retain their original entry ABI. */
#include "kernel.h"
#include "proc.h"
#include "hw.h"

#define LD_PATH "/lib/ld-glaze.so.1"

/* The program comes from romfs (rom) or from a buffer at src in src_as. */
int elf_load(struct addrspace *as, struct addrspace *src_as, const struct romfs_entry *rom, uint32_t src, uint32_t size,
             uint32_t *entry, struct gz_ld_start *start)
{
    struct gz_image_info main;
    char interp[sizeof(LD_PATH)];
    uint32_t type;
    memset(start, 0, sizeof(*start));
    int rv = image_map_exec(as, src_as, rom, src, size, &main, &type, interp, sizeof(interp));
    if (rv) return rv;
    bool executable_entry = false;
    for (unsigned i = 0; i < main.phnum; i++) {
        const struct gz_elf_phdr *p = &main.ph[i];
        if (p->type == GZ_PT_LOAD && (p->flags & GZ_PF_X) && main.entry >= main.bias + p->vaddr &&
            main.entry - main.bias - p->vaddr < p->memsz) executable_entry = true;
    }
    if (!executable_entry) return GZ_EINVAL;
    *entry = main.entry;
    if (!interp[0]) return type == GZ_ET_EXEC && !main.dynamic ? 0 : GZ_EINVAL;
    if (strcmp(interp, LD_PATH) || type != GZ_ET_DYN || !main.dynamic) return GZ_EINVAL;
    const struct romfs_entry *ld = romfs_lookup(LD_PATH);
    if (!ld) return GZ_ENOENT;
    struct gz_image_info loader;
    rv = image_map(as, ld, ld->size, &loader, 0);
    if (rv || loader.base != GZ_LD_BASE || loader.dynamic) return GZ_EINVAL;
    start->abi = GZ_LD_ABI; start->main = main; *entry = loader.entry;
    return 0;
}
