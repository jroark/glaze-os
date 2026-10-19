/*
 * Raw NAND access for the file server (servers/fs), which runs Dhara and
 * littlefs on the flash above the kernel. Only a process the
 * kernel started with PROC_FLASH may use it, and only inside that region.
 * Pages and blocks are numbered from the start of the region.
 */

#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"
#include "nand.h"

#define FIRST_BLOCK     (FLASH_FS_OFFSET / (NAND_PAGE_DATA * NAND_PAGES_PER_BLOCK))
#define FIRST_PAGE      (FIRST_BLOCK * NAND_PAGES_PER_BLOCK)

_Static_assert(FLASH_FS_OFFSET + GZ_FLASH_BLOCKS * NAND_PAGES_PER_BLOCK * NAND_PAGE_DATA == 16u << 20,
               "the file server's region runs to the end of the 16 MiB NAND");

static uint8_t page_buf[NAND_PAGE_DATA] __attribute__((aligned(4)));

static int check(struct proc *p, uint32_t n, uint32_t limit)
{
    if (!(p->flags & PROC_FLASH))
        return GZ_EPERM;
    return n < limit ? 0 : GZ_EINVAL;
}

int sys_flash_read(struct proc *p, uint32_t page, uint32_t ubuf)
{
    int rv = check(p, page, GZ_FLASH_BLOCKS * GZ_FLASH_PAGES_PER_BLOCK);

    if (rv)
        return rv;
    nand_read_page(FIRST_PAGE + page, page_buf);
    return copy_to_as(&p->as, ubuf, page_buf, NAND_PAGE_DATA);
}

int sys_flash_prog(struct proc *p, uint32_t page, uint32_t ubuf)
{
    int rv = check(p, page, GZ_FLASH_BLOCKS * GZ_FLASH_PAGES_PER_BLOCK);

    if (rv)
        return rv;
    if (copy_from_as(&p->as, page_buf, ubuf, NAND_PAGE_DATA))
        return GZ_EINVAL;
    nand_program_page(FIRST_PAGE + page, page_buf);
    return 0;
}

int sys_flash_erase(struct proc *p, uint32_t block)
{
    int rv = check(p, block, GZ_FLASH_BLOCKS);

    if (rv)
        return rv;
    nand_erase_block(FIRST_BLOCK + block);
    return 0;
}
