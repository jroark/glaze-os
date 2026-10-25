/*
 * GlazeBoot's hand-over plan, shared by the trampoline (tramp.c), the host
 * test (tests/glazeboot_plan.c), the ramboot SPL (boot/spl.c) and, through
 * the generated glazeboot_abi.h (mkabi.c), the Windows CE loader
 * (GlazeBoot.c).
 *
 * The loader reads the kernel and romfs into memory Windows CE gives it,
 * which is scattered over physical RAM, possibly over the very pages they
 * must end up in. Each 4 KiB page of content is a move {src, dst}: the
 * frame it is in now and the frame it belongs in. The trampoline puts every
 * page in place with gb_permute, swapping when a destination is still
 * occupied by content not yet placed, so no spare memory is needed.
 *
 * The trampoline sees only physical memory, so the plan lives in pages the
 * control page lists by frame: the moves, 512 to a page, and a map from
 * every frame to the move whose content is in it. None of the plan's own
 * frames, nor the trampoline's, may be a destination; the loader checks.
 */
#ifndef GLAZEBOOT_PLAN_H
#define GLAZEBOOT_PLAN_H

#include <stdint.h>

#define GB_PAGE             4096u
#define GB_FRAMES           4096u           /* 16 MiB of SDRAM */
#define GB_NONE             0xFFFFu
#define GB_MOVES_PER_PAGE   (GB_PAGE / sizeof(struct gb_move))
#define GB_MAX_MOVE_PAGES   8u
#define GB_MAX_MOVES        (GB_MAX_MOVE_PAGES * GB_MOVES_PER_PAGE)
#define GB_WHO_PER_PAGE     (GB_PAGE / sizeof(uint16_t))
#define GB_WHO_PAGES        (GB_FRAMES / GB_WHO_PER_PAGE)
#define GB_CONTROL_MAGIC    0x4c54425au     /* "ZBTL" */

struct gb_move {
    uint16_t src, dst;      /* frames */
    uint16_t where;         /* where the content is now */
    uint16_t pad;
};

/* One page, at the frame the trampoline is given. */
struct gb_control {
    uint32_t magic;
    uint32_t count;                         /* moves */
    uint32_t entry;                         /* kernel entry, KSEG0 */
    uint32_t arg;                           /* its a0: the boot info */
    uint32_t move_frames[GB_MAX_MOVE_PAGES];
    uint32_t who_frames[GB_WHO_PAGES];
};

/*
 * Page access for gb_permute: GB_PAGE_PTR(frame) is a pointer to the frame's
 * 4 KiB (KSEG0 in the trampoline, an array in the host test). Everything is
 * inline so the trampoline makes no calls and no absolute references.
 */
#ifndef GB_PAGE_PTR
#define GB_PAGE_PTR(frame)  ((uint32_t *)(0x80000000u | (uint32_t)(frame) << 12))
#endif

static inline __attribute__((always_inline))
struct gb_move *gb_move_at(const struct gb_control *c, uint32_t i)
{
    return (struct gb_move *)GB_PAGE_PTR(c->move_frames[i / GB_MOVES_PER_PAGE]) + i % GB_MOVES_PER_PAGE;
}

static inline __attribute__((always_inline))
uint16_t *gb_who_at(const struct gb_control *c, uint32_t frame)
{
    return (uint16_t *)GB_PAGE_PTR(c->who_frames[frame / GB_WHO_PER_PAGE]) + frame % GB_WHO_PER_PAGE;
}

static inline __attribute__((always_inline))
void gb_copy_page(uint32_t to, uint32_t from)
{
    volatile uint32_t *d = GB_PAGE_PTR(to);
    const volatile uint32_t *s = GB_PAGE_PTR(from);

    for (uint32_t i = 0; i < GB_PAGE / 4; i++)
        d[i] = s[i];
}

static inline __attribute__((always_inline))
void gb_swap_pages(uint32_t a, uint32_t b)
{
    volatile uint32_t *x = GB_PAGE_PTR(a), *y = GB_PAGE_PTR(b);

    for (uint32_t i = 0; i < GB_PAGE / 4; i++) {
        uint32_t t = x[i];
        x[i] = y[i];
        y[i] = t;
    }
}

/*
 * Puts every move's content in its destination frame. Move i is placed in
 * step i, and no later step touches a placed frame: it only swaps the frame
 * a later move's content is in with that move's destination, and placed
 * frames hold placed content and are no one else's destination.
 */
static inline __attribute__((always_inline))
void gb_permute(const struct gb_control *c)
{
    for (uint32_t f = 0; f < GB_FRAMES; f++)
        *gb_who_at(c, f) = GB_NONE;
    for (uint32_t i = 0; i < c->count; i++) {
        struct gb_move *m = gb_move_at(c, i);
        m->where = m->src;
        *gb_who_at(c, m->src) = (uint16_t)i;
    }
    for (uint32_t i = 0; i < c->count; i++) {
        struct gb_move *m = gb_move_at(c, i);
        uint32_t from = m->where, to = m->dst;
        if (from == to)
            continue;
        uint16_t other = *gb_who_at(c, to);
        if (other != GB_NONE) {
            gb_swap_pages(from, to);
            gb_move_at(c, other)->where = (uint16_t)from;
            *gb_who_at(c, from) = other;
        } else {
            gb_copy_page(to, from);
            *gb_who_at(c, from) = GB_NONE;
        }
        m->where = (uint16_t)to;
        *gb_who_at(c, to) = (uint16_t)i;
    }
}

#endif
