/* kernel/hwfile.c natively: manifests signed by tools/mkhwpkg.py with a test
 * key (tests/hwfile_test.py), the fetch protocol a file server drives, digest
 * and signature failures, and page accounting. Userspace addresses are
 * offsets into one buffer standing in for the file server's memory. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/kernel.h"
#include "../../kernel/hwfile.h"
#include <glaze/hwpkg.h>

static uint8_t user[1 << 20];
static unsigned pages, notified;
static int last_port;
static struct romfs_entry rom_ne2000 = { .size = 3 };

void kprintf(const char *format, ...) { (void)format; }
const struct romfs_entry *romfs_lookup(const char *name)
{ return !strcmp(name, "drivers/ne2000.ko") ? &rom_ne2000 : NULL; }
int romfs_read(const struct romfs_entry *e, uint32_t off, void *p, uint32_t n)
{ assert(e == &rom_ne2000 && off + n <= 3); memcpy(p, &"ROM"[off], n); return 0; }
/* Blocks from page_alloc_n, which hwfile.c frees a page at a time. */
static struct { uint8_t *base; unsigned count, left; } blocks[16];
void *page_alloc_n(uint32_t n)
{
    for (unsigned i = 0; i < 16; i++)
        if (!blocks[i].base) {
            blocks[i].base = calloc(n, PAGE_SIZE); assert(blocks[i].base);
            blocks[i].count = blocks[i].left = n;
            pages += n;
            return blocks[i].base;
        }
    abort();
}
void page_free(void *p)
{
    for (unsigned i = 0; i < 16; i++)
        if (blocks[i].base && (uint8_t *)p >= blocks[i].base &&
            (uint8_t *)p < blocks[i].base + blocks[i].count * PAGE_SIZE) {
            assert(pages && blocks[i].left);
            pages--;
            if (!--blocks[i].left) { free(blocks[i].base); blocks[i].base = NULL; }
            return;
        }
    abort();
}
int ipc_notify(int port, uint32_t bits) { last_port = port; notified += !!(bits & GZ_NOTIFY_HW); return 0; }
int copy_from_as(struct addrspace *as, void *dst, uint32_t uva, uint32_t len)
{ (void)as; if (uva + len > sizeof(user)) return GZ_EINVAL; memcpy(dst, user + uva, len); return 0; }
int copy_to_as(struct addrspace *as, uint32_t uva, const void *src, uint32_t len)
{ (void)as; if (uva + len > sizeof(user)) return GZ_EINVAL; memcpy(user + uva, src, len); return 0; }

struct pkg { uint8_t *data; size_t size; };
static struct pkg load(const char *path)
{
    FILE *f = fopen(path, "rb"); assert(f);
    struct pkg p;
    fseek(f, 0, SEEK_END); p.size = (size_t)ftell(f); rewind(f);
    p.data = malloc(p.size); assert(p.data && fread(p.data, 1, p.size, f) == p.size);
    fclose(f);
    return p;
}
/* An entry of a GZPK container (glaze/pkg.h). */
static const uint8_t *entry(const struct pkg *p, const char *name, uint32_t *size)
{
    uint32_t count; memcpy(&count, p->data + 4, 4);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = p->data + 8 + i * 56;
        if (!strcmp((const char *)e, name)) {
            uint32_t off; memcpy(&off, e + 48, 4); memcpy(size, e + 52, 4);
            return p->data + off;
        }
    }
    return NULL;
}

static struct proc fs = { .flags = PROC_FLASH }, app;
static int ctl(struct proc *p, uint32_t op, uint32_t a1, uint32_t a2) { return sys_hw_ctl(p, op, a1, a2); }
static int add(const uint8_t *m, uint32_t n) { memcpy(user, m, n); return ctl(&fs, GZ_HW_OP_ADD, 0, n); }
static int add_pkg(const struct pkg *p)
{
    uint32_t n; const uint8_t *m = entry(p, "Manifest", &n); assert(m);
    return add(m, n);
}
static void registered(const struct pkg *const *list, unsigned count, int expect_count)
{
    assert(!ctl(&fs, GZ_HW_OP_BEGIN, 7, 0));
    for (unsigned i = 0; i < count; i++) assert(!add_pkg(list[i]));
    assert(ctl(&fs, GZ_HW_OP_END, 0, 0) == expect_count);
}
/* The file server's side: FETCH, then SUPPLY in chunks of `chunk` bytes from
 * `pkg` (corrupting byte `flip` if it isn't -1). Returns the final SUPPLY. */
static int serve(const struct pkg *pkg, uint32_t chunk, long flip)
{
    assert(ctl(&fs, GZ_HW_OP_FETCH, 0x1000, 0) == 1);
    struct gz_hw_request r; memcpy(&r, user + 0x1000, sizeof(r));
    char name[80]; snprintf(name, sizeof(name), "hw/%s", r.path);
    uint32_t size; const uint8_t *data = entry(pkg, name, &size);
    assert(data && size == r.size);
    memcpy(user + 0x8000, data, size);
    if (flip >= 0) user[0x8000 + flip] ^= 1;
    int rv = 0;
    for (uint32_t done = 0; done < size; done += chunk) {
        uint32_t n = size - done < chunk ? size - done : chunk;
        rv = ctl(&fs, GZ_HW_OP_SUPPLY, 0x8000 + done, n);
        if (rv < 0) return rv;
    }
    return rv;
}

int main(int argc, char **argv)
{
    assert(argc == 7);
    FILE *k = fopen(argv[1], "rb"); assert(k && fread(hwfile_test_key, 1, 32, k) == 32); fclose(k);
    struct pkg good = load(argv[2]), fw = load(argv[3]), wrong = load(argv[4]), old = load(argv[5]),
               dup = load(argv[6]);
    struct hwfile f;

    /* Until the file server has registered, a missing file may yet come. */
    assert(hwfile_open("drivers/a.ko", &f) == GZ_EINPROGRESS);
    assert(!hwfile_open("drivers/ne2000.ko", &f) && f.rom == &rom_ne2000 && f.size == 3);
    char rom[3]; assert(!hwfile_read(&f, 0, rom, 3) && !memcmp(rom, "ROM", 3)); hwfile_close(&f);
    assert(ctl(&app, GZ_HW_OP_BEGIN, 7, 0) == GZ_EPERM);
    registered(NULL, 0, 0);
    assert(hwfile_open("drivers/a.ko", &f) == GZ_ENOENT);
    uint32_t gen = hwfile_generation();

    /* A signed package: fetched on demand, checked, then handed out. */
    const struct pkg *one[] = { &good };
    registered(one, 1, 2);
    assert(hwfile_generation() == gen + 1);
    assert(ctl(&fs, GZ_HW_OP_FETCH, 0x1000, 0) == 0);           /* nothing wanted yet */
    assert(hwfile_open("drivers/a.ko", &f) == GZ_EINPROGRESS && notified == 1 && last_port == 7 && pages == 1);
    assert(hwfile_open("bin/net", &f) == GZ_EINPROGRESS && pages == 1);   /* one at a time */
    assert(serve(&good, 1000, -1) == 1);
    assert(!hwfile_open("drivers/a.ko", &f) && !f.rom && f.buf && f.size == 4000);
    uint32_t size; const uint8_t *want = entry(&good, "hw/drivers/a.ko", &size);
    uint8_t got[4000]; assert(!hwfile_read(&f, 0, got, 4000) && !memcmp(got, want, 4000));
    assert(hwfile_read(&f, 3999, got, 2) == GZ_EINVAL);
    hwfile_trim(); assert(pages == 1);                          /* still open */
    hwfile_close(&f); hwfile_trim(); assert(!pages);
    /* Status for Settings, to anyone. */
    static struct gz_hw_status st;
    assert(!ctl(&app, GZ_HW_OP_STATUS, 0x2000, sizeof(st)));
    memcpy(&st, user + 0x2000, sizeof(st));
    assert(st.loaded && st.count == 2 && !strcmp(st.files[0].id, "hw-test") && st.files[0].state == GZ_HW_FILE_IDLE);
    assert(ctl(&app, GZ_HW_OP_STATUS, 0x2000, 4) == GZ_EINVAL);
    /* An overridable service: the package's copy, never romfs. */
    assert(hwfile_open_package("bin/net", &f) == GZ_EINPROGRESS);
    assert(serve(&good, 4096, -1) == 1);
    assert(!hwfile_open_package("bin/net", &f) && f.buf && f.size == 9000);
    hwfile_close(&f);
    assert(hwfile_open_package("drivers/ne2000.ko", &f) == GZ_ENOENT);
    hwfile_trim(); assert(!pages);

    /* Fetched for a consumer that went away (a card pulled mid-fetch): freed. */
    assert(hwfile_open("bin/net", &f) == GZ_EINPROGRESS);
    assert(serve(&good, 4096, -1) == 1 && pages);
    hwfile_trim(); assert(!pages);
    assert(hwfile_open("bin/net", &f) == GZ_EINPROGRESS);      /* fetched again on demand */
    assert(serve(&good, 4096, -1) == 1);
    assert(!hwfile_open("bin/net", &f)); hwfile_close(&f); hwfile_trim(); assert(!pages);
    /* A changed byte: rejected, and stays rejected until re-registered. */
    assert(hwfile_open("drivers/a.ko", &f) == GZ_EINPROGRESS);
    assert(serve(&good, 512, 1234) == GZ_ECORRUPT && !pages);
    assert(hwfile_open("drivers/a.ko", &f) == GZ_ECORRUPT);
    registered(one, 1, 2);
    assert(hwfile_open("drivers/a.ko", &f) == GZ_EINPROGRESS);
    /* Too many bytes, or a server that can't read the file. */
    memset(user + 0x8000, 0, 5000);
    assert(ctl(&fs, GZ_HW_OP_SUPPLY, 0x8000, 5000) == GZ_EINVAL && !pages);
    assert(hwfile_open("drivers/a.ko", &f) == GZ_ECORRUPT);
    assert(hwfile_open("bin/net", &f) == GZ_EINPROGRESS);
    assert(!ctl(&fs, GZ_HW_OP_FAIL, 0, 0) && !pages && hwfile_open("bin/net", &f) == GZ_ECORRUPT);
    assert(ctl(&fs, GZ_HW_OP_SUPPLY, 0x8000, 1) == GZ_ENOENT);
    /* A file server restarting mid-file starts it again. */
    registered(one, 1, 2);
    assert(hwfile_open("drivers/a.ko", &f) == GZ_EINPROGRESS);
    assert(ctl(&fs, GZ_HW_OP_FETCH, 0x1000, 0) == 1);
    memcpy(user + 0x8000, want, 100); assert(!ctl(&fs, GZ_HW_OP_SUPPLY, 0x8000, 100));
    assert(ctl(&fs, GZ_HW_OP_FETCH, 0x1000, 0) == 0 && !pages);
    assert(hwfile_open("drivers/a.ko", &f) == GZ_EINPROGRESS);
    assert(serve(&good, 2048, -1) == 1);
    /* Re-registration drops what was fetched. */
    registered(one, 1, 2); assert(!pages);

    /* Unsigned firmware is accepted; unsigned or foreign code is not. */
    assert(!ctl(&fs, GZ_HW_OP_BEGIN, 7, 0));
    assert(!add_pkg(&fw));
    assert(add_pkg(&wrong) == GZ_EPERM);
    assert(add_pkg(&old) == GZ_EPERM);                          /* built for another OS */
    assert(add_pkg(&good) == 0);
    assert(add_pkg(&dup) == GZ_EEXIST);                         /* drivers/a.ko again */
    assert(add_pkg(&good) == GZ_EEXIST);
    assert(ctl(&fs, GZ_HW_OP_END, 0, 0) == 3);
    assert(hwfile_open("firmware/fw.bin", &f) == GZ_EINPROGRESS);
    assert(serve(&fw, 300, -1) == 1 && !hwfile_open("firmware/fw.bin", &f) && f.size == 700);
    hwfile_close(&f);
    /* Tampering with a signed manifest. */
    uint32_t n; const uint8_t *m = entry(&good, "Manifest", &n);
    uint8_t copy[GZ_HW_MANIFEST_MAX]; assert(n <= sizeof(copy));
    struct gz_hw_manifest *h = (void *)copy;
    struct gz_hw_entry *e = (void *)(copy + sizeof(*h));
    assert(!ctl(&fs, GZ_HW_OP_BEGIN, 7, 0));
    memcpy(copy, m, n); copy[n - 1] ^= 1; assert(add(copy, n) == GZ_EPERM);        /* signature */
    memcpy(copy, m, n); e[0].sha256[0] ^= 1; assert(add(copy, n) == GZ_EPERM);     /* digest */
    memcpy(copy, m, n); h->flags = 0; assert(add(copy, n - GZ_HW_SIGNATURE) == GZ_EPERM);  /* unsigned code */
    memcpy(copy, m, n); assert(add(copy, n - 1) == GZ_ECORRUPT);                  /* truncated */
    memcpy(copy, m, n); h->count = 9; assert(add(copy, n) == GZ_ECORRUPT);
    memcpy(copy, m, n); strcpy(e[0].path, "drivers/../a.ko"); assert(add(copy, n) == GZ_ECORRUPT);
    memcpy(copy, m, n); strcpy(e[0].path, "kernel.bin"); assert(add(copy, n) == GZ_ECORRUPT);
    memcpy(copy, m, n); e[0].kind = GZ_HW_DATA; assert(add(copy, n) == GZ_ECORRUPT); /* code as data */
    memcpy(copy, m, n); memset(e[0].path, 'a', sizeof(e[0].path)); assert(add(copy, n) == GZ_ECORRUPT);
    memcpy(copy, m, n); e[0].size = GZ_HW_FILE_MAX + 1; assert(add(copy, n) == GZ_ECORRUPT);
    memcpy(copy, m, n); strcpy(h->id, ".bad"); assert(add(copy, n) == GZ_ECORRUPT);
    assert(ctl(&fs, GZ_HW_OP_ADD, 0, GZ_HW_MANIFEST_MAX + 1) == GZ_EINVAL);
    assert(ctl(&fs, GZ_HW_OP_END, 0, 0) == 0);
    assert(ctl(&fs, 99, 0, 0) == GZ_EINVAL);
    assert(!pages);
    puts("hwfile: registration, fetch, digest/signature/OS checks, failures and page accounting PASS");
    return 0;
}
