/* Physical Glaze NAND image v1. All words are little-endian. The manifest
 * occupies logical page 1 and is itself protected by the page's RS parity. */
#ifndef GLAZE_NAND_IMAGE_H
#define GLAZE_NAND_IMAGE_H
#include <stdint.h>
#define GZ_NAND_FS_FIRST 320u
#define GZ_NAND_FS_BLOCKS 684u
#define GZ_NAND_RESERVE_FIRST 1004u
struct gz_nand_manifest {
    uint8_t magic[8];             /* "GZNAND1\0" */
    uint32_t version, fs_first, fs_blocks, reserve_first, bad_count;
    uint8_t bad[128];             /* absolute physical block bitmap */
    uint8_t inventory_sha256[32]; /* source inventory, not a recovery backup */
    uint8_t reserved[320];
    uint32_t crc32;               /* preceding 508 bytes */
};
_Static_assert(sizeof(struct gz_nand_manifest) == 512, "NAND manifest page");
static inline uint32_t gz_nand_manifest_crc(const void *p)
{
    const uint8_t *b = p;
    uint32_t crc = ~0u;
    for (unsigned i = 0; i < 508; i++) {
        crc ^= b[i];
        for (unsigned j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
#endif
