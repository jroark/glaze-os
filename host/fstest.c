/*
 * Native test of the file server's flash stack (servers/fs/ftl.c, Dhara,
 * littlefs) on a RAM model of the NAND region with injected faults:
 *
 *   - programs only clear bits, erases set them (real NAND semantics)
 *   - random bit flips on read, up to ECC_FLIPS per page
 *   - blocks that start failing: programs and erases stop sticking
 *   - power cuts at a random raw operation, tearing that operation
 *
 * usage: ./fstest [seed [wear|flips|bad|cuts|format]]
 */

#include <setjmp.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ftl.h"

#define BLOCKS      640
#define PAGES       (BLOCKS * FTL_PAGES_PER_BLOCK)
#define PAGE        FTL_PAGE_SIZE

static uint8_t flash[PAGES][PAGE];
static uint8_t failing[BLOCKS];
static int flips_per_read;          /* max flipped bits per page read */
static long ops, cut_at = -1;
static jmp_buf cut;
static uint32_t rng = 1;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void op(void)
{
    if (++ops == cut_at)
        longjmp(cut, 1);
}

int flash_raw_read(uint32_t page, uint8_t *buf)
{
    if (page >= PAGES)
        return -1;
    memcpy(buf, flash[page], PAGE);
    for (int i = flips_per_read ? (int)(rnd() % (flips_per_read + 1)) : 0; i > 0; i--)
        buf[rnd() % PAGE] ^= (uint8_t)(1u << (rnd() % 8));
    return 0;
}

int flash_raw_prog(uint32_t page, const uint8_t *buf)
{
    uint32_t n = PAGE;

    if (page >= PAGES)
        return -1;
    if (ops + 1 == cut_at)
        n = rnd() % PAGE;           /* torn program */
    if (failing[page / FTL_PAGES_PER_BLOCK])
        n = PAGE / 3;               /* a worn-out block loses the rest */
    for (uint32_t i = 0; i < n; i++)
        flash[page][i] &= buf[i];
    op();
    return 0;
}

int flash_raw_erase(uint32_t block)
{
    uint32_t n = FTL_PAGES_PER_BLOCK;

    if (block >= BLOCKS)
        return -1;
    if (ops + 1 == cut_at)
        n = rnd() % n;              /* torn erase */
    if (failing[block])
        n = 1;
    memset(flash[block * FTL_PAGES_PER_BLOCK], 0xFF, (size_t)n * PAGE);
    op();
    return 0;
}

static int verbose;

void ftl_log(const char *msg)
{
    if (verbose)
        printf("    [%s]\n", msg);
}

/* ---- helpers ---------------------------------------------------------------- */

static int failures;

static void fail(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    printf("FAIL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    failures++;
}

static void fill(uint8_t *buf, size_t n, uint32_t seed)
{
    uint32_t s = seed * 2654435761u + 1;

    for (size_t i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u;
        buf[i] = (uint8_t)(s >> 16);
    }
}

static int write_file(lfs_t *lfs, const char *path, uint32_t seed, size_t len)
{
    static uint8_t buf[64 * 1024];
    lfs_file_t f;
    int rv;

    fill(buf, len, seed);
    rv = lfs_file_open(lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (rv)
        return rv;
    rv = lfs_file_write(lfs, &f, buf, len);
    int rc = lfs_file_close(lfs, &f);
    return rv < 0 ? rv : rc;
}

/* 1 if the file holds exactly fill(seed, len). */
static int check_file(lfs_t *lfs, const char *path, uint32_t seed, size_t len)
{
    static uint8_t want[64 * 1024], got[64 * 1024 + 1];
    lfs_file_t f;

    if (lfs_file_open(lfs, &f, path, LFS_O_RDONLY))
        return 0;
    int n = lfs_file_read(lfs, &f, got, sizeof(got));
    lfs_file_close(lfs, &f);
    fill(want, len, seed);
    return n == (int)len && !memcmp(got, want, len);
}

static void reset_flash(void)
{
    memset(flash, 0xFF, sizeof(flash));
    memset(failing, 0, sizeof(failing));
    flips_per_read = 0;
    cut_at = -1;
}

/* ---- tests ------------------------------------------------------------------ */

#define NFILES 24

static uint32_t version[NFILES];
static size_t length[NFILES];

static size_t len_for(int i, uint32_t v)
{
    return 100 + (i * 977 + v * 4093) % (24 * 1024);
}

static void path_for(int i, char *buf)
{
    sprintf(buf, "/d%d/file%02d.bin", i % 4, i);
}

static void check_all(lfs_t *lfs, const char *when)
{
    char path[32];

    for (int i = 0; i < NFILES; i++) {
        path_for(i, path);
        if (!check_file(lfs, path, (uint32_t)i << 16 | version[i], length[i]))
            fail("%s: %s v%u wrong", when, path, version[i]);
    }
}

static void populate(lfs_t *lfs)
{
    char path[32];

    for (int d = 0; d < 4; d++) {
        sprintf(path, "/d%d", d);
        lfs_mkdir(lfs, path);
    }
    for (int i = 0; i < NFILES; i++) {
        path_for(i, path);
        version[i] = 1;
        length[i] = len_for(i, 1);
        if (write_file(lfs, path, (uint32_t)i << 16 | 1, length[i]))
            fail("populate %s", path);
    }
}

/* Rewrites files until `bytes` have been written. */
static void churn(lfs_t *lfs, long bytes)
{
    char path[32];

    while (bytes > 0) {
        int i = (int)(rnd() % NFILES);
        uint32_t v = version[i] + 1;
        size_t len = len_for(i, v);

        path_for(i, path);
        int rv = write_file(lfs, path, (uint32_t)i << 16 | v, len);
        if (rv) {
            fail("churn write %s: %d", path, rv);
            return;
        }
        version[i] = v;
        length[i] = len;
        bytes -= (long)len;
    }
}

static void test_basic(void)
{
    lfs_t lfs;
    struct ftl_stats st;

    printf("basic: format, write, remount\n");
    reset_flash();
    if (ftl_mount(&lfs, BLOCKS) < 0) {
        fail("mount blank");
        return;
    }
    ftl_get_stats(&st);
    printf("  capacity %u KiB in %u blocks\n", st.capacity / 1024, st.blocks);
    populate(&lfs);
    check_all(&lfs, "before remount");
    lfs_unmount(&lfs);
    if (ftl_mount(&lfs, BLOCKS) < 0)
        fail("remount");
    check_all(&lfs, "after remount");
    lfs_unmount(&lfs);
}

static void test_wear(void)
{
    lfs_t lfs;

    printf("wear: 24 MiB of rewrites (wraps the journal several times)\n");
    if (ftl_mount(&lfs, BLOCKS) < 0) {
        fail("mount");
        return;
    }
    churn(&lfs, 24L << 20);
    check_all(&lfs, "after churn");
    lfs_unmount(&lfs);
    if (ftl_mount(&lfs, BLOCKS) < 0)
        fail("remount");
    check_all(&lfs, "after churn remount");
    lfs_unmount(&lfs);
}

static void test_bitflips(void)
{
    lfs_t lfs;
    struct ftl_stats st;

    printf("bit flips: up to 3 bits per page read\n");
    flips_per_read = 3;
    if (ftl_mount(&lfs, BLOCKS) < 0) {
        fail("mount with flips");
        flips_per_read = 0;
        return;
    }
    check_all(&lfs, "reading with flips");
    /* Writes verify by reading back; keep their flips within its slack so
     * this exercises ECC rather than block retirement. */
    flips_per_read = 2;
    churn(&lfs, 2L << 20);
    check_all(&lfs, "writing with flips");
    ftl_get_stats(&st);
    printf("  corrected %u pages, uncorrectable %u\n", st.corrected, st.uncorrectable);
    if (!st.corrected || st.uncorrectable)
        fail("ECC counts");
    lfs_unmount(&lfs);
    flips_per_read = 0;
}

static void test_bad_blocks(void)
{
    lfs_t lfs;
    struct ftl_stats st;

    printf("bad blocks: 12 blocks fail mid-use\n");
    if (ftl_mount(&lfs, BLOCKS) < 0) {
        fail("mount");
        return;
    }
    for (int i = 0; i < 12; i++)
        failing[2 + rnd() % (BLOCKS - 2)] = 1;
    churn(&lfs, 12L << 20);
    check_all(&lfs, "with failing blocks");
    ftl_get_stats(&st);
    printf("  %u blocks marked bad\n", st.bad_blocks);
    if (!st.bad_blocks)
        fail("no blocks were marked bad");
    lfs_unmount(&lfs);
    if (ftl_mount(&lfs, BLOCKS) < 0)
        fail("remount");
    ftl_get_stats(&st);
    if (!st.bad_blocks)
        fail("bad-block table not persisted");
    check_all(&lfs, "bad blocks after remount");
    churn(&lfs, 4L << 20);
    check_all(&lfs, "writing after remount with bad blocks");
    lfs_unmount(&lfs);
}

static void test_power_cuts(int rounds)
{
    printf("power cuts: %d random cuts during writes\n", rounds);
    for (int r = 0; r < rounds; r++) {
        static lfs_t lfs;
        char path[32];
        volatile int i = (int)(rnd() % NFILES);
        volatile uint32_t v = version[i] + 1;
        size_t len = len_for(i, v);

        ops = 0;
        cut_at = 1 + (long)(rnd() % 400);
        if (!setjmp(cut)) {
            if (ftl_mount(&lfs, BLOCKS) < 0) {
                fail("round %d: mount", r);
                return;
            }
            path_for(i, path);
            int rv = write_file(&lfs, path, (uint32_t)i << 16 | v, len);
            if (rv)
                fail("round %d: write %s: %d", r, path, rv);
            version[i] = v;
            length[i] = len;
            lfs_unmount(&lfs);
        }
        cut_at = -1;
        if (ftl_mount(&lfs, BLOCKS) < 0) {
            fail("round %d: mount after cut", r);
            return;
        }
        /* The interrupted file is either its old or its new version. */
        path_for(i, path);
        if (version[i] != v && check_file(&lfs, path, (uint32_t)i << 16 | v, len)) {
            version[i] = v;
            length[i] = len;
        }
        check_all(&lfs, "after power cut");
        lfs_unmount(&lfs);
        if (failures)
            return;
    }
}

static const char *only;

static bool run(const char *name)
{
    return !only || !strcmp(only, name);
}

static void test_format(void)
{
    lfs_t lfs;
    struct lfs_info info;
    lfs_file_t f;

    printf("format: erase a used volume, then fill it\n");
    if (ftl_mount(&lfs, BLOCKS) != 0) {
        fail("mount before format");
        return;
    }
    if (ftl_format(&lfs)) {
        fail("format");
        return;
    }
    if (lfs_stat(&lfs, "/d0", &info) != LFS_ERR_NOENT)
        fail("old files survive a format");
    lfs_unmount(&lfs);
    /* Old sectors must not keep taking space: fill most of the volume. */
    if (ftl_mount(&lfs, BLOCKS) != 0) {
        fail("mount after format");
        return;
    }
    static uint8_t buf[4096];
    fill(buf, sizeof(buf), 99);
    long target = (long)lfs.block_count * 4096 * 4 / 5, done = 0;
    if (lfs_file_open(&lfs, &f, "/big", LFS_O_WRONLY | LFS_O_CREAT))
        fail("open big");
    while (done < target) {
        int rv = lfs_file_write(&lfs, &f, buf, sizeof(buf));
        if (rv < 0) {
            fail("filling after format stopped at %ld KiB: %d", done / 1024, rv);
            break;
        }
        done += rv;
    }
    lfs_file_close(&lfs, &f);
    printf("  wrote %ld KiB\n", done / 1024);
    lfs_unmount(&lfs);
}

int main(int argc, char **argv)
{
    rng = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 1;
    verbose = getenv("FSTEST_VERBOSE") != NULL;
    only = argc > 2 ? argv[2] : NULL;
    test_basic();
    if (run("wear"))
        test_wear();
    if (run("flips"))
        test_bitflips();
    if (run("bad"))
        test_bad_blocks();
    if (run("cuts"))
        test_power_cuts(200);
    if (run("format"))
        test_format();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
