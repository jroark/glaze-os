/*
 * Raw NAND access for the file server (servers/fs), which runs Dhara and
 * littlefs on the flash above the kernel. Only a process the
 * kernel started with PROC_FLASH may use it, and only inside that region.
 * Pages and blocks are numbered from the start of the region.
 *
 * In a RAM boot (glaze/bootinfo.h) the region is a RAM disk instead, with
 * NAND semantics (a program clears bits, an erase sets them), and the whole
 * NAND, still holding Windows CE, is open through SYS_NAND_*.
 */

#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"
#include "../proc.h"
#include "nand.h"
#include "nand_ecc.h"
#include <glaze/bootinfo.h>

#define FIRST_BLOCK     (FLASH_FS_OFFSET / (NAND_PAGE_DATA * NAND_PAGES_PER_BLOCK))
#define FIRST_PAGE      (FIRST_BLOCK * NAND_PAGES_PER_BLOCK)
#define BLOCK_BYTES     (NAND_PAGE_DATA * NAND_PAGES_PER_BLOCK)

_Static_assert(FLASH_FS_OFFSET + GZ_FLASH_BLOCKS * NAND_PAGES_PER_BLOCK * NAND_PAGE_DATA == 16u << 20,
               "the file server's region runs to the end of the 16 MiB NAND");
_Static_assert(GZ_NAND_BLOCKS * BLOCK_BYTES == 16u << 20, "a 16 MiB NAND");

static uint8_t page_buf[NAND_PAGE_DATA] __attribute__((aligned(4)));
static bool ram_erased;

static uint32_t region_blocks(void)
{
    return ram_boot ? ram_boot->ramfs_blocks : GZ_FLASH_BLOCKS;
}

static uint8_t *ram_page(uint32_t page)
{
    /* The loader leaves the RAM disk as it found it; it starts erased. */
    if (!ram_erased) {
        memset((void *)KSEG0(ram_boot->ramfs_pa), 0xFF, ram_boot->ramfs_blocks * BLOCK_BYTES);
        ram_erased = true;
    }
    return (uint8_t *)KSEG0(ram_boot->ramfs_pa) + page * NAND_PAGE_DATA;
}

static int check(struct proc *p, uint32_t n, uint32_t limit)
{
    if (!(p->flags & PROC_FLASH))
        return GZ_EPERM;
    return n < limit ? 0 : GZ_EINVAL;
}

int sys_flash_info(struct proc *p, uint32_t uout)
{
    struct gz_flash_info info = { .blocks = region_blocks() };

    if (!(p->flags & PROC_FLASH))
        return GZ_EPERM;
    if (ram_boot) {
        info.flags = GZ_FLASH_RAM | GZ_FLASH_NAND;
        if (ram_boot->flags & GZ_BOOT_NAND_RW)
            info.flags |= GZ_FLASH_NAND_RW;
        info.nand_blocks = GZ_NAND_BLOCKS;
    }
    return copy_to_as(&p->as, uout, &info, sizeof(info));
}

int sys_flash_read(struct proc *p, uint32_t page, uint32_t ubuf)
{
    int rv = check(p, page, region_blocks() * GZ_FLASH_PAGES_PER_BLOCK);

    if (rv)
        return rv;
    if (ram_boot)
        return copy_to_as(&p->as, ubuf, ram_page(page), NAND_PAGE_DATA);
    nand_read_page(FIRST_PAGE + page, page_buf);
    return copy_to_as(&p->as, ubuf, page_buf, NAND_PAGE_DATA);
}

int sys_flash_prog(struct proc *p, uint32_t page, uint32_t ubuf)
{
    int rv = check(p, page, region_blocks() * GZ_FLASH_PAGES_PER_BLOCK);

    if (rv)
        return rv;
    if (copy_from_as(&p->as, page_buf, ubuf, NAND_PAGE_DATA))
        return GZ_EINVAL;
    if (ram_boot) {
        uint32_t *dst = (uint32_t *)ram_page(page);
        const uint32_t *src = (const uint32_t *)page_buf;
        for (uint32_t i = 0; i < NAND_PAGE_DATA / 4; i++)
            dst[i] &= src[i];
        return 0;
    }
    nand_program_page(FIRST_PAGE + page, page_buf);
    return 0;
}

int sys_flash_erase(struct proc *p, uint32_t block)
{
    int rv = check(p, block, region_blocks());

    if (rv)
        return rv;
    if (ram_boot) {
        memset(ram_page(block * NAND_PAGES_PER_BLOCK), 0xFF, BLOCK_BYTES);
        return 0;
    }
    nand_erase_block(FIRST_BLOCK + block);
    return 0;
}

/* ---- the whole NAND, in a RAM boot ------------------------------------------ */

static int check_nand(struct proc *p, uint32_t block, bool write)
{
    if (!(p->flags & PROC_FLASH) || !ram_boot)
        return GZ_EPERM;
    /* GlazeBoot's nand=off. */
    if (ram_boot->flags & GZ_BOOT_NAND_OFF)
        return GZ_ENOENT;
    if (block >= GZ_NAND_BLOCKS)
        return GZ_EINVAL;
    if (write && (!(ram_boot->flags & GZ_BOOT_NAND_RW) || block < GZ_NAND_FIRST_WRITABLE))
        return GZ_EPERM;
    return 0;
}

/*
 * Windows CE's block table, built as nanddisk.dll's NandColdBoot builds it
 * (0x019A3B24-0x019A3DCC): each physical block's first five pages carry, in
 * spare bytes 0-7, the tag AA 55 0F xx and its logical block number, or
 * AA 55 FF xx when erased. A block where three or more pages agree on a
 * number serves that logical block; three or more erased tags put it at the
 * next free number from 1004; anything else is a bad block. SYS_NAND_*
 * then address logical blocks, so that blocks Windows CE has replaced read
 * from where it moved them.
 */
#define BAT_TAG_PAGES   5u
#define BAT_FREE_FIRST  1004u
#define BAT_NONE        0xFFFFu

static uint16_t bat[GZ_NAND_BLOCKS];
static uint8_t raw[528] __attribute__((aligned(4)));

static void build_bat(void)
{
    uint32_t free_next = BAT_FREE_FIRST, moved = 0, bad = 0;
    uint64_t t0 = time_us();

    for (uint32_t i = 0; i < GZ_NAND_BLOCKS; i++)
        bat[i] = BAT_NONE;
    for (uint32_t phys = 0; phys < GZ_NAND_BLOCKS; phys++) {
        uint32_t ids[BAT_TAG_PAGES], n_ids = 0, n_erased = 0, id = BAT_NONE;

        for (uint32_t i = 0; i < BAT_TAG_PAGES; i++) {
            nand_read_raw(phys * NAND_PAGES_PER_BLOCK + i, raw);
            const uint8_t *t = raw + NAND_PAGE_DATA;
            if (t[0] != 0xAA || t[1] != 0x55)
                continue;
            uint32_t v = t[4] | t[5] << 8 | (uint32_t)t[6] << 16 | (uint32_t)t[7] << 24;
            if (t[2] == 0x0F && v != 0xFFFFFFFFu)
                ids[n_ids++] = v;
            else if (t[2] == 0xFF)
                n_erased++;
        }
        if (n_ids >= 3) {
            for (uint32_t a = 0; a < n_ids && id == BAT_NONE; a++) {
                uint32_t votes = 0;
                for (uint32_t b = 0; b < n_ids; b++)
                    votes += ids[b] == ids[a];
                if (votes >= 3 && ids[a] < GZ_NAND_BLOCKS)
                    id = ids[a];
            }
            if (id != BAT_NONE) {
                bat[id] = (uint16_t)phys;
                moved += id != phys;
            }
        } else if (n_erased >= 3) {
            if (free_next < GZ_NAND_BLOCKS)
                bat[free_next++] = (uint16_t)phys;
        } else {
            bad++;
        }
    }
    kprintf("nand: block table built in %u ms: %u blocks moved, %u bad\n",
            (unsigned)((time_us() - t0) / 1000), (unsigned)moved, (unsigned)bad);
}

/* The physical block serving logical block b, or BAT_NONE. */
static uint32_t nand_block(uint32_t b)
{
    static bool ready;

    if (!ready) {
        nand_reset();
        build_bat();
        ready = true;
    }
    return bat[b];
}

int sys_nand_read(struct proc *p, uint32_t page, uint32_t ubuf)
{
    int rv = check_nand(p, page / NAND_PAGES_PER_BLOCK, false);

    if (rv)
        return rv;
    uint32_t phys = nand_block(page / NAND_PAGES_PER_BLOCK);
    if (phys == BAT_NONE) {
        memset(raw, 0xFF, NAND_PAGE_DATA);      /* no block: reads erased */
        return copy_to_as(&p->as, ubuf, raw, NAND_PAGE_DATA);
    }
    page = phys * NAND_PAGES_PER_BLOCK + page % NAND_PAGES_PER_BLOCK;
    /* Windows CE's data: checked and corrected with the spare area's ECC as
     * nanddisk.dll does (nand_ecc.c). A page whose parity (spare bytes
     * 8-15) is erased has none: an erased page, or the emulator's, whose
     * restore images have no spare area. */
    nand_read_raw(page, raw);
    bool no_ecc = true;
    for (unsigned i = NAND_ECC_BYTES; i < sizeof(raw); i++)
        no_ecc = no_ecc && raw[i] == 0xFF;
    int ecc = no_ecc ? 0 : nand_ecc_check(raw);
    if (ecc < 0) {
        kprintf("nand: page %x has errors ECC can't correct\n", page);
        return GZ_EIO;
    }
    if (ecc > 0)
        kprintf("nand: corrected %d byte%s in page %x\n", ecc, ecc == 1 ? "" : "s", page);
    return copy_to_as(&p->as, ubuf, raw, NAND_PAGE_DATA);
}

int sys_nand_prog(struct proc *p, uint32_t page, uint32_t ubuf)
{
    int rv = check_nand(p, page / NAND_PAGES_PER_BLOCK, true);

    if (rv)
        return rv;
    if (copy_from_as(&p->as, page_buf, ubuf, NAND_PAGE_DATA))
        return GZ_EINVAL;
    /* Writes don't yet keep Windows CE's tags and ECC (nand=rw is opt-in). */
    uint32_t phys = nand_block(page / NAND_PAGES_PER_BLOCK);
    if (phys == BAT_NONE || phys < GZ_NAND_FIRST_WRITABLE)
        return GZ_EPERM;
    nand_program_page(phys * NAND_PAGES_PER_BLOCK + page % NAND_PAGES_PER_BLOCK, page_buf);
    return 0;
}

int sys_nand_erase(struct proc *p, uint32_t block)
{
    int rv = check_nand(p, block, true);

    if (rv)
        return rv;
    uint32_t phys = nand_block(block);
    if (phys == BAT_NONE || phys < GZ_NAND_FIRST_WRITABLE)
        return GZ_EPERM;
    nand_erase_block(phys);
    return 0;
}
