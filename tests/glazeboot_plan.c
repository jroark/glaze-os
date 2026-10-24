/*
 * Host test of GlazeBoot's hand-over plan (wince/glazeboot/plan.h): pages
 * scattered at random over a 16 MiB RAM model, many of them sitting in
 * other pages' destinations, must all end up in place after gb_permute,
 * with the plan's own pages untouched.
 *
 * usage: glazeboot-plan-test [seeds]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *ram;
#define GB_PAGE_PTR(frame)  ((uint32_t *)(ram + (size_t)(frame) * 4096u))
#include "../wince/glazeboot/plan.h"

static uint32_t rng;

static uint32_t next(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

static void fill(uint32_t frame, uint32_t tag)
{
    uint32_t *p = GB_PAGE_PTR(frame);

    for (uint32_t i = 0; i < GB_PAGE / 4; i++)
        p[i] = tag * 2654435761u + i;
}

static int check(uint32_t frame, uint32_t tag)
{
    uint32_t *p = GB_PAGE_PTR(frame);

    for (uint32_t i = 0; i < GB_PAGE / 4; i++)
        if (p[i] != tag * 2654435761u + i)
            return 0;
    return 1;
}

/* One plan: bootinfo at 0xF, a kernel at 0x10, romfs at the top, sources
 * anywhere; `crowded` puts most sources inside the destination ranges. */
static int run(uint32_t seed, uint32_t kernel_pages, uint32_t romfs_pages, int crowded)
{
    static uint8_t used[GB_FRAMES], dst[GB_FRAMES];
    static uint16_t src[GB_MAX_MOVES], to[GB_MAX_MOVES];
    struct gb_control *c;
    uint32_t n = 0, ctrl, romfs_first = (GB_FRAMES - romfs_pages) & ~1u;

    rng = seed;
    memset(used, 0, sizeof(used));
    memset(dst, 0, sizeof(dst));
    to[n++] = 0xF;
    for (uint32_t i = 0; i < kernel_pages; i++)
        to[n++] = (uint16_t)(0x10 + i);
    for (uint32_t i = 0; i < romfs_pages; i++)
        to[n++] = (uint16_t)(romfs_first + i);
    for (uint32_t i = 0; i < n; i++)
        dst[to[i]] = 1;

    /* Sources: distinct frames; crowded plans prefer destination frames. */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t f;
        do {
            f = crowded && next() % 4 ? to[next() % n] : 0x10 + next() % (GB_FRAMES - 0x10);
        } while (used[f]);
        used[f] = 1;
        src[i] = (uint16_t)f;
    }
    /* The plan's pages: free frames that are no destination. */
    uint32_t plan[1 + GB_MAX_MOVE_PAGES + GB_WHO_PAGES], np = 1 + (n + GB_MOVES_PER_PAGE - 1) / GB_MOVES_PER_PAGE + GB_WHO_PAGES;
    for (uint32_t i = 0; i < np; i++) {
        uint32_t f;
        do
            f = 0x10 + next() % (GB_FRAMES - 0x10);
        while (used[f] || dst[f]);
        used[f] = 1;
        plan[i] = f;
    }
    ctrl = plan[0];
    memset(GB_PAGE_PTR(ctrl), 0, GB_PAGE);
    c = (struct gb_control *)GB_PAGE_PTR(ctrl);
    c->magic = GB_CONTROL_MAGIC;
    c->count = n;
    uint32_t k = 1;
    for (uint32_t i = 0; i < (n + GB_MOVES_PER_PAGE - 1) / GB_MOVES_PER_PAGE; i++)
        c->move_frames[i] = plan[k++];
    for (uint32_t i = 0; i < GB_WHO_PAGES; i++)
        c->who_frames[i] = plan[k++];
    for (uint32_t i = 0; i < n; i++) {
        struct gb_move *m = gb_move_at(c, i);
        m->src = src[i];
        m->dst = to[i];
        fill(src[i], i + 1);
    }

    gb_permute(c);

    int ok = c->magic == GB_CONTROL_MAGIC && c->count == n;
    for (uint32_t i = 0; i < n && ok; i++)
        ok = check(to[i], i + 1) && gb_move_at(c, i)->src == src[i];
    if (!ok)
        printf("glazeboot-plan: FAIL seed %u (%u kernel, %u romfs pages, crowded %d)\n",
               seed, kernel_pages, romfs_pages, crowded);
    return ok;
}

int main(int argc, char **argv)
{
    uint32_t seeds = argc > 1 ? (uint32_t)atoi(argv[1]) : 200;
    int failures = 0;

    ram = calloc(GB_FRAMES, GB_PAGE);
    if (!ram)
        return 2;
    for (uint32_t s = 1; s <= seeds; s++) {
        failures += !run(s, 16, 1100, 0);
        failures += !run(s, 16, 1100, 1);
        failures += !run(s, 1 + s % 64, 1 + (s * 37) % 1200, (int)(s & 1));
    }
    failures += !run(7, 64, 1900, 1);   /* most of RAM moves */
    printf("glazeboot-plan: %s (%d failures, %u seeds)\n", failures ? "FAILED" : "ALL PASS", failures, seeds);
    free(ram);
    return failures != 0;
}
