/*
 * The SPL's OTA apply engine (docs/os/OTA.md): rewrites the logical boot blocks
 * from the kernel slot to the end of romfs from an update image staged in
 * the file system, as the journal the kernel armed describes. Platform
 * independent, so host/ runs it against a NAND model with power cuts.
 */
#ifndef GLAZE_OTA_APPLY_H
#define GLAZE_OTA_APPLY_H

#include <stdint.h>
#include <glaze/ota.h>
#include "../kernel/drivers/nand_boot.h"

struct ota_nand {
    /* A physical 512+16 page; 0 on success. */
    int (*read_raw)(uint32_t page, uint8_t raw[528]);
    /* Programs a physical page's data and spare; 0 if the chip reports success. */
    int (*program_raw)(uint32_t page, const uint8_t raw[528]);
    /* Erases a physical block; 0 if the chip reports success. */
    int (*erase)(uint32_t block);
    /* Called often, with the work done out of total: draw progress, and
     * reload the watchdog. */
    void (*progress)(uint32_t done, uint32_t total);
};

enum {
    OTA_APPLY_NONE,         /* no journal: nothing done */
    OTA_APPLY_DONE,         /* applied (info says what); rescan before loading */
    OTA_APPLY_REJECTED,     /* didn't verify: nothing written, journal erased */
    OTA_APPLY_FAILED,       /* the flash failed mid-way; booting would be unsafe */
};

/* gz_crc32 (glaze/ota.h) by table, since the SPL runs uncached. */
uint32_t ota_crc32(uint32_t crc, const volatile uint8_t *p, uint32_t n);
/* A kernel slot header the SPL would load: magic, format, CRC, load range. */
bool ota_kernel_header_ok(const struct gz_kernel_header *h);

/* Runs the journal map's scan found (map->journal), and leaves the outcome
 * in info's ota_* fields. The map is updated for the blocks it wrote. */
int ota_apply(const struct ota_nand *io, struct nand_boot_map *map, struct gz_spl_info *info);

#endif
