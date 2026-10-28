/* Stage a fresh RAM boot while the current OS is alive. Only the final
 * handoff stops hardware. The relocation code/control never overlap a
 * destination; source pages may overlap and are handled by gb_permute. */
#include "kernel.h"
#ifndef USB_RELOAD_TEST
#include "hw.h"
#endif
#include "drivers.h"
#include <glaze/bootinfo.h>
#include <glaze/usb.h>
#include <glaze/usb_boot.h>
#include "../wince/glazeboot/plan.h"
typedef uint32_t DWORD;
#include "tramp_blob.h"

#define STAGE_PAGES (GU_RAM_BYTES / PAGE_SIZE)
static void *stage[STAGE_PAGES];
static uint32_t pages, received, total;
static struct gz_usb_boot manifest;
static struct gu_layout layout;
static struct gb_control *control;
static void *code, *info;
static bool prepared;

static void abort_reload(void)
{
    if (control) {
        for (unsigned i = 0; i < GB_MAX_MOVE_PAGES; i++)
            if (control->move_frames[i]) page_free((void *)KSEG0(control->move_frames[i] << 12));
        for (unsigned i = 0; i < GB_WHO_PAGES; i++)
            if (control->who_frames[i]) page_free((void *)KSEG0(control->who_frames[i] << 12));
        page_free(control); control = NULL;
    }
    if (code) { page_free(code); code = NULL; }
    if (info) { page_free(info); info = NULL; }
    while (pages) { page_free(stage[--pages]); stage[pages] = NULL; }
    total = received = 0; prepared = false;
}
static void stage_read(uint32_t off, void *vp, uint32_t n)
{
    uint8_t *p = vp;
    while (n) {
        uint32_t k = MIN(n, PAGE_SIZE - off % PAGE_SIZE);
        memcpy(p, (uint8_t *)stage[off / PAGE_SIZE] + off % PAGE_SIZE, k);
        p += k; off += k; n -= k;
    }
}
static uint32_t stage_crc(uint32_t off, uint32_t n)
{
    uint32_t crc = ~0u;
    while (n) {
        uint32_t k = MIN(n, PAGE_SIZE - off % PAGE_SIZE);
        crc = gu_crc(crc, (uint8_t *)stage[off / PAGE_SIZE] + off % PAGE_SIZE, k);
        off += k; n -= k;
    }
    return ~crc;
}
static void *meta_page(void)
{
    return page_alloc_between(layout.end, layout.ramfs);
}
static int prepare(void)
{
    uint8_t h[32];
    if (!total || received != total || prepared) return GZ_EINVAL;
    stage_read(0, h, sizeof(h));
    if (gu_boot_layout(h, manifest.kernel_bytes, manifest.romfs_bytes, manifest.ramfs_kb, &layout) ||
        stage_crc(0, manifest.kernel_bytes) != manifest.kernel_crc ||
        stage_crc(ALIGN_UP(manifest.kernel_bytes, PAGE_SIZE), manifest.romfs_bytes) != manifest.romfs_crc) return GZ_EINVAL;
    uint32_t romfs_off = ALIGN_UP(manifest.kernel_bytes, PAGE_SIZE);
    stage_read(romfs_off, h, 8);
    uint32_t count = gu_get32(h + 4);
    if (memcmp(h, "GZRF", 4) || !count || count > 256 || 8 + count * 56 > manifest.romfs_bytes) return GZ_EINVAL;
    bool fs = false, usb = false, init = false;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t e[56]; stage_read(romfs_off + 8 + i * 56, e, sizeof(e));
        uint32_t off = gu_get32(e + 48), size = gu_get32(e + 52);
        if (!memchr(e, 0, 48) || off % PAGE_SIZE || off < 8 + count * 56 ||
            off > manifest.romfs_bytes || size > manifest.romfs_bytes - off) return GZ_EINVAL;
        fs |= !strcmp((char *)e, "bin/fs"); usb |= !strcmp((char *)e, "bin/usb");
        init |= !strcmp((char *)e, "bin/springboard") || !strcmp((char *)e, "bin/ktest");
    }
    if (!fs || !usb || !init) return GZ_EINVAL;
    uint32_t kp = (layout.size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t rp = (manifest.romfs_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t moves = 1 + kp + rp;
    if (moves > GB_MAX_MOVES || TRAMP_BYTES > PAGE_SIZE) return GZ_EINVAL;
    /* Boot image components share staging pages if kernel size is not page
     * aligned. Assemble destination pages into their own frames first.
     * Reuse the original staging array for source copies only after every
     * destination source has been assembled; no OS state is destroyed. */
    control = meta_page(); code = meta_page(); info = meta_page();
    if (!control || !code || !info) return GZ_ENOMEM;
    control->magic = GB_CONTROL_MAGIC; control->count = moves;
    control->entry = layout.entry; control->arg = KSEG0(GZ_BOOTINFO_PA);
    for (uint32_t i = 0; i < (moves + GB_MOVES_PER_PAGE - 1) / GB_MOVES_PER_PAGE; i++) {
        void *p = meta_page(); if (!p) return GZ_ENOMEM;
        control->move_frames[i] = PHYS(p) >> 12;
    }
    for (unsigned i = 0; i < GB_WHO_PAGES; i++) {
        void *p = meta_page(); if (!p) return GZ_ENOMEM;
        control->who_frames[i] = PHYS(p) >> 12;
    }
    struct gz_bootinfo *bi = info;
    bi->magic = GZ_BOOTINFO_MAGIC; bi->version = GZ_BOOTINFO_VERSION;
    bi->flags = manifest.flags; bi->romfs_pa = layout.romfs; bi->romfs_size = manifest.romfs_bytes;
    bi->ramfs_pa = layout.ramfs; bi->ramfs_blocks = layout.ramfs_len / 16384;
    struct gb_move *m = gb_move_at(control, 0);
    m->src = PHYS(info) >> 12; m->dst = GZ_BOOTINFO_PA >> 12;
    /* Kernel contents start at file offset 4096: naturally page aligned. */
    for (uint32_t i = 0; i < kp; i++) {
        m = gb_move_at(control, 1 + i);
        m->src = PHYS(stage[1 + i]) >> 12; m->dst = (layout.load >> 12) + i;
    }
    /* Align romfs in the staging stream by requiring page padding between
     * components at upload time; begin accounts for this padding. */
    uint32_t first_romfs = ALIGN_UP(manifest.kernel_bytes, PAGE_SIZE) / PAGE_SIZE;
    for (uint32_t i = 0; i < rp; i++) {
        m = gb_move_at(control, 1 + kp + i);
        m->src = PHYS(stage[first_romfs + i]) >> 12; m->dst = (layout.romfs >> 12) + i;
    }
    memcpy(code, tramp_code, TRAMP_BYTES);
    mm_sync_code(code, PAGE_SIZE);
    prepared = true; return 0;
}
int usb_reload_ctl(struct proc *p, uint32_t op, uint32_t buf, uint32_t len)
{
    if (op == GZ_USB_RELOAD_ABORT) { abort_reload(); return 0; }
    if (op == GZ_USB_RELOAD_BEGIN) {
        if (len != sizeof(manifest) || total) return GZ_EINVAL;
        if (copy_from_as(&p->as, &manifest, buf, len)) return GZ_EINVAL;
        if (manifest.kernel_bytes < 4096 || manifest.kernel_bytes > 1024 * 1024 ||
            manifest.romfs_bytes < 8 || manifest.romfs_bytes > 0x4c0000 ||
            manifest.ramfs_kb < 128 || manifest.ramfs_kb > 11264 ||
            manifest.flags & ~GZ_BOOT_NAND_OFF) return GZ_EINVAL;
        total = ALIGN_UP(manifest.kernel_bytes, PAGE_SIZE) + manifest.romfs_bytes;
        uint32_t want = ALIGN_UP(total, PAGE_SIZE) / PAGE_SIZE;
        if (want + 32 > mm_free_pages()) { total = 0; return GZ_ENOMEM; }
        while (pages < want) {
            void *s = page_alloc();
            if (!s) { abort_reload(); return GZ_ENOMEM; }
            stage[pages++] = s;
        }
        return 0;
    }
    if (op == GZ_USB_RELOAD_DATA) {
        uint8_t b[GU_PAYLOAD];
        if (!total || prepared || len < 5 || len > sizeof(b) || copy_from_as(&p->as, b, buf, len)) return GZ_EINVAL;
        uint32_t off = gu_get32(b), n = len - 4;
        if (off != received || n > total - received) return GZ_EINVAL;
        while (n) {
            uint32_t k = MIN(n, PAGE_SIZE - received % PAGE_SIZE);
            memcpy((uint8_t *)stage[received / PAGE_SIZE] + received % PAGE_SIZE, b + len - n, k);
            received += k; n -= k;
        }
        return 0;
    }
    if (op == GZ_USB_RELOAD_PREPARE) {
        int r = prepare();
        if (r) abort_reload();
        return r;
    }
    if (op == GZ_USB_RELOAD_GO && prepared) {
        uint64_t end = time_us() + 2000000u;
        while (!usb_tx_idle()) if (time_us() >= end) return GZ_ETIMEDOUT;
        usb_stop();
        /* The trampoline masks CPU interrupts, stops audio and flushes
         * caches before relocation. No further scheduler entry occurs. */
        ((void (*)(struct gb_control *))((uintptr_t)code + TRAMP_ENTRY))(control);
        for (;;) ;
    }
    return GZ_EINVAL;
}
