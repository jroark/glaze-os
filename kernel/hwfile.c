/*
 * Hardware-package files (glaze/hwpkg.h, docs/os/HARDWARE_PACKAGES.md).
 *
 * The file server registers the manifests of the installed hardware packages
 * here; the kernel keeps only that table. When the network dispatcher or the
 * service worker needs a file that romfs lacks, hwfile_open() asks the file
 * server for it (GZ_NOTIFY_HW, then GZ_HW_OP_FETCH and _SUPPLY) and returns
 * GZ_EINPROGRESS; the caller tries again on a later pass, so nothing in the
 * kernel waits for a user process. The bytes land in kernel pages and are
 * only handed out once their SHA-256 matches the manifest.
 *
 * Trust: kernel code (drivers/) and service replacements (bin/) come only from
 * a manifest that carries the OTA release key's Ed25519 signature and names
 * exactly this OS version, since modules are built against one kernel.
 * Firmware (firmware/) runs on the card and may come unsigned. The file
 * server is trusted only to deliver bytes; it can't change what they hash to.
 */
#include "kernel.h"
#include "hwfile.h"
#include "drivers.h"
#include <glaze/hwpkg.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/ed25519.h>
#include <wolfssl/wolfcrypt/sha256.h>
#ifdef HWFILE_TEST
uint8_t hwfile_test_key[32];
#define release_key hwfile_test_key
#else
#include "ota_key.h"            /* generated from config/ota_pubkey.bin */
#define release_key ota_release_key
#endif

#define FILES GZ_HW_STATUS_FILES

static struct file {
    char id[GZ_HW_ID_MAX];
    struct gz_hw_entry e;
    uint32_t state;             /* GZ_HW_FILE_* */
    uint8_t *buf;
    uint32_t pages, refs;
} files[FILES];
static unsigned count;
static bool loaded;             /* GZ_HW_OP_END since the file server started */
static uint32_t generation;
static int fs_port;
static int pending = -1;        /* the file being fetched */
static uint32_t received;
static wc_Sha256 sha;
static uint8_t chunk[2048] __attribute__((aligned(4)));
static uint8_t manifest[GZ_HW_MANIFEST_MAX] __attribute__((aligned(4)));

uint32_t hwfile_generation(void) { return generation; }

static void release(struct file *f)
{
    if (f->buf) {
        for (uint32_t i = 0; i < f->pages; i++)
            page_free(f->buf + i * PAGE_SIZE);
    }
    f->buf = NULL;
    f->pages = 0;
}

static void abort_fetch(uint32_t state)
{
    if (pending < 0)
        return;
    wc_Sha256Free(&sha);
    release(&files[pending]);
    files[pending].state = state;
    pending = -1;
    received = 0;
}

/* One plain name: [A-Za-z0-9._-], not starting with '.', terminated within n.
 * Ids and file names become paths in the file server's bundle folder. */
static bool plain(const char *s, uint32_t n)
{
    if (!memchr(s, 0, n) || !*s || *s == '.')
        return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              *s == '.' || *s == '_' || *s == '-'))
            return false;
    return true;
}

/* drivers/<name>, bin/<name> (code) or firmware/<name> (data). */
static int path_kind(const char *p)
{
    int prefix = !strncmp(p, "drivers/", 8) ? 8 : !strncmp(p, "bin/", 4) ? 4 : !strncmp(p, "firmware/", 9) ? 9 : 0;
    if (!prefix || !plain(p + prefix, GZ_HW_PATH_MAX - prefix))
        return 0;
    return prefix == 9 ? GZ_HW_DATA : GZ_HW_CODE;
}

static int add(const uint8_t *m, uint32_t size)
{
    const struct gz_hw_manifest *h = (const void *)m;
    const struct gz_hw_entry *e = (const void *)(m + sizeof(*h));
    bool code = false;

    if (size < sizeof(*h) || memcmp(h->magic, GZ_HW_MAGIC, 8) || h->format != GZ_HW_FORMAT ||
        (h->flags & ~GZ_HW_SIGNED) || !h->count || h->count > GZ_HW_ENTRIES_MAX || !plain(h->id, sizeof(h->id)) ||
        !memchr(h->os, 0, sizeof(h->os)) ||
        size != sizeof(*h) + h->count * sizeof(*e) + (h->flags & GZ_HW_SIGNED ? GZ_HW_SIGNATURE : 0))
        return GZ_ECORRUPT;
    for (unsigned i = 0; i < h->count; i++) {
        int kind = path_kind(e[i].path);
        if (!kind || (uint32_t)kind != e[i].kind || !e[i].size || e[i].size > GZ_HW_FILE_MAX)
            return GZ_ECORRUPT;
        code |= kind == GZ_HW_CODE;
        for (unsigned j = 0; j < i; j++)
            if (!strcmp(e[j].path, e[i].path)) return GZ_ECORRUPT;
        for (unsigned j = 0; j < count; j++)
            if (!strcmp(files[j].e.path, e[i].path) || !strcmp(files[j].id, h->id)) return GZ_EEXIST;
    }
    if (code && (!(h->flags & GZ_HW_SIGNED) || strcmp(h->os, GLAZE_VERSION)))
        return GZ_EPERM;
    if (h->flags & GZ_HW_SIGNED) {
        ed25519_key key;
        int verified = 0;
        uint32_t signed_bytes = size - GZ_HW_SIGNATURE;
        int bad = wc_ed25519_init(&key) || wc_ed25519_import_public(release_key, 32, &key) ||
                  wc_ed25519_verify_msg(m + signed_bytes, GZ_HW_SIGNATURE, m, signed_bytes, &verified, &key) ||
                  verified != 1;
        wc_ed25519_free(&key);
        if (bad)
            return GZ_EPERM;
    }
    if (count + h->count > FILES)
        return GZ_EFULL;
    for (unsigned i = 0; i < h->count; i++) {
        struct file *f = &files[count++];
        memset(f, 0, sizeof(*f));
        memcpy(f->id, h->id, sizeof(f->id));
        f->e = e[i];
        f->state = GZ_HW_FILE_IDLE;
    }
    kprintf("hw: %s %s (%u files)\n", h->id, h->flags & GZ_HW_SIGNED ? "signed" : "unsigned", h->count);
    return 0;
}

static void forget_all(void)
{
    abort_fetch(GZ_HW_FILE_IDLE);
    for (unsigned i = 0; i < count; i++)
        release(&files[i]);
    memset(files, 0, sizeof(files));
    count = 0;
}

static struct file *find(const char *path)
{
    for (unsigned i = 0; i < count; i++)
        if (!strcmp(files[i].e.path, path)) return &files[i];
    return NULL;
}

static int open_package(const char *path, struct hwfile *out)
{
    struct file *f = find(path);

    if (!f)
        return loaded ? GZ_ENOENT : GZ_EINPROGRESS;
    switch (f->state) {
    case GZ_HW_FILE_BAD:
        return GZ_ECORRUPT;
    case GZ_HW_FILE_READY:
    case GZ_HW_FILE_USED:
        f->refs++;
        f->state = GZ_HW_FILE_USED;
        out->rom = NULL;
        out->buf = f->buf;
        out->size = f->e.size;
        out->slot = (int)(f - files);
        return 0;
    case GZ_HW_FILE_FETCHING:
        return GZ_EINPROGRESS;
    }
    /* Idle: one fetch at a time; the others ask again on a later pass. */
    if (pending >= 0 || fs_port <= 0)
        return GZ_EINPROGRESS;
    f->pages = ALIGN_UP(f->e.size, PAGE_SIZE) / PAGE_SIZE;
    f->buf = page_alloc_n(f->pages);
    if (!f->buf) {
        f->pages = 0;
        return GZ_ENOMEM;
    }
    if (wc_InitSha256(&sha)) {
        release(f);
        return GZ_ENOMEM;
    }
    pending = (int)(f - files);
    received = 0;
    f->state = GZ_HW_FILE_FETCHING;
    ipc_notify(fs_port, GZ_NOTIFY_HW);
    return GZ_EINPROGRESS;
}

int hwfile_open(const char *path, struct hwfile *out)
{
    const struct romfs_entry *rom = romfs_lookup(path);

    memset(out, 0, sizeof(*out));
    out->slot = -1;
    if (rom) {
        out->rom = rom;
        out->size = rom->size;
        return 0;
    }
    return open_package(path, out);
}

int hwfile_open_package(const char *path, struct hwfile *out)
{
    memset(out, 0, sizeof(*out));
    out->slot = -1;
    return open_package(path, out);
}

int hwfile_read(const struct hwfile *f, uint32_t off, void *dst, uint32_t len)
{
    if (off > f->size || len > f->size - off)
        return GZ_EINVAL;
    if (f->rom)
        return romfs_read(f->rom, off, dst, len);
    if (!f->buf)
        return GZ_EINVAL;
    memcpy(dst, f->buf + off, len);
    return 0;
}

void hwfile_close(struct hwfile *f)
{
    if (f->slot >= 0 && (unsigned)f->slot < count && files[f->slot].refs)
        files[f->slot].refs--;
    memset(f, 0, sizeof(*f));
    f->slot = -1;
}

/* Also what was fetched for a consumer that has since gone (a card pulled
 * mid-fetch): a later request fetches it again. */
void hwfile_trim(void)
{
    for (unsigned i = 0; i < count; i++)
        if ((files[i].state == GZ_HW_FILE_USED || files[i].state == GZ_HW_FILE_READY) && !files[i].refs) {
            release(&files[i]);
            files[i].state = GZ_HW_FILE_IDLE;
        }
}

static int supply(struct proc *p, uint32_t ubuf, uint32_t len)
{
    if (pending < 0)
        return GZ_ENOENT;
    struct file *f = &files[pending];
    if (len > f->e.size - received)
        return abort_fetch(GZ_HW_FILE_BAD), GZ_EINVAL;
    while (len) {
        uint32_t n = MIN(len, (uint32_t)sizeof(chunk));
        if (copy_from_as(&p->as, chunk, ubuf, n))
            return abort_fetch(GZ_HW_FILE_IDLE), GZ_EINVAL;
        if (wc_Sha256Update(&sha, chunk, n))
            return abort_fetch(GZ_HW_FILE_BAD), GZ_EIO;
        memcpy(f->buf + received, chunk, n);
        received += n;
        ubuf += n;
        len -= n;
    }
    if (received < f->e.size)
        return 0;
    uint8_t digest[32];
    int bad = wc_Sha256Final(&sha, digest) || memcmp(digest, f->e.sha256, 32);
    wc_Sha256Free(&sha);
    pending = -1;
    received = 0;
    if (bad) {
        release(f);
        f->state = GZ_HW_FILE_BAD;
        kprintf("hw: %s from %s doesn't match its manifest\n", f->e.path, f->id);
        return GZ_ECORRUPT;
    }
    f->state = GZ_HW_FILE_READY;
    kprintf("hw: fetched %s from %s (%u bytes)\n", f->e.path, f->id, f->e.size);
    return 1;
}

static int status(struct proc *p, uint32_t uout, uint32_t size)
{
    static struct gz_hw_status st;

    if (size != sizeof(st))
        return GZ_EINVAL;
    memset(&st, 0, sizeof(st));
    st.generation = generation;
    st.loaded = loaded;
    st.count = count;
    for (unsigned i = 0; i < count; i++) {
        memcpy(st.files[i].id, files[i].id, sizeof(st.files[i].id));
        memcpy(st.files[i].path, files[i].e.path, sizeof(st.files[i].path));
        st.files[i].size = files[i].e.size;
        st.files[i].kind = files[i].e.kind;
        st.files[i].state = files[i].state;
    }
    return copy_to_as(&p->as, uout, &st, sizeof(st));
}

int sys_hw_ctl(struct proc *p, uint32_t op, uint32_t a1, uint32_t a2)
{
    if (op == GZ_HW_OP_STATUS)
        return status(p, a1, a2);
    if (!(p->flags & PROC_FLASH))
        return GZ_EPERM;
    switch (op) {
    case GZ_HW_OP_BEGIN:
        forget_all();
        loaded = false;     /* until END: waiting cards and services hold on */
        fs_port = (int)a1;
        return 0;
    case GZ_HW_OP_ADD:
        if (a2 > sizeof(manifest) || copy_from_as(&p->as, manifest, a1, a2))
            return GZ_EINVAL;
        return add(manifest, a2);
    case GZ_HW_OP_END:
        loaded = true;
        generation++;
        return (int)count;
    case GZ_HW_OP_FETCH: {
        if (pending < 0)
            return 0;
        struct gz_hw_request r;
        memset(&r, 0, sizeof(r));
        memcpy(r.id, files[pending].id, sizeof(r.id));
        memcpy(r.path, files[pending].e.path, sizeof(r.path));
        r.size = files[pending].e.size;
        if (received)       /* a restarted file server begins again */
            abort_fetch(GZ_HW_FILE_IDLE);
        if (copy_to_as(&p->as, a1, &r, sizeof(r)))
            return GZ_EINVAL;
        return pending < 0 ? 0 : 1;
    }
    case GZ_HW_OP_SUPPLY:
        return supply(p, a1, a2);
    case GZ_HW_OP_FAIL:
        if (pending >= 0)
            kprintf("hw: the file server can't read %s from %s\n", files[pending].e.path, files[pending].id);
        abort_fetch(GZ_HW_FILE_BAD);
        return 0;
    }
    return GZ_EINVAL;
}
