/*
 * Shared memory objects: refcounted sets of frames mappable by id.
 *
 * Frame i has cache colour i & 1 (physical bit 12), and as_map_frames maps
 * the first frame at a VA with bit 12 clear, so every process's mapping and
 * the kernel's KSEG0 alias index the same lines of the VR4131's virtually
 * indexed D-cache (VR4131 UM 1.6.3: two ways of 8 KiB). Frames of any colour
 * let one process read stale lines another had written: the network and
 * file servers' session buffers corrupted data on a real BE-300, which the
 * emulator, modelling no cache, never showed.
 */

#include "kernel.h"
#include "hw.h"
#include "proc.h"

#define SHM_MAX_PAGES   128     /* 512 KiB: enough for a full-screen surface pair */

struct shm {
    int refs;
    uint32_t npages;
    uint32_t frames[SHM_MAX_PAGES];
};

static struct shm shms[MAX_SHM];

int shm_create(uint32_t len)
{
    uint32_t npages = ALIGN_UP(len, PAGE_SIZE) / PAGE_SIZE;

    if (!npages || npages > SHM_MAX_PAGES)
        return GZ_EINVAL;
    for (int id = 0; id < MAX_SHM; id++) {
        struct shm *s = &shms[id];
        if (s->refs)
            continue;
        for (uint32_t i = 0; i < npages; i++) {
            void *page = page_alloc_colour_raw(i * PAGE_SIZE);
            if (!page) {
                while (i--)
                    page_free((void *)KSEG0(s->frames[i]));
                return GZ_ENOMEM;
            }
            s->frames[i] = PHYS(page);
        }
        s->npages = npages;
        /* The creator holds no mapping yet; the reference is dropped when
         * the first mapping is torn down. */
        s->refs = 1;
        return id + 1;
    }
    return GZ_EFULL;
}

static struct shm *shm_get(int id)
{
    if (id < 1 || id > MAX_SHM || !shms[id - 1].refs)
        return NULL;
    return &shms[id - 1];
}

uint32_t shm_map(struct addrspace *as, int id)
{
    struct shm *s = shm_get(id);

    if (!s)
        return 0;
    uint32_t va = as_map_frames(as, s->frames, s->npages, id);
    if (va)
        s->refs++;
    return va;
}

void shm_ref(int id)
{
    struct shm *s = shm_get(id);

    if (s)
        s->refs++;
}

void shm_unref(int id)
{
    struct shm *s = shm_get(id);

    if (!s)
        return;
    /* refs counts mappings plus the creation reference; when only the
     * creation reference remains, nobody can reach the frames anymore. */
    if (--s->refs <= 1) {
        for (uint32_t i = 0; i < s->npages; i++)
            page_free((void *)KSEG0(s->frames[i]));
        s->refs = 0;
        s->npages = 0;
    }
}
