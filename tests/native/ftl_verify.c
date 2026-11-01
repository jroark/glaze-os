/* Verification-budget boundaries through the Dhara NAND interface. The RAM
 * model injects differences at the start, middle, and end of read-back pages. */
#include <stdio.h>
#include <string.h>
#include "ftl.h"
#include "dhara/nand.h"

#ifdef FTL_BENCH
#ifdef __mips__
#include <glaze/sys.h>
static uint64_t now_us(void) { return gz_time_us(); }
#define REPEATS 20
#else
#include <time.h>
static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
#define REPEATS 2000
#endif
#endif

static uint8_t stored[FTL_PAGE_SIZE], data[FTL_PAGE_SIZE];
static unsigned flips, offset, spacing, flip_page;
static const struct dhara_nand nand = {.log2_page_size = 9, .log2_ppb = 5, .num_blocks = 128};

int flash_raw_read(uint32_t page, uint8_t *buf)
{
    memcpy(buf, stored, sizeof(stored));
    if (page % FTL_PAGES_PER_BLOCK == flip_page)
        for (unsigned i = 0; i < flips; i++)
            buf[(offset + i * spacing) % sizeof(stored)] ^= (uint8_t)(1u << (i % 8));
    return 0;
}

int flash_raw_prog(uint32_t page, const uint8_t *buf)
{
    (void)page;
    memcpy(stored, buf, sizeof(stored));
    return 0;
}

int flash_raw_erase(uint32_t block)
{
    (void)block;
    memset(stored, 0xff, sizeof(stored));
    return 0;
}

void ftl_log(const char *msg) { (void)msg; }

struct operations {
    int (*is_free)(const struct dhara_nand *, dhara_page_t);
    int (*prog)(const struct dhara_nand *, dhara_page_t, const uint8_t *, dhara_error_t *);
    int (*erase)(const struct dhara_nand *, dhara_block_t, dhara_error_t *);
};
static const struct operations current = {dhara_nand_is_free, dhara_nand_prog, dhara_nand_erase};

#ifdef FTL_BASELINE
int before_dhara_nand_is_free(const struct dhara_nand *, dhara_page_t);
int before_dhara_nand_prog(const struct dhara_nand *, dhara_page_t, const uint8_t *, dhara_error_t *);
int before_dhara_nand_erase(const struct dhara_nand *, dhara_block_t, dhara_error_t *);
static const struct operations baseline = {before_dhara_nand_is_free, before_dhara_nand_prog,
                                           before_dhara_nand_erase};
#endif

static int verify(const struct operations *f)
{
    for (unsigned spread = 0; spread < 2; spread++) {
        spacing = spread ? 173 : 0;
        for (unsigned pos = 0; pos < 3; pos++) {
            offset = pos == 0 ? 0 : pos == 1 ? 255 : 511;
            for (flips = 0; flips <= 8; flips++) {
                dhara_error_t err = DHARA_E_NONE;
                flip_page = 0;
                memset(stored, 0xff, sizeof(stored));
                if (f->is_free(&nand, 0) != (flips <= 4))
                    return 1;
                if ((f->prog(&nand, 0, data, &err) == 0) != (flips <= 2))
                    return 2;
                if (flips > 2 && err != DHARA_E_BAD_BLOCK)
                    return 3;
                for (flip_page = 0; flip_page < FTL_PAGES_PER_BLOCK; flip_page += 31) {
                    err = DHARA_E_NONE;
                    if ((f->erase(&nand, 0, &err) == 0) != (flips <= 2))
                        return 4;
                    if (flips > 2 && err != DHARA_E_BAD_BLOCK)
                        return 5;
                }
            }
        }
    }
    flips = 0;
    memset(stored, 0, sizeof(stored));
    if (f->is_free(&nand, 0))
        return 6;
    return 0;
}

#ifdef FTL_BENCH
static void bench(const struct operations *f, const char *label)
{
    static const char *names[] = {"programmed page", "erased page", "program clean",
                                 "program 2 flips", "erase clean"};
    flip_page = offset = spacing = 0;
    for (int sample = 0; sample < 3; sample++) {
        for (int which = 0; which < 5; which++) {
            flips = which == 3 ? 2 : 0;
            memset(stored, which == 0 ? 0 : 0xff, sizeof(stored));
            unsigned result = 0;
            uint64_t start = now_us();
            for (int i = 0; i < REPEATS; i++) {
                dhara_error_t err;
                if (which < 2)
                    result += f->is_free(&nand, 0);
                else if (which < 4)
                    result += f->prog(&nand, 0, data, &err) == 0;
                else
                    result += f->erase(&nand, 0, &err) == 0;
            }
            printf("ftl-bench: %s %s %u us/%u calls (result %u)\n", names[which], label,
                   (unsigned)(now_us() - start), REPEATS, result);
        }
    }
}
#endif

int main(void)
{
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 39);
    int rv = verify(&current);
#ifdef FTL_BASELINE
    rv |= verify(&baseline);
#endif
    if (rv) {
        printf("ftl verification: FAIL %d\n", rv);
        return 1;
    }
    puts("ftl verification: PASS (2-bit verify and 4-bit erased-page limits)");
#ifdef FTL_BENCH
#ifdef FTL_BASELINE
    bench(&baseline, "before");
#endif
    bench(&current, "after");
#endif
    return 0;
}
