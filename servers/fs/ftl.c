/*
 * Dhara + littlefs on the BE-300's small-page NAND.
 *
 * Raw region layout (blocks of 32 x 512-byte pages):
 *
 *   blocks 0, 1   bad-block table, alternating copies (page 0 of each)
 *   blocks 2..    Dhara journal
 *
 * ECC. The spare area can't hold anything: the emulator synthesizes it and
 * its image files store page data only. With 512-byte pages Dhara writes
 * groups of four pages, three user pages and a checkpoint page that uses
 * only its first 416 bytes (journal.c, choose_ppc). Each page gets a 4-bit
 * BCH code (7 bytes plus a marker byte); the checkpoint page carries the
 * codes for the group's user pages and for itself in that unused tail:
 *
 *   480  user page 0    488  user page 1    496  user page 2
 *   504  checkpoint, over bytes 0..503
 *
 * A user page's code only reaches flash with its checkpoint, so codes for
 * recently written pages are kept in a small cache until then. Pages of a
 * group whose checkpoint was never written (a crash or a padded group) have
 * no code; Dhara never references them.
 *
 * Bad blocks. A program is verified by reading it back and an erase by
 * checking the block reads as erased, allowing for a stray read flip or
 * two; a real mismatch reports the block bad to
 * Dhara, which relocates its data and calls mark_bad. Marks live in the
 * bad-block table rather than in the spare area.
 */

#include <string.h>
#include "ftl.h"
#include "dhara/map.h"
#include "dhara/nand.h"
#include "ecc/bch.h"

#define PAGE            FTL_PAGE_SIZE
#define PPB             FTL_PAGES_PER_BLOCK
#define BBT_BLOCKS      2u
#define MAX_BLOCKS      1024u

#define GROUP_MASK      3u          /* 1 << log2_ppc, minus one */
#define ECC_BASE        480
#define ECC_SLOT        8
#define ECC_VALID       0x00        /* slot marker; 0xFF = no code */
#define CP_COVER        (ECC_BASE + 3 * ECC_SLOT)
#define GC_RATIO        4

#define LFS_BLOCK       4096u
#define SECTORS_PER_LFS (LFS_BLOCK / PAGE)

#define BBT_MAGIC       0x54424B47u     /* "GKBT" */

static const struct bch_def *const bch = &bch_4bit;

static struct dhara_nand nand;
static struct dhara_map map;
static uint8_t map_buf[PAGE];
static struct ftl_stats stats;

/* ---- bad-block table ------------------------------------------------------ */

struct bbt {
    uint32_t magic, gen, blocks;
    uint8_t bad[MAX_BLOCKS / 8];
};

static struct bbt bbt;

/* ---- ECC ------------------------------------------------------------------ */

#define CACHE_N     8

static struct { uint32_t page; uint8_t slot[ECC_SLOT]; } ecc_cache[CACHE_N];
static unsigned cache_next;

/* Checkpoint pages read lately, corrected; page UINT32_MAX = empty. A sector
 * lookup (dhara_map_find) reads the metadata of one checkpoint per radix
 * level, and the upper levels are the same for every lookup, so keeping
 * several saves most of a lookup's raw reads and ECC checks. */
#define CP_N        16

static struct { uint32_t page, used; uint8_t buf[PAGE]; } cp_cache[CP_N];
static uint32_t cp_clock;

/* Recent sector -> page lookups, direct-mapped. Any program or erase may
 * move a sector, so both bump sec_gen, which empties the table. */
#define SEC_N       64

static struct { dhara_sector_t sector; dhara_page_t page; uint32_t gen; } sec_cache[SEC_N];
static uint32_t sec_gen = 1;

static void make_slot(const uint8_t *data, size_t len, uint8_t *slot)
{
    bch_generate(bch, data, len, slot);
    memset(slot + bch->ecc_bytes, 0xFF, ECC_SLOT - bch->ecc_bytes);
    slot[ECC_SLOT - 1] = ECC_VALID;
}

/* Whether a slot holds a code. The marker is voted on so a flipped bit can't
 * make a code look absent. */
static bool has_code(const uint8_t *slot)
{
    return __builtin_popcount(slot[ECC_SLOT - 1]) <= 4;
}

/* 0 = good (possibly after repair), -1 = uncorrectable. Without a code
 * the data passes unless `required`. */
static int ecc_check(uint8_t *data, size_t len, const uint8_t *stored, bool required)
{
    uint8_t ecc[ECC_SLOT];

    if (!has_code(stored))
        return required ? -1 : 0;
    if (!bch_verify(bch, data, len, stored))
        return 0;
    memcpy(ecc, stored, sizeof(ecc));
    bch_repair(bch, data, len, ecc);
    if (bch_verify(bch, data, len, ecc)) {
        stats.uncorrectable++;
        return -1;
    }
    stats.corrected++;
    return 0;
}

/* Only the error budget matters. Visit at most limit + 1 cleared bits,
 * avoiding a software popcount call for every byte on the VR4131. */
static bool zero_bits_within(const uint8_t *p, size_t n, unsigned limit)
{
    for (size_t i = 0; i < n; i++) {
        unsigned bits = (uint8_t)~p[i];
        while (bits) {
            if (!limit)
                return false;
            limit--;
            bits &= bits - 1;
        }
    }
    return true;
}

static bool all_ff(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0xFF)
            return false;
    }
    return true;
}

static void cache_put(uint32_t page, const uint8_t *slot)
{
    for (unsigned i = 0; i < CACHE_N; i++) {
        if (ecc_cache[i].page == page) {
            memcpy(ecc_cache[i].slot, slot, ECC_SLOT);
            return;
        }
    }
    ecc_cache[cache_next].page = page;
    memcpy(ecc_cache[cache_next].slot, slot, ECC_SLOT);
    cache_next = (cache_next + 1) % CACHE_N;
}

static bool cache_get(uint32_t page, uint8_t *slot)
{
    for (unsigned i = 0; i < CACHE_N; i++) {
        if (ecc_cache[i].page == page) {
            memcpy(slot, ecc_cache[i].slot, ECC_SLOT);
            return true;
        }
    }
    return false;
}

static void cache_drop_block(uint32_t block)
{
    for (unsigned i = 0; i < CACHE_N; i++) {
        if (ecc_cache[i].page != UINT32_MAX && ecc_cache[i].page / PPB == block)
            ecc_cache[i].page = UINT32_MAX;
    }
    for (unsigned i = 0; i < CP_N; i++) {
        if (cp_cache[i].page != UINT32_MAX && cp_cache[i].page / PPB == block)
            cp_cache[i].page = UINT32_MAX;
    }
}

static void cp_drop(uint32_t page)
{
    for (unsigned i = 0; i < CP_N; i++) {
        if (cp_cache[i].page == page)
            cp_cache[i].page = UINT32_MAX;
    }
}

static void caches_reset(void)
{
    for (unsigned i = 0; i < CACHE_N; i++)
        ecc_cache[i].page = UINT32_MAX;
    for (unsigned i = 0; i < CP_N; i++)
        cp_cache[i].page = UINT32_MAX;
    sec_gen++;
}

/* ---- raw access (Dhara page numbers) -------------------------------------- */

static uint32_t phys_page(dhara_page_t p)
{
    return p + BBT_BLOCKS * PPB;
}

static int raw_read(dhara_page_t p, uint8_t *buf)
{
    return flash_raw_read(phys_page(p), buf);
}

/* Bits a program or erase may be off by on read-back and still count as
 * good: an occasional read flip is ECC's job, not a sign of a failing block. */
#define VERIFY_SLACK    2
#define ERASED_SLACK    4

static bool diff_bits_within(const uint8_t *a, const uint8_t *b, size_t n, unsigned limit)
{
    for (size_t i = 0; i < n; i++) {
        unsigned bits = a[i] ^ b[i];
        while (bits) {
            if (!limit)
                return false;
            limit--;
            bits &= bits - 1;
        }
    }
    return true;
}

static int raw_prog_verify(uint32_t phys, const uint8_t *buf)
{
    uint8_t check[PAGE];

    if (flash_raw_prog(phys, buf) || flash_raw_read(phys, check) ||
        !diff_bits_within(check, buf, PAGE, VERIFY_SLACK))
        return -1;
    return 0;
}

static int raw_erase_verify(uint32_t phys_block)
{
    uint8_t check[PAGE];

    if (flash_raw_erase(phys_block))
        return -1;
    for (uint32_t i = 0; i < PPB; i++) {
        if (flash_raw_read(phys_block * PPB + i, check) ||
            !zero_bits_within(check, PAGE, VERIFY_SLACK))
            return -1;
    }
    return 0;
}

/* Whether a page reads as erased. Allows a few flipped bits: a search for
 * the end of the journal that took a flip for data would settle on an older
 * checkpoint. Dhara only reprograms pages past its last checkpoint, so a
 * nearly-all-0xFF page taken for erased costs nothing (journal.c, cp_free). */
static bool reads_erased(const uint8_t *buf)
{
    return zero_bits_within(buf, PAGE, ERASED_SLACK);
}

/* Reads and corrects checkpoint page `p` into the cache and returns its
 * contents, or NULL on error. A checkpoint whose
 * code never reached flash was cut off by a power loss (pages program from
 * the first byte to the last, the code's marker last); like an erased one
 * it reads as all 0xFF, which keeps it out of resume and lets garbage
 * collection discard its group. A complete one that fails ECC is an error. */
static const uint8_t *load_checkpoint(dhara_page_t p, dhara_error_t *err)
{
    unsigned victim = 0;

    for (unsigned i = 0; i < CP_N; i++) {
        if (cp_cache[i].page == p) {
            cp_cache[i].used = ++cp_clock;
            return cp_cache[i].buf;
        }
    }
    /* An empty entry, else the least recently used. */
    for (unsigned i = 0; i < CP_N; i++) {
        if (cp_cache[i].page == UINT32_MAX) {
            victim = i;
            break;
        }
        if (cp_cache[i].used < cp_cache[victim].used)
            victim = i;
    }
    uint8_t *buf = cp_cache[victim].buf;
    cp_cache[victim].page = UINT32_MAX;
    if (raw_read(p, buf)) {
        dhara_set_error(err, DHARA_E_ECC);
        return NULL;
    }
    if (reads_erased(buf) || !has_code(buf + CP_COVER)) {
        memset(buf, 0xFF, PAGE);
    } else if (ecc_check(buf, CP_COVER, buf + CP_COVER, true)) {
        dhara_set_error(err, DHARA_E_ECC);
        return NULL;
    }
    cp_cache[victim].page = p;
    cp_cache[victim].used = ++cp_clock;
    return buf;
}

/* The stored code for user page `p`, or an all-0xFF slot if it has none. */
static int user_slot(dhara_page_t p, uint8_t *slot, dhara_error_t *err)
{
    if (cache_get(p, slot))
        return 0;
    const uint8_t *cp = load_checkpoint(p | GROUP_MASK, err);
    if (!cp)
        return -1;
    memcpy(slot, cp + ECC_BASE + (p & GROUP_MASK) * ECC_SLOT, ECC_SLOT);
    return 0;
}

/* ---- Dhara NAND interface -------------------------------------------------- */

int dhara_nand_is_bad(const struct dhara_nand *n, dhara_block_t b)
{
    uint32_t pb = b + BBT_BLOCKS;

    return (bbt.bad[pb / 8] >> (pb % 8)) & 1;
}

static void bbt_save(void);

void dhara_nand_mark_bad(const struct dhara_nand *n, dhara_block_t b)
{
    uint32_t pb = b + BBT_BLOCKS;

    if (dhara_nand_is_bad(n, b))
        return;
    bbt.bad[pb / 8] |= (uint8_t)(1u << (pb % 8));
    stats.bad_blocks++;
    ftl_log("fs: marked a block bad");
    bbt_save();
}

int dhara_nand_erase(const struct dhara_nand *n, dhara_block_t b, dhara_error_t *err)
{
    cache_drop_block(b);
    sec_gen++;
    if (raw_erase_verify(b + BBT_BLOCKS)) {
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }
    return 0;
}

int dhara_nand_prog(const struct dhara_nand *n, dhara_page_t p, const uint8_t *data,
                    dhara_error_t *err)
{
    uint8_t buf[PAGE], slot[ECC_SLOT];

    memcpy(buf, data, PAGE);
    sec_gen++;
    if ((p & GROUP_MASK) == GROUP_MASK) {
        /* Checkpoint: add the group's user-page codes, then its own. */
        for (dhara_page_t k = 0; k < GROUP_MASK; k++) {
            dhara_page_t q = (p & ~GROUP_MASK) | k;
            uint8_t raw[PAGE];

            if (!cache_get(q, slot)) {
                /* Written before a restart (or never): code what's there. */
                if (raw_read(q, raw) || all_ff(raw, PAGE))
                    memset(slot, 0xFF, ECC_SLOT);
                else
                    make_slot(raw, PAGE, slot);
            }
            memcpy(buf + ECC_BASE + k * ECC_SLOT, slot, ECC_SLOT);
        }
        make_slot(buf, CP_COVER, buf + CP_COVER);
        cp_drop(p);
    } else {
        make_slot(buf, PAGE, slot);
        cache_put(p, slot);
    }
    if (raw_prog_verify(phys_page(p), buf)) {
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }
    return 0;
}

int dhara_nand_is_free(const struct dhara_nand *n, dhara_page_t p)
{
    uint8_t buf[PAGE];

    return !raw_read(p, buf) && reads_erased(buf);
}

int dhara_nand_read(const struct dhara_nand *n, dhara_page_t p, size_t offset, size_t length,
                    uint8_t *data, dhara_error_t *err)
{
    uint8_t buf[PAGE], slot[ECC_SLOT];

    if ((p & GROUP_MASK) == GROUP_MASK) {
        const uint8_t *cp = load_checkpoint(p, err);
        if (!cp)
            return -1;
        memcpy(data, cp + offset, length);
        return 0;
    }
    if (raw_read(p, buf) || user_slot(p, slot, err))
        goto fail;
    if (!all_ff(buf, PAGE) && ecc_check(buf, PAGE, slot, false))
        goto fail;
    memcpy(data, buf + offset, length);
    return 0;
fail:
    dhara_set_error(err, DHARA_E_ECC);
    return -1;
}

int dhara_nand_copy(const struct dhara_nand *n, dhara_page_t src, dhara_page_t dst,
                    dhara_error_t *err)
{
    uint8_t buf[PAGE];

    if (dhara_nand_read(n, src, 0, PAGE, buf, err))
        return -1;
    return dhara_nand_prog(n, dst, buf, err);
}

/* ---- bad-block table storage ---------------------------------------------- */

static void bbt_save(void)
{
    uint8_t buf[PAGE];

    bbt.gen++;
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, &bbt, sizeof(bbt));
    make_slot(buf, CP_COVER, buf + CP_COVER);
    /* Alternate copies so a torn write leaves the previous table. */
    uint32_t block = bbt.gen & 1;
    if (raw_erase_verify(block) || raw_prog_verify(block * PPB, buf))
        ftl_log("fs: bad-block table write failed");
}

static bool bbt_load_copy(uint32_t block, struct bbt *out)
{
    uint8_t buf[PAGE];

    if (flash_raw_read(block * PPB, buf) || all_ff(buf, PAGE) ||
        ecc_check(buf, CP_COVER, buf + CP_COVER, true))
        return false;
    memcpy(out, buf, sizeof(*out));
    return out->magic == BBT_MAGIC && out->blocks == nand.num_blocks + BBT_BLOCKS;
}

static void bbt_load(void)
{
    struct bbt a, b;
    bool ok_a = bbt_load_copy(0, &a), ok_b = bbt_load_copy(1, &b);

    if (ok_a && (!ok_b || (int32_t)(a.gen - b.gen) > 0))
        bbt = a;
    else if (ok_b)
        bbt = b;
    else {
        /* Fresh flash. Factory-marked blocks aren't detected: their marks
         * are in the spare area, which the emulator doesn't model. */
        memset(&bbt, 0, sizeof(bbt));
        bbt.magic = BBT_MAGIC;
        bbt.blocks = nand.num_blocks + BBT_BLOCKS;
        bbt_save();
    }
    stats.bad_blocks = 0;
    for (uint32_t i = 0; i < bbt.blocks; i++)
        stats.bad_blocks += (bbt.bad[i / 8] >> (i % 8)) & 1;
}

/* ---- littlefs block device --------------------------------------------------- */

static int lfs_err(dhara_error_t err)
{
    if (err == DHARA_E_MAP_FULL)
        return LFS_ERR_NOSPC;
    return err == DHARA_E_ECC ? LFS_ERR_CORRUPT : LFS_ERR_IO;
}

static int bd_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer,
                   lfs_size_t size)
{
    dhara_sector_t s = block * SECTORS_PER_LFS + off / PAGE;
    dhara_error_t err;

    for (uint8_t *p = buffer; size; size -= PAGE, p += PAGE, s++) {
        /* dhara_map_read, with the lookup cached. */
        unsigned i = s % SEC_N;
        dhara_page_t page;
        if (sec_cache[i].gen == sec_gen && sec_cache[i].sector == s) {
            page = sec_cache[i].page;
        } else if (dhara_map_find(&map, s, &page, &err)) {
            if (err != DHARA_E_NOT_FOUND)
                return lfs_err(err);
            memset(p, 0xFF, PAGE);
            continue;
        } else {
            sec_cache[i].sector = s;
            sec_cache[i].page = page;
            sec_cache[i].gen = sec_gen;
        }
        if (dhara_nand_read(&nand, page, 0, PAGE, p, &err))
            return lfs_err(err);
    }
    return 0;
}

static int bd_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
                   const void *buffer, lfs_size_t size)
{
    dhara_sector_t s = block * SECTORS_PER_LFS + off / PAGE;
    dhara_error_t err;

    for (const uint8_t *p = buffer; size; size -= PAGE, p += PAGE, s++) {
        if (dhara_map_write(&map, s, p, &err))
            return lfs_err(err);
    }
    return 0;
}

/* Nothing to erase under a translation layer, but trimming lets Dhara's
 * garbage collector drop the old contents. */
static int bd_erase(const struct lfs_config *c, lfs_block_t block)
{
    dhara_error_t err;

    for (uint32_t i = 0; i < SECTORS_PER_LFS; i++) {
        if (dhara_map_trim(&map, block * SECTORS_PER_LFS + i, &err))
            return lfs_err(err);
    }
    return 0;
}

static int bd_sync(const struct lfs_config *c)
{
    dhara_error_t err;

    return dhara_map_sync(&map, &err) ? lfs_err(err) : 0;
}

static uint8_t lfs_read_buf[PAGE], lfs_prog_buf[PAGE];
static uint8_t lfs_lookahead[256];

static struct lfs_config cfg = {
    .read = bd_read,
    .prog = bd_prog,
    .erase = bd_erase,
    .sync = bd_sync,
    .read_size = PAGE,
    .prog_size = PAGE,
    .block_size = LFS_BLOCK,
    .block_cycles = -1,         /* Dhara levels wear */
    .cache_size = PAGE,
    .lookahead_size = sizeof(lfs_lookahead),
    .read_buffer = lfs_read_buf,
    .prog_buffer = lfs_prog_buf,
    .lookahead_buffer = lfs_lookahead,
};

int ftl_mount(lfs_t *lfs, uint32_t blocks)
{
    dhara_error_t err;
    bool fresh = false;

    if (blocks > MAX_BLOCKS || blocks <= BBT_BLOCKS)
        return LFS_ERR_INVAL;
    caches_reset();
    memset(&stats, 0, sizeof(stats));
    stats.blocks = blocks;

    nand.log2_page_size = 9;
    nand.log2_ppb = 5;
    nand.num_blocks = blocks - BBT_BLOCKS;
    bbt_load();

    dhara_map_init(&map, &nand, map_buf, GC_RATIO);
    if (dhara_map_resume(&map, &err)) {
        ftl_log("fs: no flash journal, starting a new one");
        dhara_map_clear(&map);
        fresh = true;
    }

    /* littlefs needs a size that never changes, but Dhara's capacity moves
     * with its bad-block estimate, so size the filesystem from the geometry
     * with room for 1/32 of the blocks to go bad (journal.c and map.c
     * capacity formulas, 3 user pages per group of 4). */
    uint32_t good = nand.num_blocks - nand.num_blocks / 32;
    uint32_t sectors = good * PPB / 4 * 3;
    sectors -= sectors / (GC_RATIO + 1) + DHARA_MAX_RETRIES * PPB;
    cfg.block_count = sectors / SECTORS_PER_LFS;
    stats.capacity = cfg.block_count * LFS_BLOCK;
    /* Only blank flash is formatted automatically; a journal that holds no
     * mountable filesystem is left for ftl_format so nothing is wiped
     * behind the user's back. */
    int rv = lfs_mount(lfs, &cfg);
    if (rv && fresh) {
        ftl_log("fs: formatting new flash");
        rv = ftl_format(lfs);
        return rv ? rv : 1;
    }
    return rv;
}

int ftl_format(lfs_t *lfs)
{
    /* Drop every mapped sector so the old contents stop taking space. */
    dhara_map_clear(&map);
    sec_gen++;
    int rv = lfs_format(lfs, &cfg);

    return rv ? rv : lfs_mount(lfs, &cfg);
}

void ftl_get_stats(struct ftl_stats *st)
{
    *st = stats;
}
