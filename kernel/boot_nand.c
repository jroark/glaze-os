/* Shared kernel boot-content mapping and validated physical-image geometry. */
#include "kernel.h"
#include "hw.h"
#include "drivers/nand.h"
#include "drivers/nand_boot.h"
#include <glaze/nand_image.h>
static struct nand_boot_map map;
static bool ready, manifest_ready;
static struct gz_nand_manifest manifest;
static bool physical;
static int raw_read(void *ctx, uint32_t page, uint8_t raw[528])
{
    (void)ctx;
    nand_read_raw(page, raw);
    return 0;
}
void boot_nand_read(uint32_t page, void *buf)
{
    if (!ready) {
        if (nand_boot_scan(&map, raw_read, NULL, true)) panic("nand: ambiguous boot map");
        ready = true;
    }
    if (nand_boot_read(&map, raw_read, NULL, page, buf, true))
        panic("nand: missing or corrupt boot page %u", page);
}
const struct gz_nand_manifest *boot_nand_manifest(void)
{
    if (!manifest_ready) {
        boot_nand_read(1, &manifest);
        if (!memcmp(manifest.magic, "GZNAND1", 8)) {
            unsigned count = 0;
            for (unsigned i = 0; i < 1024; i++) count += (manifest.bad[i/8] >> (i%8)) & 1;
            if (!map.protected || manifest.version != 1 ||
                manifest.fs_first != GZ_NAND_FS_FIRST || manifest.fs_blocks != GZ_NAND_FS_BLOCKS ||
                manifest.reserve_first != GZ_NAND_RESERVE_FIRST || manifest.bad_count != count ||
                manifest.crc32 != gz_nand_manifest_crc(&manifest))
                panic("nand: invalid physical image manifest");
            physical = true;
            kprintf("nand: physical layout FS %u+%u, reserve %u, quarantined %u\n",
                    manifest.fs_first, manifest.fs_blocks, manifest.reserve_first, manifest.bad_count);
        }
        manifest_ready = true;
    }
    return physical ? &manifest : NULL;
}
