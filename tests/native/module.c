/* Load the real cross-built ET_REL drivers without executing MIPS code, then
 * corrupt their headers/symbols/relocations and check bounded rollback. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include "../../kernel/module.h"
static uint8_t *input;
static struct romfs_entry rom;
static unsigned pages;
static bool missing_import;
static uintptr_t module_resolve(const char *name)
{ (void)name; return missing_import ? 0 : 0x40010000u; }
#define MODULE_TEST
#include "../../kernel/module.c"
void kprintf(const char *format, ...) { (void)format; }
const struct romfs_entry *romfs_lookup(const char *name) { return &rom; }
int romfs_read(const struct romfs_entry *e, uint32_t off, void *p, uint32_t n)
{ assert(off <= e->size && n <= e->size - off); memcpy(p, input + off, n); return 0; }
/* Relocated addresses must fit in 32 bits: MAP_32BIT on x86-64, else a
 * low hint (Linux honours a free one). */
#ifdef MAP_32BIT
#define LOW_FLAGS MAP_32BIT
#else
#define LOW_FLAGS 0
#endif
static uintptr_t low_hint = 0x40000000u;
void *page_alloc_n(uint32_t n)
{
    void *hint = LOW_FLAGS ? NULL : (void *)low_hint;
    low_hint += (n + 1) * 4096u;
    void *p = mmap(hint, n * 4096u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | LOW_FLAGS, -1, 0);
    assert(p != MAP_FAILED && (uintptr_t)p >= 0x40000000u && (uintptr_t)p < 0x50000000u);
    pages += n;
    return p;
}
void page_free(void *p) { assert(!munmap(p, 4096)); assert(pages); pages--; }
void mm_sync_code(void *p, uint32_t n) {}
/* kernel/hwfile.c: romfs, or (from_package) a verified file already in memory. */
static bool from_package;
static int hwfile_opens;
int hwfile_open(const char *name, struct hwfile *f)
{
    memset(f, 0, sizeof(*f));
    f->slot = -1;
    f->size = rom.size;
    if (from_package) f->buf = input; else f->rom = romfs_lookup(name);
    hwfile_opens++;
    return 0;
}
int hwfile_read(const struct hwfile *f, uint32_t off, void *p, uint32_t n)
{ assert(f->rom); return romfs_read(f->rom, off, p, n); }
void hwfile_close(struct hwfile *f) { assert(hwfile_opens > 0); hwfile_opens--; }
static void fails(void)
{
    struct module m = {0};
    assert(module_load("test", "glaze_driver", &m) < 0);
    assert(!m.base && !m.pages && !pages);
}
int main(int argc, char **argv)
{
    assert(argc >= 2);
    for (int arg = 1; arg < argc; arg++) {
        FILE *f = fopen(argv[arg], "rb"); assert(f);
        fseek(f, 0, SEEK_END); rom.size = (uint32_t)ftell(f); rewind(f);
        input = malloc(rom.size); uint8_t *saved = malloc(rom.size); assert(input && saved);
        assert(fread(input, 1, rom.size, f) == rom.size); fclose(f);
        memcpy(saved, input, rom.size);
        struct module m = {0};
        assert(!module_load("test", "glaze_driver", &m));
        assert(module_contains(&m, m.entry, 16));
        assert(!module_executable(&m, m.entry));
        uint32_t callback; memcpy(&callback, (uint8_t *)m.entry + 16, 4);
        assert(module_executable(&m, (const void *)(uintptr_t)callback));
        assert(!module_executable(&m, (const void *)(uintptr_t)(callback + 1)));
        assert(module_load("test", "glaze_driver", &m) == GZ_EBUSY);
        module_unload(&m); assert(!pages);
        /* A package's copy loads in place: only the image's pages. */
        from_package = true;
        assert(!module_load("test", "glaze_driver", &m));
        assert(pages == m.pages && module_contains(&m, m.entry, 16));
        module_unload(&m); assert(!pages);
        {
            struct gz_elf_ehdr *pe = (void *)input;
            uint32_t was = pe->shoff; pe->shoff = 0xfffffffcu; fails(); pe->shoff = was;
        }
        from_package = false;
        assert(!hwfile_opens);
        struct gz_elf_ehdr *e = (void *)input;
        uint32_t old = e->shoff; e->shoff = 0xfffffffcu; fails(); e->shoff = old;
        e->type = 2; fails(); e->type = 1;
        e->flags |= 4; fails(); e->flags &= ~4u;
        missing_import = true; fails(); missing_import = false;
        struct section *sh = (void *)(input + e->shoff);
        for (unsigned i = 0; i < e->shnum; i++) {
            if ((sh[i].flags & SH_ALLOC) && sh[i].size) {
                old = sh[i].size; sh[i].size = IMAGE_MAX + 1; fails(); sh[i].size = old;
            }
            if (sh[i].type != 9 || !sh[i].size || !(sh[sh[i].info].flags & SH_ALLOC)) continue;
            struct gz_elf_rel *r = (void *)(input + sh[i].offset);
            old = r[0].offset; r[0].offset = 0xfffffffc; fails(); r[0].offset = old;
            old = r[0].info; r[0].info = (old & ~255u) | 255u; fails(); r[0].info = old;
            r[0].info = 0xffffff02u; fails(); r[0].info = old;
        }
        for (uint32_t size = 0; size < 52; size++) { uint32_t n = rom.size; rom.size = size; fails(); rom.size = n; }
        assert(!memcmp(input, saved, rom.size));
        free(input); free(saved);
    }
    /* ABI signed LO16 carry and the shared-LO HI16 case. */
    uint32_t hi = 0x3c080001, lo = 0x25088000;
    assert(relocate(&hi, 5, 0x40008000, lo));
    assert(relocate(&lo, 6, 0x40008000, 0));
    assert((hi & 65535) == 0x4001 && (lo & 65535) == 0);
    puts("modules: real drivers, relocations, malformed ELF and allocation rollback PASS");
    return 0;
}
