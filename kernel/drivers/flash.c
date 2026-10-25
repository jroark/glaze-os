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
    /* GlazeBoot's nand=off: on a real BE-300 the first read after Windows CE
     * stalls the bus in the stream window, freezing the machine, until the
     * NAND engine's state after nanddisk.dll is understood. */
    if (ram_boot->flags & GZ_BOOT_NAND_OFF)
        return GZ_ENOENT;
    if (block >= GZ_NAND_BLOCKS)
        return GZ_EINVAL;
    if (write && (!(ram_boot->flags & GZ_BOOT_NAND_RW) || block < GZ_NAND_FIRST_WRITABLE))
        return GZ_EPERM;
    return 0;
}

int sys_nand_read(struct proc *p, uint32_t page, uint32_t ubuf)
{
    int rv = check_nand(p, page / NAND_PAGES_PER_BLOCK, false);

    if (rv)
        return rv;
    nand_read_page(page, page_buf);
    return copy_to_as(&p->as, ubuf, page_buf, NAND_PAGE_DATA);
}

int sys_nand_prog(struct proc *p, uint32_t page, uint32_t ubuf)
{
    int rv = check_nand(p, page / NAND_PAGES_PER_BLOCK, true);

    if (rv)
        return rv;
    if (copy_from_as(&p->as, page_buf, ubuf, NAND_PAGE_DATA))
        return GZ_EINVAL;
    nand_program_page(page, page_buf);
    return 0;
}

int sys_nand_erase(struct proc *p, uint32_t block)
{
    int rv = check_nand(p, block, true);

    if (rv)
        return rv;
    nand_erase_block(block);
    return 0;
}
