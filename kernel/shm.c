/* Shared memory objects: refcounted sets of frames mappable by id. */

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
            void *page = page_alloc();
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
