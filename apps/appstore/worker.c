/* All browsing I/O and TLS work belongs to this thread. The mailbox lock
 * protects only ownership transfers, never I/O or image verification. */
#include "worker.h"
#include "net.h"
#include "web.h"
#include <glaze/fs.h>
#include <glaze/sys.h>
#include <glaze/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/sha256.h>
#ifndef APPSTORE_URL
#define APPSTORE_URL "https://appstore.glaze-os.com"
#endif
static struct gz_mutex lock = GZ_MUTEX_INIT;
static int wake;
static unsigned serial;
static bool pending, ready;
static struct store_job command;
static struct store_result completion;
static int progress;
static char discard_package[FS_PATH_MAX];

void store_result_free(struct store_result *r)
{
    free(r->catalog);
    free(r->local);
    free(r->data);
    memset(r, 0, sizeof(*r));
}
static bool cancelled(unsigned token)
{
    gz_mutex_lock(&lock);
    bool stop = token != serial;
    gz_mutex_unlock(&lock);
    return stop;
}
static bool digest(const void *data, int n, const char *expected)
{
    uint8_t sum[32];
    wc_Sha256 hash;
    if (wc_InitSha256(&hash))
        return false;
    int rv = wc_Sha256Update(&hash, data, n);
    if (!rv)
        rv = wc_Sha256Final(&hash, sum);
    wc_Sha256Free(&hash);
    if (rv)
        return false;
    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", sum[i]);
    return !strcmp(hex, expected);
}
static struct store_catalog *read_catalog(const char *path)
{
    struct fs_stat st;
    if (fs_stat(path, &st) || !st.size || st.size > STORE_CATALOG_MAX)
        return NULL;
    char *data = malloc(st.size);
    struct store_catalog *c = malloc(sizeof(*c));
    if (!data || !c || fs_read_file(path, data, st.size) != (int)st.size ||
        !store_parse(c, data, st.size)) {
        free(c);
        c = NULL;
    }
    free(data);
    return c;
}
static void icon_path(const struct store_app *a, char *path, size_t n)
{
    snprintf(path, n, STORE_CACHE "/%s-%.16s.gzi", a->id, a->icon_sha256);
}
static bool read_icon(const struct store_app *a, const char *path, struct store_result *r)
{
    struct fs_stat st;
    if (fs_stat(path, &st) || !st.size || st.size > STORE_ICON_MAX)
        return false;
    void *data = malloc(st.size);
    gfx_image image;
    if (!data)
        return false;
    if (fs_read_file(path, data, st.size) != (int)st.size ||
        !digest(data, st.size, a->icon_sha256) || !gfx_image_parse(&image, data, st.size) ||
        image.w != 44 || image.h != 44) {
        free(data);
        return false;
    }
    r->data = data;
    r->image = image;
    return true;
}
static void prune(const struct store_catalog *c, unsigned token)
{
    struct fs_dirent entry;
    uint32_t skip = 0;
    while (!cancelled(token) && fs_list(STORE_CACHE, skip, &entry, 1) == 1) {
        size_t len = strlen(entry.name);
        bool keep = true;
        if (entry.type == FS_FILE && len > 4 && !strcmp(entry.name + len - 4, ".gzi")) {
            keep = false;
            for (int i = 0; i < c->count; i++) {
                char path[FS_PATH_MAX];
                icon_path(&c->apps[i], path, sizeof(path));
                if (!strcmp(entry.name, strrchr(path, '/') + 1)) {
                    keep = true;
                    break;
                }
            }
        }
        if (!keep) {
            char path[FS_PATH_MAX];
            snprintf(path, sizeof(path), STORE_CACHE "/%s", entry.name);
            if (fs_remove(path))
                skip++;
        } else
            skip++;
    }
}
static const char *network_error(int e)
{
    switch (e) {
    case WE_NETDOWN:
        return "Connect an Ethernet card, then Retry.";
    case WE_DNS:
        return "Store not found. Check your connection.";
    case WE_TIMEOUT:
        return "Connection timed out. Please Retry.";
    case WE_CERT_DATE:
        return "Check the date in Settings, then Retry.";
    case WE_CERT:
    case WE_CERT_NAME:
    case WE_TLS:
        return "Cannot verify the secure store connection.";
    case WE_TOO_BIG:
        return "This download is too large.";
    default:
        return "Download interrupted. Please Retry.";
    }
}
struct transfer {
    int fd, received, error;
    wc_Sha256 hash;
    unsigned token;
};
static int write_bytes(const uint8_t *data, int n, void *arg)
{
    struct transfer *t = arg;
    if (cancelled(t->token))
        return WE_RESET;
    if (fs_write(t->fd, data, n) != n) {
        t->error = GZ_ENOSPC;
        return WE_TOO_BIG;
    }
    if (wc_Sha256Update(&t->hash, data, n)) {
        t->error = GZ_EIO;
        return WE_PROTOCOL;
    }
    t->received += n;
    gz_mutex_lock(&lock);
    if (t->token == serial)
        progress = t->received;
    gz_mutex_unlock(&lock);
    return 0;
}
static void download(const struct store_job *j, struct store_result *r, unsigned token)
{
    char path[FS_PATH_MAX], url[320];
    const char *asset = j->kind == STORE_REFRESH ? "catalog.txt"
                        : j->kind == STORE_ICON  ? j->app.icon
                                                 : j->app.package;
    if (!store_path_ok(asset)) {
        r->error = WE_PROTOCOL;
        return;
    }
    if (j->kind == STORE_DOWNLOAD) {
        struct fs_info volume;
        struct fs_stat st;
        if (*j->root && fs_stat("/Card", &st)) {
            strlcpy(r->message, "Insert the card, then try again.", sizeof(r->message));
            r->error = WE_NOTFOUND;
            return;
        }
        if (fs_statvfs(*j->root ? j->root : "/", &volume) ||
            volume.total - volume.used < j->app.size * 2u + 32768u) {
            strlcpy(r->message, "Not enough space to download and install.", sizeof(r->message));
            r->error = WE_TOO_BIG;
            return;
        }
        char dir[32];
        snprintf(dir, sizeof(dir), "%s/Applications", j->root);
        int rv = fs_mkdirs(dir);
        if (rv) {
            r->error = rv;
            strlcpy(r->message, fs_strerror(rv), sizeof(r->message));
            return;
        }
        snprintf(path, sizeof(path), "%s/Applications/.store-download", j->root);
    } else
        strlcpy(path, STORE_CACHE "/download.tmp", sizeof(path));
    struct transfer t = {.fd = -1, .token = token};
    t.fd = fs_open(path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    if (t.fd < 0) {
        r->error = t.fd;
        strlcpy(r->message, fs_strerror(t.fd), sizeof(r->message));
        return;
    }
    if (wc_InitSha256(&t.hash)) {
        fs_close(t.fd);
        fs_remove(path);
        r->error = WE_NOMEM;
        return;
    }
    snprintf(url, sizeof(url), "%s/%s", APPSTORE_URL, asset);
    int max = j->kind == STORE_REFRESH ? STORE_CATALOG_MAX
              : j->kind == STORE_ICON  ? STORE_ICON_MAX
                                       : (int)j->app.size;
    struct http *h = http_start_stream(url, max, write_bytes, &t);
    int rv = h ? 0 : WE_NOMEM;
    while (h && !cancelled(token) && !(rv = http_step(h)))
        gz_sleep_ms(5);
    r->status = h ? http_status(h) : 0;
    r->received = t.received;
    http_free(h);
    int closed = fs_close(t.fd);
    uint8_t sum[32] = {0};
    int hashed = wc_Sha256Final(&t.hash, sum);
    wc_Sha256Free(&t.hash);
    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", sum[i]);
    if (cancelled(token)) {
        fs_remove(path);
        return;
    }
    printf("appstore: fetch kind=%d result=%d HTTP=%d bytes=%d\n",
           j->kind == STORE_REFRESH ? 1
           : j->kind == STORE_ICON  ? 2
                                    : 3,
           rv, r->status, t.received);
    if (rv < 0 || r->status != 200 || closed < 0 || hashed) {
        r->error = rv < 0 ? rv : WE_PROTOCOL;
        if (t.error || closed < 0)
            strlcpy(r->message, fs_strerror(t.error ? t.error : closed), sizeof(r->message));
        else if (rv < 0)
            strlcpy(r->message, network_error(rv), sizeof(r->message));
        else
            snprintf(r->message, sizeof(r->message), "Store returned HTTP %d. Please Retry.",
                     r->status);
    } else if (j->kind == STORE_REFRESH) {
        r->catalog = read_catalog(path);
        if (r->catalog) {
            fs_rename(path, STORE_CACHE "/catalog.txt");
            prune(r->catalog, token);
        } else {
            r->error = WE_PROTOCOL;
            strlcpy(r->message, "Invalid store catalog. Please Retry.", sizeof(r->message));
        }
    } else if (j->kind == STORE_ICON) {
        if (!read_icon(&j->app, path, r))
            r->error = WE_PROTOCOL;
        else {
            char dest[FS_PATH_MAX];
            icon_path(&j->app, dest, sizeof(dest));
            fs_rename(path, dest);
        }
    } else {
        struct pkg_info info;
        if (t.received != (int)j->app.size || strcmp(hex, j->app.sha256) ||
            pkg_read_info(path, &info) || strcmp(info.id, j->app.id) ||
            strcmp(info.version, j->app.version)) {
            r->error = WE_PROTOCOL;
            strlcpy(r->message, "Package verification failed. Please Retry.", sizeof(r->message));
        } else {
            strlcpy(r->package, path, sizeof(r->package));
            return;
        }
    }
    fs_remove(path);
}
static void run(const struct store_job *j, struct store_result *r, unsigned token)
{
    r->kind = j->kind;
    r->generation = j->generation;
    r->index = j->index;
    if (j->kind == STORE_BOOT) {
        fs_mkdirs(STORE_CACHE);
        fs_mkdirs("/Applications");
        fs_remove(STORE_CACHE "/download.tmp");
        fs_remove("/Applications/.store-download");
        fs_remove("/Card/Applications/.store-download");
        r->catalog = read_catalog(STORE_CACHE "/catalog.txt");
    } else if (j->kind == STORE_SCAN) {
        r->local = calloc(STORE_MAX_APPS, sizeof(*r->local));
        if (!r->local) {
            r->error = WE_NOMEM;
            return;
        }
        for (int i = 0; i < j->catalog->count && !cancelled(token); i++) {
            struct store_local *l = &r->local[i];
            const struct store_app *a = &j->catalog->apps[i];
            l->installed = !pkg_find(a->id, l->bundle) && !pkg_bundle_info(l->bundle, &l->info);
            l->update = l->installed && store_version_ok(l->info.version) &&
                        store_version_cmp(a->version, l->info.version) > 0;
        }
    } else if (j->kind == STORE_ICON_CACHE || j->kind == STORE_ICON) {
        char path[FS_PATH_MAX];
        icon_path(&j->app, path, sizeof(path));
        if (!read_icon(&j->app, path, r) && j->kind == STORE_ICON)
            download(j, r, token);
    } else
        download(j, r, token);
}
static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        gz_sem_wait(wake, -1);
        gz_mutex_lock(&lock);
        char cleanup[FS_PATH_MAX];
        strlcpy(cleanup, discard_package, sizeof(cleanup));
        discard_package[0] = 0;
        if (!pending) {
            gz_mutex_unlock(&lock);
            if (*cleanup)
                fs_remove(cleanup);
            continue;
        }
        struct store_job j = command;
        unsigned token = serial;
        pending = false;
        command.catalog = NULL;
        gz_mutex_unlock(&lock);
        if (*cleanup)
            fs_remove(cleanup);
        struct store_result r = {0};
        run(&j, &r, token);
        free(j.catalog);
        gz_mutex_lock(&lock);
        bool stale = token != serial;
        if (!stale) {
            completion = r;
            ready = true;
        }
        gz_mutex_unlock(&lock);
        if (stale) {
            if (*r.package)
                fs_remove(r.package);
            store_result_free(&r);
        }
    }
}
bool store_worker_start(void)
{
    wake = gz_sem_create(0);
    if (wake < 0)
        return false;
    if (gz_thread_create(worker, NULL) < 0) {
        gz_sem_destroy(wake);
        return false;
    }
    return true;
}
void store_worker_cancel(void)
{
    struct store_result discarded = {0};
    struct store_catalog *old;
    gz_mutex_lock(&lock);
    serial++;
    old = command.catalog;
    command.catalog = NULL;
    pending = false;
    if (ready) {
        discarded = completion;
        if (*discarded.package)
            strlcpy(discard_package, discarded.package, sizeof(discard_package));
        memset(&completion, 0, sizeof(completion));
        ready = false;
    }
    progress = 0;
    gz_mutex_unlock(&lock);
    /* Completed package paths are cleaned by the worker,
     * never by UI filesystem calls that could wait behind DNS. */
    free(old);
    store_result_free(&discarded);
    gz_sem_post(wake);
}
void store_worker_submit(const struct store_job *j)
{
    store_worker_cancel();
    gz_mutex_lock(&lock);
    command = *j;
    pending = true;
    gz_mutex_unlock(&lock);
    gz_sem_post(wake);
}
bool store_worker_poll(struct store_result *r, int *received)
{
    gz_mutex_lock(&lock);
    *received = progress;
    bool got = ready;
    if (got) {
        *r = completion;
        memset(&completion, 0, sizeof(completion));
        ready = false;
    }
    gz_mutex_unlock(&lock);
    return got;
}
