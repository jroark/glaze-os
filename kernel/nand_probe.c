/* Read-only hardware diagnostic: scan physical tags/ECC while running from
 * RAM. No NAND program/erase entry points or user file contents are used. */
#include "kernel.h"
#include "hw.h"
#include "proc.h"
#include "drivers/nand.h"
#include "drivers/nand_boot.h"
#include <glaze/bootinfo.h>

static int nand_probe_raw(void *ctx, uint32_t page, uint8_t raw[528]);

void nand_boot_probe(void)
{
    static struct nand_boot_map map;
    uint8_t page[512] __attribute__((aligned(4)));
    uint64_t start = time_us();
    if (!ram_boot || (ram_boot->flags & GZ_BOOT_NAND_OFF)) {
        kputs("nand-probe: requires RAM boot with NAND readable\n");
        return;
    }
    nand_reset();
    if (nand_boot_scan(&map, nand_probe_raw, NULL, false)) {
        kputs("nand-probe: FAIL ambiguous protected block map\n");
        return;
    }
    unsigned mapped = 0;
    for (unsigned i = 0; i < NAND_BOOT_BLOCKS; i++) mapped += map.block[i] != NAND_BOOT_NONE;
    kprintf("nand-probe: strict ECC map %u/%u blocks, %u corrected symbols, %u ms\n",
            mapped, NAND_BOOT_BLOCKS, map.corrected, (unsigned)((time_us()-start)/1000));
    kprintf("nand-probe: logical 0/1/10/16 -> physical %u/%u/%u/%u\n",
            map.block[0], map.block[1], map.block[10], map.block[16]);
    if (nand_boot_read(&map, nand_probe_raw, NULL, 0, page, false)) {
        kputs("nand-probe: FAIL protected partition page unreadable\n");
        return;
    }
    for (unsigned i = 0; i < 4; i++) {
        const uint32_t *part = (const uint32_t *)(page + i * 16);
        kprintf("nand-probe: partition %u start=%u pages=%u\n", i, part[2], part[3]);
    }
    kputs("nand-probe: PASS read-only physical tag/ECC scan and logical partition read\n");
}

static int nand_probe_raw(void *ctx, uint32_t page, uint8_t raw[528])
{
    (void)ctx;
    nand_read_raw(page, raw);
    return 0;
}
