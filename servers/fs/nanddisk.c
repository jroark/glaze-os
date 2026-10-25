/*
 * Windows CE's FAT partition in the NAND, as FatFs drive NAND_DRIVE (/NAND)
 * in a RAM boot (glaze/bootinfo.h).
 *
 * The stock image starts with a table of 16-byte entries in page 0, the
 * start and length in pages at +8 and +12 (0xFFFFFFFF entries unused):
 * the SPL, NK, then a FAT16 volume (tools/wince_assets.py reads the same
 * table). A FAT sector is a NAND page, mapped one to one; the kernel maps
 * those logical pages through Windows CE's block table and corrects them
 * with its spare-area ECC (kernel/drivers/flash.c, nand_ecc.c).
 *
 * NAND pages can only be programmed after their block is erased, so writes
 * go into a copy of one 16 KiB erase block, written back when FatFs syncs,
 * when another block is written, and after every request (card_sync). A
 * block whose changed pages were all still erased is only programmed;
 * otherwise it is erased and every non-blank page programmed again.
 *
 * Not handled yet (docs/GLAZEBOOT.md): writes keep neither Windows CE's
 * block tags nor its ECC, which suits the emulator's NAND; on a device,
 * writes stay off (the loader's nand=ro) until they do.
 */

#include <string.h>
#include "card.h"

#define PAGE        512u
#define PPB         32u
#define TABLE_SLOTS (PAGE / 16)

static uint32_t first, count;           /* the volume, in pages */
static bool open_, writable;

static uint8_t block[PAGE * PPB] __attribute__((aligned(4)));
static uint32_t cached = UINT32_MAX;    /* NAND block held in block[] */
static uint32_t dirty, blank;           /* page bits: changed; erased when read */

static uint32_t u32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool is_blank(const uint8_t *p)
{
    for (uint32_t i = 0; i < PAGE; i++)
        if (p[i] != 0xFF)
            return false;
    return true;
}

/* A FAT boot sector: the 0x55AA mark and a FAT file system type. */
static bool is_fat(const uint8_t *p)
{
    return p[510] == 0x55 && p[511] == 0xAA &&
           (!memcmp(p + 54, "FAT", 3) || !memcmp(p + 82, "FAT32", 5));
}

int nand_disk_open(bool rw)
{
    static uint8_t page[PAGE] __attribute__((aligned(4)));
    uint8_t table[PAGE];

    if (open_ && nand_disk_flush() < 0)
        return GZ_EIO;
    open_ = false;
    int rv = nand_raw_read(0, table);
    if (rv == GZ_ENOENT)            /* GlazeBoot's nand=off */
        return rv;
    if (rv < 0)
        return GZ_EIO;
    for (uint32_t i = 0; i < TABLE_SLOTS; i++) {
        uint32_t start = u32(table + i * 16 + 8), len = u32(table + i * 16 + 12);
        if (start == UINT32_MAX || !len || start < PPB * GZ_NAND_FIRST_WRITABLE ||
            start + len < start || start + len > GZ_NAND_BLOCKS * PPB)
            continue;
        if (nand_raw_read(start, page) < 0 || !is_fat(page))
            continue;
        first = start;
        count = len;
        writable = rw;
        open_ = true;
        cached = UINT32_MAX;
        return 0;
    }
    return GZ_ECORRUPT;              /* no FAT volume in the table */
}

DSTATUS nand_disk_status(void)
{
    if (!open_)
        return STA_NOINIT | STA_NODISK;
    return writable ? 0 : STA_PROTECT;
}

int nand_disk_flush(void)
{
    int rv = 0;

    if (cached == UINT32_MAX || !dirty)
        return 0;
    uint32_t base = cached * PPB;
    bool erase = (dirty & ~blank) != 0;
    if (erase)
        rv = nand_raw_erase(cached);
    for (uint32_t i = 0; i < PPB && !rv; i++) {
        uint8_t *p = block + i * PAGE;
        if (erase ? !is_blank(p) : (dirty >> i) & 1)
            rv = nand_raw_prog(base + i, p);
    }
    /* What was programmed is no longer blank; a failure leaves the block to
     * be read again. */
    if (rv) {
        cached = UINT32_MAX;
    } else {
        blank = 0;
        for (uint32_t i = 0; i < PPB; i++)
            blank |= (uint32_t)is_blank(block + i * PAGE) << i;
    }
    dirty = 0;
    return rv < 0 ? rv : 0;
}

/* Brings NAND block b into block[], writing back the one there. */
static int load(uint32_t b)
{
    int rv;

    if (cached == b)
        return 0;
    if ((rv = nand_disk_flush()) < 0)
        return rv;
    cached = UINT32_MAX;
    blank = 0;
    for (uint32_t i = 0; i < PPB; i++) {
        uint8_t *p = block + i * PAGE;
        if ((rv = nand_raw_read(b * PPB + i, p)) < 0)
            return rv;
        blank |= (uint32_t)is_blank(p) << i;
    }
    cached = b;
    return 0;
}

static bool in_volume(LBA_t sector, UINT n)
{
    return open_ && sector < count && n <= count - sector;
}

DRESULT nand_disk_read(BYTE *buff, LBA_t sector, UINT n)
{
    if (!in_volume(sector, n))
        return open_ ? RES_PARERR : RES_NOTRDY;
    for (; n; n--, sector++, buff += PAGE) {
        uint32_t page = first + (uint32_t)sector;
        if (page / PPB == cached)
            memcpy(buff, block + page % PPB * PAGE, PAGE);
        else if (nand_raw_read(page, buff) < 0)
            return RES_ERROR;
    }
    return RES_OK;
}

DRESULT nand_disk_write(const BYTE *buff, LBA_t sector, UINT n)
{
    if (!in_volume(sector, n))
        return open_ ? RES_PARERR : RES_NOTRDY;
    if (!writable)
        return RES_WRPRT;
    for (; n; n--, sector++, buff += PAGE) {
        uint32_t page = first + (uint32_t)sector;
        if (load(page / PPB) < 0)
            return RES_ERROR;
        uint8_t *p = block + page % PPB * PAGE;
        if (memcmp(p, buff, PAGE)) {
            memcpy(p, buff, PAGE);
            dirty |= 1u << page % PPB;
        }
    }
    return RES_OK;
}

DRESULT nand_disk_ioctl(BYTE cmd, void *buff)
{
    if (!open_)
        return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:
        return nand_disk_flush() < 0 ? RES_ERROR : RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = count;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = PPB;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
