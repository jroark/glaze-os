/* Private memory for userspace executable loaders. The CPU has no NX bit:
 * publication synchronizes its split caches and optionally removes writes. */
#ifndef GLAZE_VM_H
#define GLAZE_VM_H

#include <glaze/sys.h>

/* Page-aligned, nonempty regions below 0x0f000000, at most 4 MiB each.
 * Existing mappings (including demand-paged images) are never replaced. */
static inline int gz_vm_map(uint32_t address, uint32_t size)
{
    return GZ_SC2(SYS_VM_MAP, address, size);
}
/* Operates on page-aligned subranges of one allocation; writable is 0 or 1. */
static inline int gz_vm_publish(uint32_t address, uint32_t size, uint32_t writable)
{
    return GZ_SC3(SYS_VM_PUBLISH, address, size, writable);
}
static inline int gz_vm_release(uint32_t address)
{
    return GZ_SC1(SYS_VM_RELEASE, address);
}

#endif
