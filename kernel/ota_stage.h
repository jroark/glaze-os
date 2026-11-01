/*
 * Verifying an OTA update image as the file server streams it in, and
 * writing the journal the SPL applies (docs/os/OTA.md). Plain C over the
 * structures in glaze/ota.h, so host/ tests it natively with the SPL's
 * apply engine (boot/ota_apply.c). kernel/ota.c is the syscall layer.
 */
#ifndef GLAZE_OTA_STAGE_H
#define GLAZE_OTA_STAGE_H

#include <stdbool.h>
#include <stdint.h>
#include <glaze/nand_image.h>
#include <glaze/ota.h>
#include "drivers/nand_boot.h"

/* Each returns 0 or a GZ_OTA_E_* code. */

/* Checks the header (format, version newer than `running`, spl_min against
 * spl_version, the file's size) and its Ed25519 signature by `key`, and
 * starts hashing. `path` is the staged file on the flash volume. */
int ota_stage_begin(const struct gz_ota_header *h, const char *path, uint32_t file_size,
                    const char *running, uint32_t spl_version, const uint8_t key[32]);
/* The next n payload bytes, in order. */
int ota_stage_data(const void *buf, uint32_t n);
/* After the whole payload: its SHA-256, the kernel header and image CRC,
 * the romfs table; then builds the journal. */
int ota_stage_commit(void);
void ota_stage_abort(void);
/* The journal of a committed image, else NULL. */
const struct gz_ota_journal *ota_stage_journal(void);

struct ota_flash {
    int (*read_raw)(uint32_t page, uint8_t raw[528]);
    int (*program_raw)(uint32_t page, const uint8_t raw[528]);
    int (*erase)(uint32_t block);
};

/* Writes journal j into a free reserve-pool block (or over a stale journal
 * the scan found) and reads it back. Returns the block, or -GZ_OTA_E_*. */
int ota_journal_write(const struct ota_flash *f, const struct gz_ota_journal *j,
                      const struct nand_boot_map *map, const struct gz_nand_manifest *m);

#endif
