/* Read-only boot content in the ROM/CE logical block namespace. */
#ifndef GLAZE_NAND_BOOT_H
#define GLAZE_NAND_BOOT_H
#include <stdbool.h>
#include <stdint.h>
#define NAND_BOOT_BLOCKS 320u /* immutable image below physical FS arena 0x500000 */
#define NAND_BOOT_NONE 0xFFFFu
struct nand_boot_map {
    uint16_t block[NAND_BOOT_BLOCKS];
    uint8_t raw[528] __attribute__((aligned(4)));
    uint32_t corrected;
    bool protected;
    uint8_t protected_block[NAND_BOOT_BLOCKS];
    uint8_t quarantine[128]; /* conservative CE tag/ECC physical inventory */
};
typedef int (*nand_boot_raw_fn)(void *ctx, uint32_t physical_page, uint8_t raw[528]);
/* Both operations fail closed on read/ECC error, missing or duplicate mapping.
 * allow_no_ecc exists for data-only emulator images with synthetic spare tags.
 * Once any real parity is observed, every selected tag quorum and content
 * page must have valid ECC even in that compatibility mode. */
int nand_boot_scan(struct nand_boot_map *map, nand_boot_raw_fn read, void *ctx,
                   bool allow_no_ecc);
int nand_boot_read(struct nand_boot_map *map, nand_boot_raw_fn read, void *ctx,
                   uint32_t logical_page, void *data, bool allow_no_ecc);
#endif
