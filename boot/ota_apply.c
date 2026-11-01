/*
 * The SPL's OTA apply engine (ota_apply.h, docs/OTA.md).
 *
 * The kernel verified the update image (signature, digest, layout), wrote a
 * CRC-32 of every target block into the journal, froze the file system and
 * reset. Here, before the kernel loads:
 *
 *   1. The journal is read (RS-corrected, CRC-checked) and the staged file
 *      opened through a read-only littlefs on Dhara (servers/fs/ftl.c with
 *      FTL_READONLY), on the file system region the GZNAND1 manifest gives.
 *   2. Every target block is read from the file, inflated (puff, from
 *      zlib) and checked against the journal first, along with the new
 *      kernel's header and image CRC and the romfs table. Any problem before
 *      the first write rejects the journal: it is erased and the old OS
 *      boots untouched.
 *   3. A marker page in the journal block records that writing started.
 *      Then each target block whose content doesn't already match is erased
 *      and programmed with the stock tag (AA 55 0F FF, logical number) and
 *      RS parity, and read back. A block that fails moves to a free block of
 *      the reserve pool, as Windows CE's and the ROM's tag search allow.
 *   4. The journal block is erased.
 *
 * A power cut anywhere repeats this from the start on the next boot; the
 * blocks already written then match and are skipped. The file system isn't
 * written until the new kernel's file server mounts it, so the staged file
 * can't move. Only once writing has started is a failure fatal, since the
 * boot blocks may by then be a mix of old and new.
 */

#include "ota_apply.h"
#include <string.h>
#include <glaze/nand_image.h>
#include "../kernel/drivers/nand_ecc.h"
#include "../servers/fs/ftl.h"
#include "puff.h"

#define PAGE            512u
#define PPB             32u
#define BLOCK           (PAGE * PPB)
#define FS_FIRST_PAGE   (GZ_NAND_FS_FIRST * PPB)
#define NAND_BLOCKS     1024u
#define MARKER_PAGE     GZ_OTA_TAG_PAGES    /* after the tagged pages */
#define MARKER          0x534F5A47u         /* "GZOS": writing has started */
#define ATTEMPTS        4

static const struct ota_nand *io;
static struct nand_boot_map *map;
static uint8_t raw[528] __attribute__((aligned(4)));
static uint8_t check[528] __attribute__((aligned(4)));
static uint8_t page_buf[PAGE] __attribute__((aligned(4)));
static uint8_t block_buf[BLOCK] __attribute__((aligned(4)));
static uint8_t packed[BLOCK] __attribute__((aligned(4)));     /* a block as stored */
static struct gz_ota_journal journal;
static struct gz_nand_manifest manifest;
static uint32_t crc_table[256];
static uint8_t failed[NAND_BLOCKS / 8];     /* blocks that failed this boot */

static lfs_t lfs;
static lfs_file_t file;
static uint8_t file_buf[PAGE] __attribute__((aligned(4)));
static const struct lfs_file_config file_cfg = { .buffer = file_buf };

/* ---- read-only ftl.c hooks: the file system region of the physical layout -- */

int flash_raw_is_bad(uint32_t block)
{
    uint32_t phys = GZ_NAND_FS_FIRST + block;

    return phys >= NAND_BLOCKS || ((manifest.bad[phys / 8] >> (phys % 8)) & 1);
}

int flash_raw_read(uint32_t page, uint8_t *buf)
{
    if (io->read_raw(FS_FIRST_PAGE + page, raw))
        return -1;
    memcpy(buf, raw, PAGE);
    return 0;
}

void ftl_log(const char *msg)
{
    (void)msg;
}

/* ---- helpers ---------------------------------------------------------------- */

uint32_t ota_crc32(uint32_t crc, const volatile uint8_t *p, uint32_t n)
{
    if (!crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (unsigned j = 0; j < 8; j++)
                c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
            crc_table[i] = c;
        }
    }
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

static int boot_raw(void *ctx, uint32_t page, uint8_t buf[528])
{
    (void)ctx;
    return io->read_raw(page, buf);
}

static bool all_ff(const uint8_t *p, uint32_t n)
{
    while (n--)
        if (*p++ != 0xFF)
            return false;
    return true;
}

/* The data of a corrected, tagged journal-block page. */
static int journal_page(uint32_t page, void *out)
{
    if (io->read_raw(map->journal * PPB + page, raw) || nand_ecc_check(raw) < 0)
        return -1;
    memcpy(out, raw, PAGE);
    return 0;
}

static bool started(void)
{
    uint32_t word;

    if (io->read_raw(map->journal * PPB + MARKER_PAGE, raw))
        return true;            /* can't tell: assume the worst */
    memcpy(&word, raw, sizeof(word));
    return !all_ff(raw, PAGE) || word == MARKER;
}

static void mark_started(void)
{
    uint32_t word = MARKER;

    memset(raw, 0xFF, sizeof(raw));
    memcpy(raw, &word, sizeof(word));
    io->program_raw(map->journal * PPB + MARKER_PAGE, raw);
}

/* Clears a block's tag pages, so neither our scan nor the ROM's takes it
 * for a logical block or a journal again. Best effort: it may be failing. */
static void kill_tags(uint32_t phys)
{
    memset(raw, 0, sizeof(raw));
    for (uint32_t p = 0; p < GZ_OTA_TAG_PAGES; p++)
        io->program_raw(phys * PPB + p, raw);
}

static void journal_done(void)
{
    if (io->erase(map->journal))
        kill_tags(map->journal);
    map->journal = NAND_BOOT_NONE;
}

static bool manifest_bad(uint32_t phys)
{
    return (manifest.bad[phys / 8] >> (phys % 8)) & 1;
}

static bool mapped(uint32_t phys)
{
    for (uint32_t i = 0; i < NAND_BOOT_BLOCKS; i++)
        if (map->block[i] == phys)
            return true;
    return false;
}

/* A block left with cleared tags by kill_tags. */
static bool dead(uint32_t phys)
{
    if (io->read_raw(phys * PPB, raw))
        return true;
    return !raw[512] && !raw[513] && !raw[514] && !raw[515];
}

static bool usable(uint32_t phys)
{
    return phys < NAND_BLOCKS && phys != map->journal && !manifest_bad(phys) &&
           !((failed[phys / 8] >> (phys % 8)) & 1) && !mapped(phys) && !dead(phys);
}

/* Where logical block `logical` goes: where it is, else its own physical
 * block (where the packer put every good one), else the reserve pool. */
static int target_for(uint32_t logical)
{
    if (map->block[logical] != NAND_BOOT_NONE)
        return map->block[logical];
    if (usable(logical))
        return (int)logical;
    for (uint32_t phys = GZ_NAND_RESERVE_FIRST; phys < NAND_BLOCKS; phys++)
        if (usable(phys))
            return (int)phys;
    return -1;
}

/* ---- the image ---------------------------------------------------------------- */

static int file_read(uint32_t at, void *buf, uint32_t n)
{
    if (at > journal.file_size || n > journal.file_size - at ||
        lfs_file_seek(&lfs, &file, (lfs_soff_t)at, LFS_SEEK_SET) < 0 ||
        lfs_file_read(&lfs, &file, buf, n) != (lfs_ssize_t)n)
        return GZ_OTA_E_READ;
    return 0;
}

/* Target block b (the b-th from GZ_OTA_FIRST_BLOCK) from the staged file,
 * inflated into block_buf, checked against the journal. */
static int source_block(uint32_t b)
{
    struct gz_ota_block e;

    memset(block_buf, 0xFF, BLOCK);
    if (b < journal.blocks) {
        if (file_read(journal.table_offset + b * sizeof(e), &e, sizeof(e)) || e.size == 0 ||
            e.size > BLOCK || file_read(e.offset, e.size == BLOCK ? block_buf : packed, e.size))
            return GZ_OTA_E_READ;
        if (e.size < BLOCK) {
            unsigned long out = BLOCK, in = e.size;
            if (puff(block_buf, &out, packed, &in) || out != BLOCK)
                return GZ_OTA_E_CRC;
        }
    }
    return ota_crc32(0, block_buf, BLOCK) == journal.block_crc[b] ? 0 : GZ_OTA_E_CRC;
}

bool ota_kernel_header_ok(const struct gz_kernel_header *h)
{
    return h->magic == GZ_KHDR_MAGIC && h->format == GZ_KHDR_FORMAT &&
           h->header_crc == ota_crc32(0, (const uint8_t *)h, sizeof(*h) - 4) &&
           h->size != 0 && h->size <= GZ_KERNEL_IMAGE_MAX &&
           h->load >= GZ_KERNEL_LOAD_MIN && h->load <= GZ_KERNEL_LOAD_END - h->size &&
           h->entry >= h->load && h->entry < h->load + h->size && !(h->entry & 3);
}

/* What the new blocks must hold beyond their CRCs, checked as they go by:
 * a kernel the SPL will load (header, image CRC, this version), and a romfs
 * table the kernel will take. */
static struct gz_kernel_header new_kernel;
static uint32_t image_crc;

static int check_block(uint32_t b)
{
    if (b < GZ_OTA_KERNEL_BLOCKS) {
        if (b == 0) {
            memcpy(&new_kernel, block_buf, sizeof(new_kernel));
            if (!ota_kernel_header_ok(&new_kernel) ||
                strncmp(new_kernel.version, journal.version, sizeof(journal.version)))
                return GZ_OTA_E_FORMAT;
            image_crc = 0;
        }
        uint32_t lo = b * BLOCK, hi = lo + BLOCK, from = sizeof(new_kernel), to = from + new_kernel.size;
        if (lo < to && hi > from) {
            uint32_t a = lo > from ? lo : from, z = hi < to ? hi : to;
            image_crc = ota_crc32(image_crc, block_buf + (a - lo), z - a);
        }
        if (b == GZ_OTA_KERNEL_BLOCKS - 1 && image_crc != new_kernel.image_crc)
            return GZ_OTA_E_FORMAT;
    } else if (b == GZ_OTA_KERNEL_BLOCKS) {
        uint32_t magic, count, off, len;
        memcpy(&magic, block_buf, 4);
        memcpy(&count, block_buf + 4, 4);
        if (magic != 0x46525A47u || count > 256 || 8 + count * 56 > BLOCK)   /* "GZRF" */
            return GZ_OTA_E_FORMAT;
        for (uint32_t i = 0; i < count; i++) {
            memcpy(&off, block_buf + 8 + i * 56 + 48, 4);
            memcpy(&len, block_buf + 8 + i * 56 + 52, 4);
            if (off % 4096 || off < 8 + count * 56 || off > GZ_ROMFS_MAX || len > GZ_ROMFS_MAX - off)
                return GZ_OTA_E_FORMAT;
        }
    }
    return 0;
}

/* Whether logical block `logical` already holds target block b. */
static bool current_matches(uint32_t logical, uint32_t b)
{
    uint32_t crc = 0;

    if (map->block[logical] == NAND_BOOT_NONE)
        return false;
    for (uint32_t p = 0; p < PPB; p++) {
        if (nand_boot_read(map, boot_raw, NULL, logical * PPB + p, page_buf, true))
            return false;
        crc = ota_crc32(crc, page_buf, PAGE);
    }
    return crc == journal.block_crc[b];
}

static int program_block(uint32_t phys, uint32_t logical)
{
    if (io->erase(phys))
        return -1;
    for (uint32_t p = 0; p < PPB; p++) {
        memcpy(raw, block_buf + p * PAGE, PAGE);
        raw[512] = 0xAA; raw[513] = 0x55; raw[514] = 0x0F; raw[515] = 0xFF;
        raw[516] = (uint8_t)logical; raw[517] = (uint8_t)(logical >> 8);
        raw[518] = 0; raw[519] = 0;
        nand_ecc_encode(raw);
        if (io->program_raw(phys * PPB + p, raw) || io->read_raw(phys * PPB + p, check) ||
            nand_ecc_check(check) < 0 || memcmp(check, raw, NAND_ECC_BYTES))
            return -1;
    }
    return 0;
}

/* block_buf to logical block `logical`, moving it to the reserve pool if
 * its block fails. */
static int write_block(uint32_t logical)
{
    for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
        int phys = target_for(logical);
        if (phys < 0)
            return -1;
        map->block[logical] = NAND_BOOT_NONE;
        if (!program_block((uint32_t)phys, logical)) {
            map->block[logical] = (uint16_t)phys;
            map->protected_block[logical] = 1;
            return 0;
        }
        failed[phys / 8] |= (uint8_t)(1u << (phys % 8));
        kill_tags((uint32_t)phys);
    }
    return -1;
}

static int load_journal(void)
{
    uint8_t *j = (uint8_t *)&journal;

    if (map->journals != 1)
        return GZ_OTA_E_JOURNAL;
    for (uint32_t p = 0; p < GZ_OTA_JOURNAL_PAGES; p++)
        if (journal_page(p, j + p * PAGE))
            return GZ_OTA_E_JOURNAL;
    if (journal.magic != GZ_OTA_JOURNAL_TAG || journal.format != GZ_OTA_JOURNAL_FORMAT ||
        journal.crc != ota_crc32(0, j, sizeof(journal) - 4) ||
        journal.blocks < GZ_OTA_MIN_BLOCKS || journal.blocks > GZ_OTA_BLOCKS ||
        journal.file_size > GZ_OTA_FILE_MAX || journal.table_offset > journal.file_size ||
        journal.path[0] != '/' || !memchr(journal.path, 0, sizeof(journal.path)))
        return GZ_OTA_E_JOURNAL;
    if (nand_boot_read(map, boot_raw, NULL, 1, &manifest, true) ||
        memcmp(manifest.magic, "GZNAND1", 8) || manifest.fs_first != GZ_NAND_FS_FIRST ||
        manifest.fs_blocks != GZ_NAND_FS_BLOCKS || manifest.crc32 != gz_nand_manifest_crc(&manifest))
        return GZ_OTA_E_JOURNAL;
    return 0;
}

static int open_source(void)
{
    struct lfs_info st;

    if (ftl_mount(&lfs, manifest.fs_blocks))
        return GZ_OTA_E_MOUNT;
    if (lfs_stat(&lfs, journal.path, &st) || st.type != LFS_TYPE_REG || st.size != journal.file_size ||
        lfs_file_opencfg(&lfs, &file, journal.path, LFS_O_RDONLY, &file_cfg))
        return GZ_OTA_E_FILE;
    return 0;
}

static int verify_all(void)
{
    for (uint32_t b = 0; b < GZ_OTA_BLOCKS; b++) {
        int rv = source_block(b);
        if (!rv)
            rv = check_block(b);
        if (rv)
            return rv;
        io->progress(b + 1, 2 * GZ_OTA_BLOCKS);
    }
    return 0;
}

int ota_apply(const struct ota_nand *nand, struct nand_boot_map *boot_map, struct gz_spl_info *info)
{
    io = nand;
    map = boot_map;
    info->ota_result = GZ_OTA_NONE;
    info->ota_detail = 0;
    if (map->journal == NAND_BOOT_NONE)
        return OTA_APPLY_NONE;
    memset(&manifest, 0, sizeof(manifest));
    memset(&journal, 0, sizeof(journal));
    memset(failed, 0, sizeof(failed));

    bool resuming = started();
    int rv = load_journal();
    if (!rv)
        rv = open_source();
    if (!rv)
        rv = verify_all();
    if (rv) {
        info->ota_detail = (uint32_t)rv;
        memcpy(info->ota_version, journal.version, sizeof(info->ota_version) - 1);
        if (resuming)
            return OTA_APPLY_FAILED;
        journal_done();
        info->ota_result = GZ_OTA_REJECTED;
        return OTA_APPLY_REJECTED;
    }
    if (!resuming)
        mark_started();

    uint32_t written = 0;
    for (uint32_t b = 0; b < GZ_OTA_BLOCKS; b++) {
        uint32_t logical = GZ_OTA_FIRST_BLOCK + b;
        if (!current_matches(logical, b)) {
            if (source_block(b) || write_block(logical)) {
                info->ota_detail = GZ_OTA_E_READ;
                return OTA_APPLY_FAILED;
            }
            written++;
        }
        io->progress(GZ_OTA_BLOCKS + b + 1, 2 * GZ_OTA_BLOCKS);
    }
    lfs_file_close(&lfs, &file);
    journal_done();
    info->ota_result = GZ_OTA_APPLIED;
    info->ota_detail = written;
    memcpy(info->ota_version, journal.version, sizeof(info->ota_version) - 1);
    return OTA_APPLY_DONE;
}
