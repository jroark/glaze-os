/*
 * Read-only file archive linked into the kernel image (tools/mkromfs.py).
 *
 *   header:  "GZRF" u32 count
 *   entries: char name[48], u32 offset, u32 size   (offset from archive start)
 *   data:    each file 4 KiB aligned so it can be mapped into user space
 */

#include "kernel.h"
#include "proc.h"

extern const uint8_t __romfs_start[], __romfs_end[];

struct romfs_entry {
    char name[48];
    uint32_t offset;
    uint32_t size;
};

static const struct romfs_entry *entries(uint32_t *count)
{
    if (__romfs_end - __romfs_start < 8 || memcmp(__romfs_start, "GZRF", 4)) {
        *count = 0;
        return NULL;
    }
    *count = *(const uint32_t *)(__romfs_start + 4);
    return (const struct romfs_entry *)(__romfs_start + 8);
}

const void *romfs_find(const char *name, uint32_t *size)
{
    uint32_t n;
    const struct romfs_entry *e = entries(&n);

    if (*name == '/')
        name++;
    for (uint32_t i = 0; i < n; i++) {
        if (!strncmp(e[i].name, name, sizeof(e[i].name))) {
            *size = e[i].size;
            return __romfs_start + e[i].offset;
        }
    }
    return NULL;
}

void romfs_list(void)
{
    uint32_t n;

    entries(&n);
    kprintf("romfs: %u files, %u KiB\n", n, (uint32_t)(__romfs_end - __romfs_start) / 1024);
}
