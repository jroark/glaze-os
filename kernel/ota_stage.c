/*
 * Verifying an OTA update and writing its journal (ota_stage.h).
 *
 * Nothing here trusts the file server beyond the order of the bytes: the
 * header must carry the release key's signature, and the rest of the file
 * (the block table and the compressed blocks) must hash to the digest it
 * signs. The table's CRC-32 of each target block goes into the journal for
 * the SPL, which inflates every block from the staged file and checks it
 * against them before it writes anything, and again as it writes; the SPL
 * also checks the new kernel's header and the romfs table then.
 */

#include "ota_stage.h"
#include <string.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/ed25519.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include "drivers/nand_ecc.h"

#define BLOCK           GZ_NAND_BLOCK_BYTES
#define PPB             32u
#define NAND_BLOCKS     1024u

enum { IDLE, STREAMING, VERIFIED };

static int state = IDLE;
static struct gz_ota_header hdr;
static struct gz_ota_journal journal;
static struct gz_ota_block table[GZ_OTA_BLOCKS];
static wc_Sha256 sha;
static uint32_t received;           /* bytes after the header */
static ed25519_key key;
static uint8_t raw[528] __attribute__((aligned(4)));

static bool path_ok(const char *p)
{
    const char *end = memchr(p, 0, GZ_OTA_PATH_MAX);

    /* On the flash volume: the SPL reads only the NAND's littlefs. */
    return p[0] == '/' && end && end > p + 1 && strncmp(p, "/Card", 5) && strncmp(p, "/NAND", 5) &&
           !strstr(p, "/../") && !strstr(p, "//");
}

void ota_stage_abort(void)
{
    if (state == STREAMING)
        wc_Sha256Free(&sha);
    state = IDLE;
    received = 0;
}

int ota_stage_begin(const struct gz_ota_header *h, const char *path, uint32_t file_size,
                    const char *running, uint32_t spl_version, const uint8_t pub[32])
{
    int verified = 0;

    ota_stage_abort();
    if (memcmp(h->magic, GZ_OTA_MAGIC "\0\0", 8) || h->header_size != GZ_OTA_HEADER_SIZE ||
        h->blocks < GZ_OTA_MIN_BLOCKS || h->blocks > GZ_OTA_BLOCKS || h->file_size > GZ_OTA_FILE_MAX ||
        h->file_size < h->header_size + h->blocks * sizeof(struct gz_ota_block) ||
        !gz_version_ok(h->version, sizeof(h->version)) || !memchr(h->build, 0, sizeof(h->build)))
        return GZ_OTA_E_FORMAT;
    if (!path_ok(path) || file_size != h->file_size)
        return GZ_OTA_E_FILE;
    if (wc_ed25519_init(&key) || wc_ed25519_import_public(pub, 32, &key) ||
        wc_ed25519_verify_msg(h->signature, sizeof(h->signature), (const byte *)h,
                              GZ_OTA_SIGNED_BYTES, &verified, &key) || verified != 1) {
        wc_ed25519_free(&key);
        return GZ_OTA_E_SIGNATURE;
    }
    wc_ed25519_free(&key);
    if (gz_version_cmp(h->version, running) <= 0)
        return GZ_OTA_E_VERSION;
    if (h->spl_min > spl_version)
        return GZ_OTA_E_SPL;
    if (wc_InitSha256(&sha))
        return GZ_OTA_E_FORMAT;

    hdr = *h;
    memset(&journal, 0, sizeof(journal));
    journal.magic = GZ_OTA_JOURNAL_TAG;
    journal.format = GZ_OTA_JOURNAL_FORMAT;
    journal.file_size = file_size;
    journal.table_offset = h->header_size;
    journal.blocks = h->blocks;
    memcpy(journal.version, h->version, sizeof(journal.version));
    memcpy(journal.path, path, strlen(path) + 1);
    received = 0;
    state = STREAMING;
    return 0;
}

int ota_stage_data(const void *buf, uint32_t n)
{
    uint32_t table_bytes = hdr.blocks * sizeof(struct gz_ota_block);

    if (state != STREAMING || n > hdr.file_size - hdr.header_size - received) {
        ota_stage_abort();
        return GZ_OTA_E_FORMAT;
    }
    wc_Sha256Update(&sha, buf, n);
    if (received < table_bytes) {
        uint32_t k = n < table_bytes - received ? n : table_bytes - received;
        memcpy((uint8_t *)table + received, buf, k);
    }
    received += n;
    return 0;
}

static uint32_t crc32_ff(uint32_t n)
{
    static const uint8_t ff[64] = {
        [0 ... 63] = 0xFF,
    };
    uint32_t crc = 0;

    for (; n; n -= sizeof(ff))
        crc = gz_crc32(crc, ff, sizeof(ff));
    return crc;
}

int ota_stage_commit(void)
{
    uint8_t digest[WC_SHA256_DIGEST_SIZE];
    uint32_t data = hdr.header_size + hdr.blocks * sizeof(struct gz_ota_block);

    if (state != STREAMING || received != hdr.file_size - hdr.header_size) {
        ota_stage_abort();
        return GZ_OTA_E_FORMAT;
    }
    wc_Sha256Final(&sha, digest);
    wc_Sha256Free(&sha);
    state = IDLE;
    if (memcmp(digest, hdr.body_sha256, sizeof(digest)))
        return GZ_OTA_E_DIGEST;
    for (uint32_t b = 0; b < hdr.blocks; b++) {
        const struct gz_ota_block *e = &table[b];
        if (e->offset < data || e->size == 0 || e->size > BLOCK || e->offset > hdr.file_size ||
            e->size > hdr.file_size - e->offset)
            return GZ_OTA_E_FORMAT;
        journal.block_crc[b] = e->crc;
    }
    uint32_t empty = crc32_ff(BLOCK);
    for (uint32_t b = hdr.blocks; b < GZ_OTA_BLOCKS; b++)
        journal.block_crc[b] = empty;
    journal.crc = gz_crc32(0, &journal, sizeof(journal) - 4);
    state = VERIFIED;
    return 0;
}

const struct gz_ota_journal *ota_stage_journal(void)
{
    return state == VERIFIED ? &journal : NULL;
}

/* ---- the journal block ------------------------------------------------------ */

static bool mapped(const struct nand_boot_map *map, uint32_t phys)
{
    for (uint32_t i = 0; i < NAND_BOOT_BLOCKS; i++)
        if (map->block[i] == phys)
            return true;
    return false;
}

/* Erased, or what's left of a journal: a torn write the scan didn't take
 * for one, or one the SPL couldn't erase. Not a block the SPL gave up on
 * (cleared tags) or anything else. */
static bool reusable(const struct ota_flash *f, uint32_t phys)
{
    bool journal = false;

    for (uint32_t p = 0; p < GZ_OTA_TAG_PAGES; p++) {
        if (f->read_raw(phys * PPB + p, raw))
            return false;
        if (!memcmp(raw + 512, "GZOJ", 4)) {
            journal = true;
            continue;
        }
        for (uint32_t i = 0; i < sizeof(raw); i++)
            if (raw[i] != 0xFF && !journal)
                return false;
    }
    return true;
}

static int pick_block(const struct ota_flash *f, const struct nand_boot_map *map,
                      const struct gz_nand_manifest *m)
{
    if (map->journal != NAND_BOOT_NONE && map->journals == 1)
        return map->journal;
    for (uint32_t phys = GZ_NAND_RESERVE_FIRST; phys < NAND_BLOCKS; phys++)
        if (!((m->bad[phys / 8] >> (phys % 8)) & 1) && !mapped(map, phys) && reusable(f, phys))
            return (int)phys;
    return -1;
}

int ota_journal_write(const struct ota_flash *f, const struct gz_ota_journal *j,
                      const struct nand_boot_map *map, const struct gz_nand_manifest *m)
{
    static uint8_t check[528] __attribute__((aligned(4)));
    int phys = pick_block(f, map, m);

    if (phys < 0)
        return -GZ_OTA_E_SPACE;
    if (f->erase((uint32_t)phys))
        return -GZ_OTA_E_FLASH;
    for (uint32_t p = 0; p < GZ_OTA_TAG_PAGES; p++) {
        uint32_t tag = GZ_OTA_JOURNAL_TAG;

        memset(raw, 0xFF, sizeof(raw));
        if (p < GZ_OTA_JOURNAL_PAGES)
            memcpy(raw, (const uint8_t *)j + p * 512, 512);
        memcpy(raw + 512, &tag, 4);
        nand_ecc_encode(raw);
        uint32_t page = (uint32_t)phys * PPB + p;
        if (f->program_raw(page, raw) || f->read_raw(page, check) || nand_ecc_check(check) < 0 ||
            memcmp(check, raw, NAND_ECC_BYTES))
            return -GZ_OTA_E_FLASH;
    }
    return phys;
}
