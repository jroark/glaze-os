/*
 * Physical page allocator and per-process address spaces.
 *
 * Page tables are two-level (1024 x 1024 x 4 KiB). Each PGD slot points at a
 * page table, or at the shared all-invalid table, so the refill handler can
 * walk without branching. PTEs are stored directly in VR41xx EntryLo format
 * (see mips.h).
 *
 * TODO(real hardware): the VR4131 D-cache is virtually indexed with an 8 KiB
 * way, so KSEG0 and user aliases of a page can land in different cache
 * colours. The emulator does not model cache contents; real hardware will
 * need colour-matched allocation or explicit writeback around copies.
 */

#include "kernel.h"
#include "hw.h"
#include "mips.h"
#include "proc.h"

extern uint8_t __kernel_end[];

#define NPAGES  (SDRAM_SIZE / PAGE_SIZE)

static uint8_t page_used[NPAGES];
static uint32_t free_count, total_count;
static uint32_t alloc_hint;

pte_t **cur_pgd;
static pte_t *empty_pt;

void mm_init(void)
{
    uint32_t first = ALIGN_UP(PHYS(__kernel_end), PAGE_SIZE) / PAGE_SIZE;

    for (uint32_t i = 0; i < NPAGES; i++)
        page_used[i] = i < first;
    /* Low page 0 holds the exception vectors; already below `first`. */
    total_count = free_count = NPAGES - first;
    alloc_hint = first;

    empty_pt = page_alloc();
    KASSERT(empty_pt);
    kprintf("mm: kernel ends at 0x%08x, %u KiB free\n",
            (uint32_t)__kernel_end, free_count * 4);
}

void *page_alloc(void)
{
    for (uint32_t n = 0; n < NPAGES; n++) {
        uint32_t i = (alloc_hint + n) % NPAGES;
        if (!page_used[i]) {
            page_used[i] = 1;
            free_count--;
            alloc_hint = i + 1;
            void *kva = (void *)KSEG0(i * PAGE_SIZE);
            memset(kva, 0, PAGE_SIZE);
            return kva;
        }
    }
    return NULL;
}

/* n physically contiguous zeroed pages (kernel stacks). */
void *page_alloc_n(uint32_t n)
{
    for (uint32_t i = alloc_hint; i + n <= NPAGES; i++) {
        uint32_t j = 0;
        while (j < n && !page_used[i + j])
            j++;
        if (j < n) {
            i += j;
            continue;
        }
        for (j = 0; j < n; j++)
            page_used[i + j] = 1;
        free_count -= n;
        void *kva = (void *)KSEG0(i * PAGE_SIZE);
        memset(kva, 0, n * PAGE_SIZE);
        return kva;
    }
    if (alloc_hint) {
        uint32_t saved = alloc_hint;
        alloc_hint = 0;
        void *kva = page_alloc_n(n);
        if (!kva)
            alloc_hint = saved;
        return kva;
    }
    return NULL;
}

void page_free(void *kva)
{
    uint32_t i = PHYS(kva) / PAGE_SIZE;

    KASSERT(i < NPAGES && page_used[i]);
    page_used[i] = 0;
    free_count++;
}

uint32_t mm_free_pages(void) { return free_count; }
uint32_t mm_total_pages(void) { return total_count; }

int as_create(struct addrspace *as, uint8_t asid)
{
    memset(as, 0, sizeof(*as));
    as->pgd = page_alloc();
    if (!as->pgd)
        return GZ_ENOMEM;
    for (int i = 0; i < 1024; i++)
        as->pgd[i] = empty_pt;
    as->asid = asid;
    as->mmap_top = USER_MMAP_BASE;
    return 0;
}

static pte_t *pte_slot(struct addrspace *as, uint32_t va, bool create)
{
    pte_t *pt = as->pgd[va >> 22];

    if (pt == empty_pt) {
        if (!create)
            return NULL;
        pt = page_alloc();
        if (!pt)
            return NULL;
        as->pgd[va >> 22] = pt;
    }
    return &pt[(va >> PAGE_SHIFT) & 0x3FF];
}

int as_map_page(struct addrspace *as, uint32_t va, uint32_t pa, uint32_t flags)
{
    pte_t *slot = pte_slot(as, va, true);

    if (!slot)
        return GZ_ENOMEM;
    *slot = PTE_MAKE(pa) | flags;
    return 0;
}

pte_t as_lookup(struct addrspace *as, uint32_t va)
{
    if (va >= USER_TOP)
        return 0;
    pte_t *slot = pte_slot(as, va, false);
    return slot ? *slot : 0;
}

static struct region *region_add(struct addrspace *as, uint32_t va, uint32_t npages, int kind, int shm_id)
{
    for (int i = 0; i < MAX_REGIONS; i++) {
        struct region *r = &as->regions[i];
        if (!r->kind) {
            r->va = va;
            r->npages = npages;
            r->kind = kind;
            r->shm_id = shm_id;
            return r;
        }
    }
    return NULL;
}

/* Map fresh zeroed, owned pages over [va, va+len). Already-mapped pages are
 * left alone (ELF segments may share a page). */
int as_alloc_region(struct addrspace *as, uint32_t va, uint32_t len, int kind)
{
    uint32_t start = va & ~(PAGE_SIZE - 1);
    uint32_t end = ALIGN_UP(va + len, PAGE_SIZE);
    uint32_t added = 0;

    for (uint32_t a = start; a < end; a += PAGE_SIZE) {
        if (as_lookup(as, a) & PTE_V)
            continue;
        void *page = page_alloc();
        if (!page || as_map_page(as, a, PHYS(page), PTE_CACHED | PTE_D | PTE_V)) {
            if (page)
                page_free(page);
            return GZ_ENOMEM;
        }
        added++;
    }
    if (added && !region_add(as, start, (end - start) / PAGE_SIZE, kind, -1))
        return GZ_ENOMEM;
    return 0;
}

uint32_t as_mmap_anon(struct addrspace *as, uint32_t len)
{
    uint32_t va = as->mmap_top;

    len = ALIGN_UP(len, PAGE_SIZE);
    if (!len || va + len >= USER_STACK_TOP - USER_STACK_SIZE)
        return 0;
    if (as_alloc_region(as, va, len, REGION_OWNED))
        return 0;
    as->mmap_top += len + PAGE_SIZE;    /* unmapped guard page */
    return va;
}

uint32_t as_map_phys(struct addrspace *as, uint32_t pa, uint32_t len, uint32_t cache)
{
    uint32_t va = as->mmap_top;

    len = ALIGN_UP(len, PAGE_SIZE);
    for (uint32_t off = 0; off < len; off += PAGE_SIZE) {
        if (as_map_page(as, va + off, pa + off, cache | PTE_D | PTE_V))
            return 0;
    }
    if (!region_add(as, va, len / PAGE_SIZE, REGION_DEVICE, -1))
        return 0;
    as->mmap_top += len + PAGE_SIZE;
    return va;
}

/* Used by shm.c to map an existing set of frames. */
uint32_t as_map_frames(struct addrspace *as, const uint32_t *frames, uint32_t npages, int shm_id)
{
    uint32_t va = as->mmap_top;

    for (uint32_t i = 0; i < npages; i++) {
        if (as_map_page(as, va + i * PAGE_SIZE, frames[i], PTE_CACHED | PTE_D | PTE_V))
            return 0;
    }
    if (!region_add(as, va, npages, REGION_SHM, shm_id))
        return 0;
    as->mmap_top += npages * PAGE_SIZE + PAGE_SIZE;
    return va;
}

void as_destroy(struct addrspace *as)
{
    for (int i = 0; i < MAX_REGIONS; i++) {
        struct region *r = &as->regions[i];
        if (r->kind == REGION_OWNED) {
            for (uint32_t p = 0; p < r->npages; p++) {
                pte_t pte = as_lookup(as, r->va + p * PAGE_SIZE);
                if (pte & PTE_V)
                    page_free((void *)KSEG0(PTE_PA(pte)));
            }
        } else if (r->kind == REGION_SHM) {
            shm_unref(r->shm_id);
        }
        r->kind = 0;
    }
    for (int i = 0; i < 1024; i++) {
        if (as->pgd[i] != empty_pt)
            page_free(as->pgd[i]);
    }
    page_free(as->pgd);
    as->pgd = NULL;
}

void as_activate(struct addrspace *as)
{
    cur_pgd = as->pgd;
    write_c0_entryhi(as->asid);
}

/* Resolve a user VA to a writable/readable KSEG0 pointer, or NULL. */
static uint8_t *uva_to_kva(struct addrspace *as, uint32_t uva, bool write)
{
    pte_t pte = as_lookup(as, uva);

    if (!(pte & PTE_V) || (write && !(pte & PTE_D)))
        return NULL;
    uint32_t pa = PTE_PA(pte) + (uva & (PAGE_SIZE - 1));
    /* Device mappings (the framebuffer) are reached uncached. */
    if ((pte & (7u << 3)) == PTE_UNCACHED)
        return (uint8_t *)KSEG1(pa);
    return (uint8_t *)KSEG0(pa);
}

int copy_to_as(struct addrspace *as, uint32_t uva, const void *src, uint32_t len)
{
    const uint8_t *s = src;

    if (uva + len < uva || uva + len > USER_TOP)
        return GZ_EINVAL;
    while (len) {
        uint8_t *k = uva_to_kva(as, uva, true);
        if (!k)
            return GZ_EINVAL;
        uint32_t chunk = MIN(len, PAGE_SIZE - (uva & (PAGE_SIZE - 1)));
        memcpy(k, s, chunk);
        s += chunk;
        uva += chunk;
        len -= chunk;
    }
    return 0;
}

int copy_from_as(struct addrspace *as, void *dst, uint32_t uva, uint32_t len)
{
    uint8_t *d = dst;

    if (uva + len < uva || uva + len > USER_TOP)
        return GZ_EINVAL;
    while (len) {
        const uint8_t *k = uva_to_kva(as, uva, false);
        if (!k)
            return GZ_EINVAL;
        uint32_t chunk = MIN(len, PAGE_SIZE - (uva & (PAGE_SIZE - 1)));
        memcpy(d, k, chunk);
        d += chunk;
        uva += chunk;
        len -= chunk;
    }
    return 0;
}

int copy_str_from_as(struct addrspace *as, char *dst, uint32_t uva, uint32_t max)
{
    for (uint32_t i = 0; i < max; i++) {
        const uint8_t *k = uva_to_kva(as, uva + i, false);
        if (!k)
            return GZ_EINVAL;
        dst[i] = (char)*k;
        if (!*k)
            return (int)i;
    }
    return GZ_EINVAL;
}
