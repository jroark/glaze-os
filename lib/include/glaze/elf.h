/* Glaze's supported ELF32/MIPS O32 subset. No host libc dependencies. */
#ifndef GLAZE_ELF_H
#define GLAZE_ELF_H
#include <glaze/types.h>
struct gz_elf_ehdr {
    uint8_t ident[16]; uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct gz_elf_phdr { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };
struct gz_elf_dyn { uint32_t tag, val; };
struct gz_elf_sym { uint32_t name, value, size; uint8_t info, other; uint16_t shndx; };
struct gz_elf_rel { uint32_t offset, info; };
#define GZ_ET_EXEC 2
#define GZ_ET_DYN 3
#define GZ_PT_LOAD 1
#define GZ_PT_DYNAMIC 2
#define GZ_PT_INTERP 3
#define GZ_PT_TLS 7
#define GZ_PT_GNU_RELRO 0x6474e552u
#define GZ_PF_X 1
#define GZ_PF_W 2
#define GZ_PF_R 4
#define GZ_ELF_PHMAX 32
#define GZ_ELF_MAX_SIZE (2u * 1024u * 1024u)
/* Validate arithmetic before forming any file pointer. Shared with host tests. */
static inline bool gz_elf_range(uint32_t off, uint32_t len, uint32_t size)
{ return off <= size && len <= size - off; }
static inline bool gz_elf_header(const struct gz_elf_ehdr *e, uint32_t size)
{
    return size >= sizeof(*e) && e->ident[0] == 0x7f && e->ident[1] == 'E' &&
        e->ident[2] == 'L' && e->ident[3] == 'F' && e->ident[4] == 1 &&
        e->ident[5] == 1 && e->ident[6] == 1 && e->version == 1 && e->machine == 8 &&
        (e->type == GZ_ET_EXEC || e->type == GZ_ET_DYN) && e->ehsize == sizeof(*e) &&
        !(e->phoff & 3u) && e->phentsize == sizeof(struct gz_elf_phdr) && e->phnum && e->phnum <= GZ_ELF_PHMAX &&
        gz_elf_range(e->phoff, e->phnum * e->phentsize, size) &&
        (e->flags & 0xf0000000u) <= 0x20000000u && /* MIPS I/II/III */
        !(e->flags & (0x06000000u | 0x00000020u | 0x00000040u)) && /* microMIPS, ABI2, ABI_ON32 */
        (!(e->flags & 0xf000u) || (e->flags & 0xf000u) == 0x1000u);
}
static inline bool gz_elf_segment(const struct gz_elf_phdr *p, uint32_t size)
{
    if (p->type == GZ_PT_TLS) return false;
    if (p->type != GZ_PT_LOAD) return true;
    return p->filesz <= p->memsz && gz_elf_range(p->offset, p->filesz, size) &&
        p->memsz <= 0x04000000u && p->vaddr <= 0x80000000u - p->memsz &&
        (p->vaddr & 4095u) == (p->offset & 4095u) &&
        (!p->align || (!(p->align & (p->align - 1)) && p->align <= 65536u &&
            (p->vaddr & (p->align - 1)) == (p->offset & (p->align - 1))));
}
#endif
