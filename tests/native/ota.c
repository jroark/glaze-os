/*
 * OTA updates end to end on a NAND model (make -C host ota-test, driven by
 * tests/ota_test.py): the kernel's verification and journal
 * (kernel/ota_stage.c) and the SPL's apply engine (boot/ota_apply.c, over
 * the read-only flash stack) on a physical image whose file system holds
 * the staged update, as mkfsimg built it. Covers rejected images, power
 * cuts at every stage of the apply, a failing boot block, and a damaged
 * staged file.
 *
 *   ota-test FS_IMAGE STAGED_PATH OLD.bin NEW.gzu NEW.bin PUB.bin BAD_KEY.gzu DOWNGRADE.gzu
 *
 * OLD.bin and NEW.bin are the boot areas (kernel slot, romfs) before and after.
 */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../boot/ota_apply.h"
#include "../../kernel/ota_stage.h"
#include "../../kernel/drivers/nand_ecc.h"

#define PAGES       32768u
#define PPB         32u
#define BLOCK       (512u * PPB)
#define FS_PAGES    (GZ_NAND_FS_BLOCKS * PPB)

static uint8_t nand[PAGES][528];
static uint8_t snapshot[PAGES][528];
static unsigned ops, cut_at;
static jmp_buf cut;
static uint8_t failing[1024];
static uint8_t *fs_image, *old_bin, *new_gzu, *new_bin, *bad_key_gzu, *downgrade_gzu;
static size_t fs_size, old_size, new_size, new_bin_size, bad_key_size, downgrade_size;
static uint8_t pub[32];
static const char *staged;

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    rewind(f);
    uint8_t *p = malloc(*size);
    assert(p && fread(p, 1, *size, f) == *size);
    fclose(f);
    return p;
}

/* ---- the NAND model: programs clear bits, erases set them, power cuts ---- */

static void op(void)
{
    if (cut_at && ++ops >= cut_at)
        longjmp(cut, 1);
}

static int read_raw(uint32_t page, uint8_t raw[528])
{
    assert(page < PAGES);
    memcpy(raw, nand[page], 528);
    return 0;
}

static int program_raw(uint32_t page, const uint8_t raw[528])
{
    assert(page < PAGES);
    op();
    /* A failing block: bits still clear, as NAND cells do, but a stuck one
     * spoils the data, and the chip reports failure. */
    for (unsigned i = 0; i < 528; i++)
        nand[page][i] &= raw[i];
    if (failing[page / PPB]) {
        nand[page][7] &= 0xFE;
        return -1;
    }
    return 0;
}

static int erase(uint32_t block)
{
    assert(block < 1024);
    op();
    if (failing[block])
        return -1;
    memset(nand[block * PPB], 0xFF, BLOCK / 512 * 528);
    return 0;
}

static void progress(uint32_t done, uint32_t total)
{
    assert(done <= total);
}

static const struct ota_nand spl_io = { read_raw, program_raw, erase, progress };
static const struct ota_flash kernel_io = { read_raw, program_raw, erase };

/* ---- images ------------------------------------------------------------------ */

static void tagged(uint32_t logical, uint32_t phys, const uint8_t *data)
{
    for (uint32_t p = 0; p < PPB; p++) {
        uint8_t *raw = nand[phys * PPB + p];
        memcpy(raw, data + p * 512, 512);
        memcpy(raw + 512, "\xAA\x55\x0F\xFF", 4);
        raw[516] = (uint8_t)logical; raw[517] = (uint8_t)(logical >> 8); raw[518] = raw[519] = 0;
        nand_ecc_encode(raw);
    }
}

/* Logical block `logical` of a boot area (the kernel slot on). */
static void boot_block(const uint8_t *area, size_t size, uint32_t logical, uint8_t *out)
{
    memset(out, 0xFF, BLOCK);
    if (logical == 0) {
        struct gz_nand_manifest *m = (void *)(out + 512);
        memset(m, 0, sizeof(*m));
        memcpy(m->magic, "GZNAND1", 8);
        m->version = 1;
        m->fs_first = GZ_NAND_FS_FIRST;
        m->fs_blocks = GZ_NAND_FS_BLOCKS;
        m->reserve_first = GZ_NAND_RESERVE_FIRST;
        m->crc32 = gz_nand_manifest_crc(m);
    } else if (logical >= GZ_OTA_FIRST_BLOCK) {
        uint32_t off = (logical - GZ_OTA_FIRST_BLOCK) * BLOCK;
        if (off < size)
            memcpy(out, area + off, size - off < BLOCK ? size - off : BLOCK);
    }
}

static void build(const uint8_t *gzu, size_t size)
{
    static uint8_t block[BLOCK];

    memset(nand, 0xFF, sizeof(nand));
    memset(failing, 0, sizeof(failing));
    for (uint32_t l = 0; l < NAND_BOOT_BLOCKS; l++) {
        boot_block(gzu, size, l, block);
        tagged(l, l, block);
    }
    assert(fs_size == FS_PAGES * 512u);
    for (uint32_t i = 0; i < FS_PAGES; i++)
        memcpy(nand[GZ_NAND_FS_FIRST * PPB + i], fs_image + i * 512u, 512);
}

static struct nand_boot_map map;
static int boot_raw(void *ctx, uint32_t page, uint8_t raw[528]) { (void)ctx; return read_raw(page, raw); }

static void scan(void)
{
    assert(!nand_boot_scan(&map, boot_raw, NULL, false));
}

/* Whether the boot area now holds gzu's payload, and only one copy of each block. */
static bool installed(const uint8_t *gzu, size_t size)
{
    static uint8_t want[BLOCK];
    uint8_t page[512];

    scan();
    for (uint32_t l = GZ_OTA_FIRST_BLOCK; l < NAND_BOOT_BLOCKS; l++) {
        boot_block(gzu, size, l, want);
        for (uint32_t p = 0; p < PPB; p++)
            if (nand_boot_read(&map, boot_raw, NULL, l * PPB + p, page, false) ||
                memcmp(page, want + p * 512, 512))
                return false;
    }
    return true;
}

static bool fs_untouched(void)
{
    for (uint32_t i = 0; i < FS_PAGES; i++)
        if (memcmp(nand[GZ_NAND_FS_FIRST * PPB + i], fs_image + i * 512u, 512))
            return false;
    return true;
}

/* ---- the kernel's side --------------------------------------------------------- */

static int stage(const uint8_t *gzu, size_t size, const char *path, const char *running, uint32_t spl)
{
    const struct gz_ota_header *h = (const void *)gzu;
    int rv = ota_stage_begin(h, path, (uint32_t)size, running, spl, pub);
    for (size_t at = h->header_size; !rv && at < size; at += GZ_OTA_DATA_MAX)
        rv = ota_stage_data(gzu + at, size - at < GZ_OTA_DATA_MAX ? (uint32_t)(size - at) : GZ_OTA_DATA_MAX);
    return rv ? rv : ota_stage_commit();
}

static int arm(void)
{
    static struct gz_nand_manifest m;

    scan();
    assert(!nand_boot_read(&map, boot_raw, NULL, 1, &m, false));
    return ota_journal_write(&kernel_io, ota_stage_journal(), &map, &m);
}

static int spl(struct gz_spl_info *info)
{
    scan();
    memset(info, 0, sizeof(*info));
    return ota_apply(&spl_io, &map, info);
}

/* ---- tests ----------------------------------------------------------------------- */

static void rejections(void)
{
    uint8_t *copy = malloc(new_size);

    assert(stage(bad_key_gzu, bad_key_size, staged, "1.0", 2) == GZ_OTA_E_SIGNATURE);
    assert(stage(downgrade_gzu, downgrade_size, staged, "1.0", 2) == GZ_OTA_E_VERSION);
    assert(stage(new_gzu, new_size, staged, "1.1", 2) == GZ_OTA_E_VERSION);    /* reinstall */
    assert(stage(new_gzu, new_size, staged, "1.0", 1) == GZ_OTA_E_SPL);
    assert(stage(new_gzu, new_size, "/Card/Updates/x.gzu", "1.0", 2) == GZ_OTA_E_FILE);
    assert(stage(new_gzu, new_size - 1, staged, "1.0", 2) == GZ_OTA_E_FILE);
    memcpy(copy, new_gzu, new_size);
    copy[40] ^= 1;                                  /* the signed build string */
    assert(stage(copy, new_size, staged, "1.0", 2) == GZ_OTA_E_SIGNATURE);
    memcpy(copy, new_gzu, new_size);
    copy[new_size - 100] ^= 0x10;                   /* romfs contents */
    assert(stage(copy, new_size, staged, "1.0", 2) == GZ_OTA_E_DIGEST);
    assert(!ota_stage_journal());
    free(copy);
    assert(stage(new_gzu, new_size, staged, "1.0", 2) == 0 && ota_stage_journal());
    puts("ota: rejects wrong key, tampering, downgrade, reinstall, old SPL, card path, wrong size");
}

static void straight(void)
{
    struct gz_spl_info info;

    build(old_bin, old_size);
    assert(installed(old_bin, old_size));
    scan();
    assert(map.journal == NAND_BOOT_NONE);
    assert(spl(&info) == OTA_APPLY_NONE);
    assert(stage(new_gzu, new_size, staged, "1.0", 2) == 0);
    int phys = arm();
    assert(phys >= (int)GZ_NAND_RESERVE_FIRST);
    scan();
    assert(map.journal == phys && map.journals == 1);
    assert(spl(&info) == OTA_APPLY_DONE && info.ota_result == GZ_OTA_APPLIED);
    assert(!strcmp(info.ota_version, "1.1") && info.ota_detail > 0);
    assert(installed(new_bin, new_bin_size) && map.journal == NAND_BOOT_NONE && fs_untouched());
    assert(spl(&info) == OTA_APPLY_NONE);
    puts("ota: applies a staged update, leaves the file system and no journal");
}

static void power_cuts(void)
{
    struct gz_spl_info info;
    unsigned total, tried = 0;

    build(old_bin, old_size);
    assert(stage(new_gzu, new_size, staged, "1.0", 2) == 0 && arm() > 0);
    memcpy(snapshot, nand, sizeof(nand));
    /* Count the apply's program and erase operations. */
    cut_at = ~0u;
    ops = 0;
    assert(spl(&info) == OTA_APPLY_DONE);
    total = ops;
    for (unsigned at = 1; at <= total; at += at < 40 ? 1 : at > total - 40 ? 1 : 97) {
        memcpy(nand, snapshot, sizeof(nand));
        ops = 0;
        cut_at = at;
        if (!setjmp(cut)) {
            spl(&info);
            assert(!"no power cut");
        }
        /* Sometimes lose power again while resuming. */
        if (tried % 5 == 4) {
            ops = 0;
            cut_at = 1 + tried % 300;
            if (!setjmp(cut))
                spl(&info);
        }
        cut_at = 0;
        int rv = spl(&info);
        assert(rv == OTA_APPLY_DONE || (rv == OTA_APPLY_NONE && at == total));
        assert(installed(new_bin, new_bin_size) && map.journal == NAND_BOOT_NONE && fs_untouched());
        tried++;
    }
    cut_at = 0;
    printf("ota: %u power cuts out of %u flash operations all resume to the new OS\n", tried, total);
}

static void failing_block(void)
{
    struct gz_spl_info info;

    build(old_bin, old_size);
    assert(stage(new_gzu, new_size, staged, "1.0", 2) == 0 && arm() > 0);
    failing[GZ_OTA_FIRST_BLOCK + 20] = 1;           /* romfs's 10th block */
    assert(spl(&info) == OTA_APPLY_DONE);
    assert(installed(new_bin, new_bin_size));
    assert(map.block[GZ_OTA_FIRST_BLOCK + 20] >= GZ_NAND_RESERVE_FIRST);
    puts("ota: a boot block that fails to program moves to the reserve pool");
}

static void damaged_source(void)
{
    struct gz_spl_info info;

    build(old_bin, old_size);
    assert(stage(new_gzu, new_size, staged, "1.0", 2) == 0 && arm() > 0);
    /* Wreck every data page in the middle of the file system: some hold the
     * staged file, and the corruption is beyond the BCH code. */
    for (uint32_t i = FS_PAGES / 3; i < FS_PAGES / 3 + 2048; i++)
        if ((i & 3) != 3)
            for (unsigned b = 0; b < 64; b++)
                nand[GZ_NAND_FS_FIRST * PPB + i][b] ^= 0xA5;
    assert(spl(&info) == OTA_APPLY_REJECTED && info.ota_result == GZ_OTA_REJECTED);
    assert(info.ota_detail == GZ_OTA_E_READ || info.ota_detail == GZ_OTA_E_CRC ||
           info.ota_detail == GZ_OTA_E_MOUNT || info.ota_detail == GZ_OTA_E_FILE);
    assert(installed(old_bin, old_size) && map.journal == NAND_BOOT_NONE);
    assert(spl(&info) == OTA_APPLY_NONE);
    puts("ota: a damaged staged file is rejected before anything is written");
}

/* Power lost while the kernel wrote the journal, after `pages` pages. */
static int torn_arm(unsigned pages)
{
    build(old_bin, old_size);
    assert(stage(new_gzu, new_size, staged, "1.0", 2) == 0);
    ops = 0;
    cut_at = 1 + 1 + pages; /* the erase, then the pages */
    if (!setjmp(cut))
        arm();
    cut_at = 0;
    scan();
    return map.journal;
}

static void torn_journal(void)
{
    struct gz_spl_info info;

    /* Two pages: not a journal. The old OS boots, and the next arm reuses
     * the block. */
    int phys = torn_arm(2);
    assert(phys == NAND_BOOT_NONE);
    assert(spl(&info) == OTA_APPLY_NONE && installed(old_bin, old_size));
    assert(arm() == (int)GZ_NAND_RESERVE_FIRST);
    /* Three pages hold all of it: installed, since the file system was
     * already frozen when the power went. */
    phys = torn_arm(3);
    assert(phys == (int)GZ_NAND_RESERVE_FIRST);
    assert(spl(&info) == OTA_APPLY_DONE && installed(new_bin, new_bin_size));
    /* A journal page beyond repair: rejected, the old OS boots. */
    phys = torn_arm(5);
    for (unsigned b = 100; b < 140; b++)
        nand[phys * PPB + 1][b] ^= 0xFF;
    nand[phys * PPB + 4][512] = 'G';    /* keep three tags readable */
    scan();
    if (map.journal != NAND_BOOT_NONE) {
        assert(spl(&info) == OTA_APPLY_REJECTED && info.ota_detail == GZ_OTA_E_JOURNAL);
    } else {
        assert(spl(&info) == OTA_APPLY_NONE);
    }
    assert(installed(old_bin, old_size) && map.journal == NAND_BOOT_NONE);
    puts("ota: a torn or corrupt journal installs only if complete, else the old OS boots");
}

int main(int argc, char **argv)
{
    size_t n;

    assert(argc == 9);
    fs_image = slurp(argv[1], &fs_size);
    staged = argv[2];
    old_bin = slurp(argv[3], &old_size);
    new_gzu = slurp(argv[4], &new_size);
    new_bin = slurp(argv[5], &new_bin_size);
    uint8_t *key = slurp(argv[6], &n);
    assert(n == 32);
    memcpy(pub, key, 32);
    bad_key_gzu = slurp(argv[7], &bad_key_size);
    downgrade_gzu = slurp(argv[8], &downgrade_size);

    assert(gz_version_cmp("1.10", "1.9") > 0 && gz_version_cmp("1", "1.0") == 0 &&
           gz_version_cmp("1.0", "1.0.1") < 0 && gz_version_cmp("1.x", "1.0") < 0);
    assert(gz_version_ok("1.0", 16) && !gz_version_ok("1..0", 16) && !gz_version_ok("", 16));
    rejections();
    straight();
    failing_block();
    damaged_source();
    torn_journal();
    power_cuts();
    puts("ota-test: PASS");
    return 0;
}
