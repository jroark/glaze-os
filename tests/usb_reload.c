/* Exercise the real staging/validation/planner with a 16 MiB RAM allocator. */
#include "../kernel/kernel.h"
#include "../kernel/drivers.h"
#include <glaze/usb.h>
#include <glaze/bootinfo.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t *memory;
static uint8_t used[4096], input[4096];
static unsigned allocated, allocation_limit = 4096;
#define SDRAM_SIZE (16u * 1024u * 1024u)
#define KSEG0(pa) ((uintptr_t)(memory + (pa)))
#define PHYS(p) ((uintptr_t)(p) - (uintptr_t)memory)
#define GB_PAGE_PTR(frame) ((uint32_t *)(memory + (frame) * 4096u))
#define USB_RELOAD_TEST
#include "../kernel/usb_reload.c"
void *page_alloc_between(uint32_t low, uint32_t high)
{
    if (allocated == allocation_limit) return NULL;
    for (unsigned i = (low + 4095) / 4096; i < high / 4096; i++) {
        if (used[i]) continue;
        used[i] = 1; allocated++; memset(memory + i * 4096, 0, 4096); return memory + i * 4096;
    }
    return NULL;
}
void *page_alloc(void) { return page_alloc_between(0x40000, SDRAM_SIZE); }
void page_free(void *p)
{ unsigned i = (unsigned)(PHYS(p) / 4096); assert(i < 4096 && used[i]); used[i] = 0; allocated--; }
uint32_t mm_free_pages(void) { return allocation_limit - allocated; }
uint32_t romfs_reclaim(uint32_t n) { (void)n; return 0; }
void mm_sync_code(void *p, uint32_t n) { (void)p; (void)n; }
int copy_from_as(struct addrspace *as, void *p, uint32_t va, uint32_t n)
{ (void)as; if (va > sizeof(input) || n > sizeof(input) - va) return -1; memcpy(p, input + va, n); return 0; }
bool usb_tx_idle(void) { return true; }
void usb_stop(void) {}
uint64_t time_us(void) { return 0; }
static void begin(struct proc *p, const uint8_t *kernel, unsigned ks, const uint8_t *romfs, unsigned rs)
{
    struct gz_usb_boot b = {ks,rs,2048,GZ_BOOT_NAND_OFF,~gu_crc(~0u,kernel,ks),~gu_crc(~0u,romfs,rs)};
    memcpy(input, &b, sizeof(b));
    assert(!usb_reload_ctl(p, GZ_USB_RELOAD_BEGIN, 0, sizeof(b)));
    assert(!allocated && !pages); /* BEGIN must not allocate the entire image */
}
static void send_data(struct proc *p, const uint8_t *image, unsigned n)
{
    unsigned off = 0;
    while (off < n) {
        unsigned k = MIN(n - off, sizeof(input) - 4);
        gu_put32(input, off); memcpy(input + 4, image + off, k);
        assert(!usb_reload_ctl(p, GZ_USB_RELOAD_DATA, 0, k + 4)); off += k;
    }
}
int main(void)
{
    memory = calloc(1, SDRAM_SIZE); assert(memory);
    unsigned ks = 4096 + 5001, rs = 5 * 4096;
    uint8_t *kernel = calloc(1, ks), *romfs = calloc(1, rs); assert(kernel && romfs);
    gu_put32(kernel, GZ_KERNEL_BIN_MAGIC); gu_put32(kernel + 4, 0x80010000);
    gu_put32(kernel + 8, 0x80010000); gu_put32(kernel + 12, ks - 4096); gu_put32(kernel + 16, 0x80040000);
    memset(kernel + 4096, 0xa5, ks - 4096);
    memcpy(romfs, "GZRF", 4); gu_put32(romfs + 4, 3);
    const char *names[] = {"bin/fs","bin/usb","bin/springboard"};
    for (unsigned i = 0; i < 3; i++) {
        strcpy((char *)romfs + 8 + i * 56, names[i]);
        gu_put32(romfs + 8 + i * 56 + 48, (i + 1) * 4096);
        gu_put32(romfs + 8 + i * 56 + 52, 4096);
        memset(romfs + (i + 1) * 4096, (int)(0x10 + i), 4096);
    }
    unsigned padded = (ks + 4095) & ~4095u, size = padded + rs;
    uint8_t *image = calloc(1, size); assert(image);
    memcpy(image, kernel, ks); memcpy(image + padded, romfs, rs);
    struct proc p = {0};
    begin(&p, kernel, ks, romfs, rs);
    allocation_limit = 0;
    gu_put32(input, 0); input[4] = image[0];
    assert(usb_reload_ctl(&p, GZ_USB_RELOAD_DATA, 0, 5) == GZ_ENOMEM);
    assert(!allocated && !pages && !total);
    allocation_limit = 4096;
    begin(&p, kernel, ks, romfs, rs);
    assert(usb_reload_ctl(&p, GZ_USB_RELOAD_PREPARE, 0, 0) == GZ_EINVAL && !allocated);
    begin(&p, kernel, ks, romfs, rs);
    gu_put32(input, 99); assert(usb_reload_ctl(&p, GZ_USB_RELOAD_DATA, 0, 5) == GZ_EINVAL);
    send_data(&p, image, size); ((uint8_t *)stage[1])[0] ^= 1;
    assert(usb_reload_ctl(&p, GZ_USB_RELOAD_PREPARE, 0, 0) == GZ_EINVAL && !allocated);
    begin(&p, kernel, ks, romfs, rs); send_data(&p, image, size);
    allocation_limit = allocated;
    assert(usb_reload_ctl(&p, GZ_USB_RELOAD_PREPARE, 0, 0) == GZ_ENOMEM && !allocated);
    allocation_limit = 4096;
    begin(&p, kernel, ks, romfs, rs); send_data(&p, image, size);
    assert(!usb_reload_ctl(&p, GZ_USB_RELOAD_PREPARE, 0, 0) && prepared);
    for (unsigned i = 0; i < control->count; i++) {
        struct gb_move *m = gb_move_at(control, i);
        assert(m->src < 4096 && m->dst < 4096);
        assert(PHYS(control) / 4096 != m->dst && PHYS(code) / 4096 != m->dst);
    }
    gb_permute(control);
    assert(!memcmp(memory + layout.load, kernel + 4096, ks - 4096));
    assert(!memcmp(memory + layout.romfs, romfs, rs));
    struct gz_bootinfo *bi = (struct gz_bootinfo *)(memory + GZ_BOOTINFO_PA);
    assert(bi->magic == GZ_BOOTINFO_MAGIC && bi->flags == GZ_BOOT_NAND_OFF && bi->ramfs_pa == layout.ramfs);
    abort_reload(); assert(!allocated);
    free(image); free(kernel); free(romfs); free(memory);
    puts("usb-reload: ALL PASS"); return 0;
}
