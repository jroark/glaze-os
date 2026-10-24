/* Immutable ELF snapshots and process-private instances. The kernel maps pages;
 * the userspace interpreter owns symbol lookup and relocation policy.
 *
 * An image comes from romfs (rom) or from bytes copied into frames[]. A romfs
 * image's shared read-only pages are left unmapped and faulted in from the
 * romfs page cache (image_fault), so only the pages a process touches are
 * read from NAND; its private pages are copied when it is mapped. */
#include "kernel.h"
#include "proc.h"
#include "mips.h"
#include "hw.h"

struct image {
    struct image *next;
    uint32_t refs, size, hash, lo, hi, dynamic, colour;
    const struct romfs_entry *rom;
    struct gz_elf_ehdr eh;
    struct gz_elf_phdr ph[GZ_ELF_PHMAX];
    uint32_t frames[GZ_ELF_MAX_SIZE / PAGE_SIZE];
};
_Static_assert(sizeof(struct image) <= PAGE_SIZE, "image descriptor fits one page");
static struct image *images;

/* NULL if a romfs page can't be read in (out of memory). */
static const uint8_t *file_ptr(struct image *im, uint32_t off)
{
    return im->rom ? romfs_ptr(im->rom, off) : (const uint8_t *)KSEG0(im->frames[off / PAGE_SIZE]) + off % PAGE_SIZE;
}
static int file_read(struct image *im, uint32_t off, void *dst, uint32_t len)
{
    if (im->rom)
        return romfs_read(im->rom, off, dst, len);
    while (len) {
        uint32_t n = MIN(len, PAGE_SIZE - off % PAGE_SIZE);
        memcpy(dst, file_ptr(im, off), n);
        off += n; dst = (uint8_t *)dst + n; len -= n;
    }
    return 0;
}
/* Where a file offset's page sits, for its cache colour (bit 12): the frame,
 * or the romfs archive offset, which is a romfs cache frame's colour. */
static uint32_t file_addr(struct image *im, uint32_t off)
{ return im->rom ? im->rom->offset + off : im->frames[off / PAGE_SIZE]; }
static void image_unref(struct image *im)
{
    if (--im->refs) return;
    struct image **link = &images;
    while (*link != im) link = &(*link)->next;
    *link = im->next;
    if (!im->rom) for (uint32_t i = 0; i < ALIGN_UP(im->size, PAGE_SIZE) / PAGE_SIZE; i++) page_free((void *)KSEG0(im->frames[i]));
    page_free(im);
}
static int validate(struct image *im)
{
    if (file_read(im, 0, &im->eh, sizeof(im->eh)) || !gz_elf_header(&im->eh, im->size)) return GZ_EINVAL;
    if (file_read(im, im->eh.phoff, im->ph, im->eh.phnum * sizeof(im->ph[0]))) return GZ_ENOMEM;
    im->lo = UINT32_MAX; im->hi = 0;
    bool executable_entry = false, colour_set = false;
    for (unsigned i = 0; i < im->eh.phnum; i++) {
        struct gz_elf_phdr *p = &im->ph[i];
        if (!gz_elf_segment(p, im->size) ||
            (im->eh.type == GZ_ET_DYN && p->type == GZ_PT_LOAD && (p->flags & (GZ_PF_W | GZ_PF_X)) == (GZ_PF_W | GZ_PF_X))) return GZ_EINVAL;
        if (p->type == 0x70000003u) { /* MIPS .abiflags: o32 soft-float, no ASEs */
            uint8_t abi[24];
            if (p->filesz != sizeof(abi) || !gz_elf_range(p->offset, sizeof(abi), im->size)) return GZ_EINVAL;
            if (file_read(im, p->offset, abi, sizeof(abi))) return GZ_ENOMEM;
            if (abi[0] || abi[1] || abi[2] > 3 || abi[4] != 1 || abi[5] || abi[6] || abi[7] != 3 ||
                abi[8] || abi[9] || abi[10] || abi[11] || abi[12] || abi[13] || abi[14] || abi[15]) return GZ_EINVAL;
        }
        if (p->type == GZ_PT_DYNAMIC) {
            if (im->dynamic || !p->filesz || p->filesz % sizeof(struct gz_elf_dyn)) return GZ_EINVAL;
            im->dynamic = p->vaddr;
        }
        if (p->type != GZ_PT_LOAD || !p->memsz) continue;
        uint32_t lo = p->vaddr & ~(PAGE_SIZE - 1), hi = ALIGN_UP(p->vaddr + p->memsz, PAGE_SIZE);
        if (hi > USER_TOP || hi < lo) return GZ_EINVAL;
        for (unsigned j = 0; j < i; j++) {
            struct gz_elf_phdr *q = &im->ph[j];
            if (q->type == GZ_PT_LOAD && q->memsz && lo < ALIGN_UP(q->vaddr + q->memsz, PAGE_SIZE) &&
                hi > (q->vaddr & ~(PAGE_SIZE - 1))) return GZ_EINVAL;
        }
        im->lo = MIN(im->lo, lo); im->hi = MAX(im->hi, hi);
        if ((p->flags & GZ_PF_X) && im->eh.entry >= p->vaddr && im->eh.entry - p->vaddr < p->memsz) executable_entry = true;
        if (!colour_set && !(p->flags & GZ_PF_W) && p->filesz) {
            im->colour = (file_addr(im, p->offset & ~(PAGE_SIZE - 1)) - lo) & 0x1000u;
            colour_set = true;
        }
    }
    if (im->lo == UINT32_MAX || im->hi - im->lo > 0x04000000u ||
        (im->eh.type == GZ_ET_EXEC && !executable_entry)) return GZ_EINVAL;
    return 0;
}

/* Input remains stable throughout a syscall: the kernel is non-preemptive.
 * A romfs image is shared by its entry, without reading it; byte images are
 * shared by content. */
static struct image *image_get(struct addrspace *as, const struct romfs_entry *rom, uint32_t src, uint32_t size)
{
    if (size < sizeof(struct gz_elf_ehdr) || size > GZ_ELF_MAX_SIZE) return NULL;
    uint8_t buf[128]; uint32_t hash = 2166136261u;
    if (rom) {
        for (struct image *im = images; im; im = im->next) if (im->rom == rom) { im->refs++; return im; }
    } else {
        for (uint32_t off = 0; off < size;) {
            uint32_t n = MIN(size - off, sizeof(buf));
            if (copy_from_as(as, buf, src + off, n)) return NULL;
            for (uint32_t i = 0; i < n; i++) hash = (hash ^ buf[i]) * 16777619u;
            off += n;
        }
        for (struct image *im = images; im; im = im->next) {
            if (im->rom || im->size != size || im->hash != hash) continue;
            bool equal = true;
            for (uint32_t off = 0; off < size && equal;) {
                uint32_t n = MIN(MIN(size - off, sizeof(buf)), PAGE_SIZE - off % PAGE_SIZE);
                if (copy_from_as(as, buf, src + off, n)) return NULL;
                equal = !memcmp(buf, file_ptr(im, off), n); off += n;
            }
            if (equal) { im->refs++; return im; }
        }
    }
    struct image *im = page_alloc();
    if (!im) return NULL;
    im->rom = rom; im->size = size; im->hash = hash; im->refs = 1;
    if (!rom) {
        uint32_t count = ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE;
        for (uint32_t i = 0; i < count; i++) {
            void *page = page_alloc_colour_raw(i * PAGE_SIZE);
            if (!page) {
                for (uint32_t j = 0; j < i; j++) page_free((void *)KSEG0(im->frames[j]));
                page_free(im); return NULL;
            }
            im->frames[i] = PHYS(page);
            uint32_t n = MIN(PAGE_SIZE, size - i * PAGE_SIZE);
            if (copy_from_as(as, page, src + i * PAGE_SIZE, n)) {
                for (uint32_t j = 0; j <= i; j++) page_free((void *)KSEG0(im->frames[j]));
                page_free(im); return NULL;
            }
            memset((uint8_t *)page + n, 0, PAGE_SIZE - n);
            mm_sync_code(page, PAGE_SIZE);
        }
    }
    im->next = images; images = im;
    if (validate(im)) { image_unref(im); return NULL; }
    return im;
}

static bool available(struct addrspace *as, uint32_t base, uint32_t end)
{
    for (struct region_page *p = as->regions; p; p = p->next)
        for (unsigned i = 0; i < ARRAY_SIZE(p->entry); i++) {
            struct region *r = &p->entry[i];
            if (r->kind && base < r->va + r->npages * PAGE_SIZE && end > r->va) return false;
        }
    return true;
}
/* The file offset + 1 of a page mapped read-only straight from the file, or 0
 * when it gets a private copy (writable, partly bss, or the wrong colour). */
static uint32_t shared_off(struct image *im, uint32_t bias, uint32_t va)
{
    uint32_t relative = va - bias;
    for (unsigned i = 0; i < im->eh.phnum; i++) {
        struct gz_elf_phdr *p = &im->ph[i];
        uint32_t lo = p->vaddr & ~(PAGE_SIZE - 1);
        if (p->type != GZ_PT_LOAD || p->flags & GZ_PF_W || p->filesz != p->memsz || relative < lo ||
            relative - lo >= ALIGN_UP(p->vaddr + p->memsz, PAGE_SIZE) - lo) continue;
        uint32_t off = (p->offset & ~(PAGE_SIZE - 1)) + relative - lo;
        if (off >= im->size) return 0;
        if (!((file_addr(im, off) ^ va) & 0x1000u)) return off + 1;
    }
    return 0;
}
/* A shared page's frame: the byte image's own, or the romfs cache frame (read
 * in when load is set). 0 if it isn't shared, or isn't in the cache. */
static uint32_t shared_pa(struct image *im, uint32_t bias, uint32_t va, bool load)
{
    uint32_t so = shared_off(im, bias, va);
    if (!so) return 0;
    if (!im->rom) return im->frames[(so - 1) / PAGE_SIZE];
    return load ? romfs_frame(im->rom, so - 1) : romfs_cached_frame(im->rom, so - 1);
}
static int map_instance(struct addrspace *as, struct image *im, struct gz_image_info *info, uint32_t at)
{
    uint32_t bias = 0, len = im->hi - im->lo;
    if (im->eh.type == GZ_ET_DYN) {
        uint32_t base = at ? at : GZ_DSO_BASE;
        base = ALIGN_UP(base, 8192u) + ((im->lo + im->colour) & 0x1000u);
        while (!available(as, base, base + len + PAGE_SIZE)) {
            if (at || base > GZ_DSO_END - len - 8192u) { image_unref(im); return GZ_ENOMEM; }
            base += 8192u;
        }
        if (!at && base + len + PAGE_SIZE > GZ_DSO_END) { image_unref(im); return GZ_ENOMEM; }
        bias = base - im->lo;
    } else if (im->lo < USER_TEXT_BASE || im->hi >= USER_MMAP_BASE || !available(as, im->lo, im->hi)) {
        image_unref(im); return GZ_EINVAL;
    }
    struct region *r = as_region_add(as, bias + im->lo, len / PAGE_SIZE, REGION_IMAGE, (int)(uintptr_t)im);
    if (!r) { image_unref(im); return GZ_ENOMEM; }
    for (unsigned i = 0; i < im->eh.phnum; i++) {
        struct gz_elf_phdr *p = &im->ph[i];
        if (p->type != GZ_PT_LOAD || !p->memsz) continue;
        uint32_t lo = p->vaddr & ~(PAGE_SIZE - 1), hi = ALIGN_UP(p->vaddr + p->memsz, PAGE_SIZE);
        for (uint32_t a = lo; a < hi; a += PAGE_SIZE) {
            if (im->rom && shared_off(im, bias, bias + a))
                continue;               /* faulted in from the romfs cache (image_fault) */
            uint32_t pa = shared_pa(im, bias, bias + a, false);
            bool own = !pa;
            void *page = NULL;
            if (own) {
                page = page_alloc_colour(bias + a);
                if (!page) goto fail;
                uint32_t start = MAX(a, p->vaddr), end = MIN(a + PAGE_SIZE, p->vaddr + p->filesz);
                if (end > start &&
                    file_read(im, p->offset + start - p->vaddr, (uint8_t *)page + start - a, end - start)) {
                    page_free(page);
                    goto fail;
                }
                pa = PHYS(page);
            }
            if (as_map_page(as, bias + a, pa, PTE_CACHED | PTE_V | ((p->flags & GZ_PF_W) ? PTE_D : 0))) {
                if (own) page_free(page);
                goto fail;
            }
            if (p->flags & GZ_PF_X) mm_sync_code((void *)KSEG0(pa), PAGE_SIZE);
        }
    }
    memset(info, 0, sizeof(*info));
    info->base = r->va; info->end = r->va + len; info->bias = bias; info->entry = bias + im->eh.entry;
    info->dynamic = im->dynamic ? bias + im->dynamic : 0; info->phnum = im->eh.phnum;
    memcpy(info->ph, im->ph, im->eh.phnum * sizeof(im->ph[0]));
    return 0;
fail:
    image_release_region(as, r); as_region_drop(as, r); as_prune_tables(as);
    return GZ_ENOMEM;
}
/* A read-only shared page of a romfs image, on first touch. */
int image_fault(struct addrspace *as, struct region *r, uint32_t va)
{
    struct image *im = (void *)(uintptr_t)(uint32_t)r->shm_id;
    va &= ~(PAGE_SIZE - 1);
    uint32_t pa = im->rom ? shared_pa(im, r->va - im->lo, va, true) : 0;
    if (!pa) return GZ_EINVAL;
    int rv = as_map_page(as, va, pa, PTE_CACHED | PTE_V);
    if (!rv) romfs_map_ref(pa);
    return rv;
}
/* A program for a new process: from romfs, or from a buffer in the spawner's
 * address space (SYS_SPAWN_IMAGE). Its PT_INTERP, if any, is copied out. */
int image_map_exec(struct addrspace *as, struct addrspace *src_as, const struct romfs_entry *rom, uint32_t src, uint32_t size,
                   struct gz_image_info *info, uint32_t *type, char *interp, uint32_t max)
{
    struct image *im = image_get(src_as, rom, src, size);
    if (!im) return GZ_EINVAL;
    *type = im->eh.type;
    interp[0] = 0;
    for (unsigned i = 0; i < im->eh.phnum; i++) {
        struct gz_elf_phdr *p = &im->ph[i];
        if (p->type != GZ_PT_INTERP) continue;
        if (interp[0] || !p->filesz || p->filesz > max || !gz_elf_range(p->offset, p->filesz, im->size)) {
            image_unref(im); return GZ_EINVAL;
        }
        if (file_read(im, p->offset, interp, p->filesz)) { image_unref(im); return GZ_ENOMEM; }
        if (interp[p->filesz - 1] || !interp[0]) { image_unref(im); return GZ_EINVAL; }
    }
    return map_instance(as, im, info, USER_TEXT_BASE);
}
int image_map(struct addrspace *as, const struct romfs_entry *rom, uint32_t size, struct gz_image_info *info, uint32_t at)
{
    struct image *im = image_get(as, rom, 0, size);
    return im ? map_instance(as, im, info, at) : GZ_EINVAL;
}
int image_map_user(struct addrspace *as, uint32_t src, uint32_t size, uint32_t flags, uint32_t out)
{
    const struct romfs_entry *rom = NULL;
    if (flags == GZ_IMAGE_ROMFS) {
        char path[48];
        if (copy_str_from_as(as, path, src, sizeof(path)) < 0) return GZ_EINVAL;
        rom = romfs_lookup(path);
        if (!rom) return GZ_ENOENT;
        size = rom->size;
    } else if (flags != GZ_IMAGE_BYTES || !gz_elf_range(src, size, USER_TOP)) return GZ_EINVAL;
    struct image *im = image_get(as, rom, src, size);
    if (!im) return GZ_EINVAL;
    if (im->eh.type != GZ_ET_DYN) { image_unref(im); return GZ_EINVAL; }
    for (unsigned i = 0; i < im->eh.phnum; i++)
        if (im->ph[i].type == GZ_PT_INTERP) { image_unref(im); return GZ_EINVAL; }
    struct gz_image_info info;
    int rv = map_instance(as, im, &info, 0);
    if (!rv && copy_to_as(as, out, &info, sizeof(info))) { image_release(as, info.base); return GZ_EINVAL; }
    return rv;
}
void image_release_region(struct addrspace *as, struct region *r)
{
    struct image *im = (void *)(uintptr_t)(uint32_t)r->shm_id;
    uint32_t bias = r->va - im->lo;
    for (uint32_t a = r->va; a < r->va + r->npages * PAGE_SIZE; a += PAGE_SIZE) {
        pte_t pte = as_lookup(as, a);
        if (!(pte & PTE_V)) continue;
        bool shared = PTE_PA(pte) == shared_pa(im, bias, a, false);
        if (shared && im->rom) romfs_map_unref(PTE_PA(pte));
        as_unmap_page(as, a, !shared);
    }
    image_unref(im); r->kind = 0;
}
static struct region *find_mapping(struct addrspace *as, uint32_t base)
{
    for (struct region_page *p = as->regions; p; p = p->next)
        for (unsigned i = 0; i < ARRAY_SIZE(p->entry); i++)
            if (p->entry[i].kind == REGION_IMAGE && p->entry[i].va == base) return &p->entry[i];
    return NULL;
}
int image_release(struct addrspace *as, uint32_t base)
{
    struct region *r = find_mapping(as, base);
    if (!r || base < GZ_DSO_BASE) return GZ_EINVAL; /* startup images stay until exit */
    image_release_region(as, r); as_region_drop(as, r); as_prune_tables(as); return 0;
}
int image_seal(struct addrspace *as, uint32_t base)
{
    struct region *r = find_mapping(as, base);
    if (!r) return GZ_EINVAL;
    struct image *im = (void *)(uintptr_t)(uint32_t)r->shm_id;
    uint32_t bias = base - im->lo;
    for (unsigned i = 0; i < im->eh.phnum; i++) {
        struct gz_elf_phdr *p = &im->ph[i];
        if (p->type != GZ_PT_GNU_RELRO) continue;
        if (p->vaddr < im->lo || p->vaddr > im->hi || p->memsz > im->hi - p->vaddr) return GZ_EINVAL;
        uint32_t start = (bias + p->vaddr) & ~(PAGE_SIZE - 1), end = (bias + p->vaddr + p->memsz) & ~(PAGE_SIZE - 1);
        for (uint32_t a = start; a < end; a += PAGE_SIZE) {
            pte_t pte = as_lookup(as, a);
            if (!(pte & PTE_V)) return GZ_EINVAL;
            as_map_page(as, a, PTE_PA(pte), (pte & 0x3fu) & ~PTE_D);
        }
    }
    tlb_flush_all(); return 0;
}
int image_stats(struct addrspace *as, uint32_t out)
{
    struct gz_image_stats stats = {0};
    for (struct region_page *p = as->regions; p; p = p->next)
        for (unsigned i = 0; i < ARRAY_SIZE(p->entry); i++) {
            struct region *r = &p->entry[i];
            if (r->kind != REGION_IMAGE) continue;
            stats.mappings++;
            struct image *im = (void *)(uintptr_t)(uint32_t)r->shm_id;
            for (uint32_t a = r->va; a < r->va + r->npages * PAGE_SIZE; a += PAGE_SIZE) {
                pte_t pte = as_lookup(as, a);
                if (!(pte & PTE_V)) continue;
                if (PTE_PA(pte) == shared_pa(im, r->va - im->lo, a, false)) stats.shared_pages++;
                else stats.private_pages++;
            }
        }
    return copy_to_as(as, out, &stats, sizeof(stats));
}

/* Inspect only this process's image mappings, for library/memory diagnostics. */
int image_query(struct addrspace *as, uint32_t address, uint32_t out)
{
    for (struct region_page *p = as->regions; p; p = p->next)
        for (unsigned i = 0; i < ARRAY_SIZE(p->entry); i++) {
            struct region *r = &p->entry[i];
            if (r->kind != REGION_IMAGE || address < r->va || address - r->va >= r->npages * PAGE_SIZE) continue;
            pte_t pte = as_lookup(as, address);
            if (!(pte & PTE_V) && !as_fault(as, address, false)) pte = as_lookup(as, address);  /* not touched yet */
            if (!(pte & PTE_V)) return GZ_EINVAL;
            struct image *im = (void *)(uintptr_t)(uint32_t)r->shm_id;
            struct gz_image_page page = { PTE_PA(pte), im->refs, (pte & PTE_D) != 0 };
            return copy_to_as(as, out, &page, sizeof(page));
        }
    return GZ_EINVAL;
}
