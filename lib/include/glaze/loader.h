/* Kernel image mappings and private interpreter/libc startup ABI. */
#ifndef GLAZE_LOADER_H
#define GLAZE_LOADER_H
#include <glaze/elf.h>
#define GZ_LD_ABI 1u
#define GZ_LD_BASE 0x0f000000u
#define GZ_DSO_BASE 0x20000000u
#define GZ_DSO_END 0x40000000u
#define GZ_IMAGE_ROMFS 1u
#define GZ_IMAGE_BYTES 2u
struct gz_image_info {
    uint32_t base, end, bias, entry, dynamic, phnum;
    struct gz_elf_phdr ph[GZ_ELF_PHMAX];
};
struct gz_image_page { uint32_t frame, users, writable; };
struct gz_image_stats { uint32_t mappings, shared_pages, private_pages; };
/* The interpreter calls PIC functions through ld_call (t9 must name callee). */
struct gz_ld_hooks {
    int (*read_file)(const char *, void **, uint32_t *);
    void (*free_file)(void *);
};
struct gz_ld_api {
    void *(*open)(const char *, int);
    void *(*symbol)(void *, const char *);
    int (*close)(void *);
    char *(*error)(void);
    void (*finish)(void);
};
struct gz_ld_start {
    uint32_t abi, pid;
    struct gz_image_info main;
};
/* Called once, after libc's own relocations and before any constructors. */
void __glaze_ld_init(const struct gz_ld_api *, const struct gz_ld_hooks **);
#endif
