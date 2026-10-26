/* Production worker with deterministic stalled HTTP and POSIX-backed flash.
 * Reuse the installer's filesystem adapter to cover real cache I/O. */
#include "radio_host.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main store_package_fixture_main
#include "store_packages.c"
#undef main
#pragma GCC diagnostic pop
#include "../apps/appstore/worker.h"
#include "../apps/browser/web.h"
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <time.h>

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int count;
} sem = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};
void gz_mutex_lock(struct gz_mutex *m)
{
    assert(!pthread_mutex_lock(&m->native));
}
void gz_mutex_unlock(struct gz_mutex *m)
{
    assert(!pthread_mutex_unlock(&m->native));
}
int gz_sem_create(int count)
{
    sem.count = count;
    return 1;
}
int gz_sem_destroy(int id)
{
    (void)id;
    return 0;
}
int gz_sem_wait(int id, int timeout)
{
    (void)id;
    (void)timeout;
    pthread_mutex_lock(&sem.lock);
    while (!sem.count)
        pthread_cond_wait(&sem.cond, &sem.lock);
    sem.count--;
    pthread_mutex_unlock(&sem.lock);
    return 0;
}
int gz_sem_post(int id)
{
    (void)id;
    pthread_mutex_lock(&sem.lock);
    sem.count++;
    pthread_cond_signal(&sem.cond);
    pthread_mutex_unlock(&sem.lock);
    return 0;
}
static pthread_t worker_thread;
struct start {
    void (*fn)(void *);
    void *arg;
};
static void *start(void *arg)
{
    struct start s = *(struct start *)arg;
    free(arg);
    s.fn(s.arg);
    return NULL;
}
int gz_thread_create(void (*fn)(void *), void *arg)
{
    struct start *s = malloc(sizeof(*s));
    assert(s);
    *s = (struct start){fn, arg};
    assert(!pthread_create(&worker_thread, NULL, start, s));
    return 1;
}
uint32_t gz_time_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
void gz_sleep_ms(uint32_t ms)
{
    usleep(ms * 1000);
}
const char *fs_strerror(int e)
{
    (void)e;
    return "Fixture disk error";
}
static unsigned char pixels[12 + 44 * 44 * 2] = {'G', 'Z', 'I', '1', 44, 0, 44};
static uint8_t package_body[1024];
static int package_size;
static pthread_mutex_t http_lock = PTHREAD_MUTEX_INITIALIZER;
static int requests, steps;
struct http {
    int (*sink)(const uint8_t *, int, void *);
    void *arg;
    bool stall, fail, package;
};
struct http *http_start_stream(const char *url, int max, int (*sink)(const uint8_t *, int, void *),
                               void *arg)
{
    (void)max;
    struct http *h = calloc(1, sizeof(*h));
    assert(h);
    h->sink = sink;
    h->arg = arg;
    h->package = strstr(url, ".gzp") != NULL;
    h->stall = strstr(url, "stall") != NULL;
    h->fail = strstr(url, "/fail.gzi") != NULL;
    pthread_mutex_lock(&http_lock);
    requests++;
    pthread_mutex_unlock(&http_lock);
    return h;
}
int http_step(struct http *h)
{
    pthread_mutex_lock(&http_lock);
    steps++;
    pthread_mutex_unlock(&http_lock);
    if (h->stall)
        return 0;
    if (h->fail)
        return WE_TIMEOUT;
    int rv = h->package ? h->sink(package_body, package_size, h->arg)
                        : h->sink(pixels, sizeof(pixels), h->arg);
    return rv ? rv : 1;
}
int http_status(const struct http *h)
{
    (void)h;
    return 200;
}
void http_free(struct http *h)
{
    free(h);
}
static struct store_result wait_result(void)
{
    struct store_result r;
    int progress;
    uint32_t before = gz_time_ms();
    while (!store_worker_poll(&r, &progress)) {
        assert(gz_time_ms() - before < 3000);
        usleep(1000);
    }
    return r;
}
static int request_count(void)
{
    pthread_mutex_lock(&http_lock);
    int n = requests;
    pthread_mutex_unlock(&http_lock);
    return n;
}
static void await_request(int n)
{
    uint32_t before = gz_time_ms();
    while (request_count() < n) {
        assert(gz_time_ms() - before < 3000);
        usleep(1000);
    }
}
static void hash_data(const void *data, int size, char *hex)
{
    uint8_t sum[32];
    wc_Sha256 h;
    assert(!wc_InitSha256(&h));
    assert(!wc_Sha256Update(&h, data, size));
    assert(!wc_Sha256Final(&h, sum));
    wc_Sha256Free(&h);
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", sum[i]);
}
int main(void)
{
    char template[] = "/tmp/glaze-store-worker-XXXXXX";
    char *directory = mkdtemp(template);
    assert(directory);
    strcpy(root, directory);
    assert(store_worker_start());
    struct store_job j = {.kind = STORE_BOOT, .generation = 1};
    store_worker_submit(&j);
    struct store_result r = wait_result();
    assert(!r.catalog && r.kind == STORE_BOOT);
    store_result_free(&r);

    j = (struct store_job){.kind = STORE_ICON, .generation = 2, .index = 3};
    strcpy(j.app.id, "first");
    strcpy(j.app.icon, "apps/first/icon.gzi");
    hash_data(pixels, sizeof(pixels), j.app.icon_sha256);
    store_worker_submit(&j);
    r = wait_result();
    assert(!r.error && r.data && r.index == 3 && r.generation == 2);
    assert(r.image.w == 44 && r.image.h == 44);
    void *owned = r.data;
    r.data = NULL;
    store_result_free(&r);
    int count = request_count();
    j.kind = STORE_ICON_CACHE;
    store_worker_submit(&j);
    r = wait_result();
    assert(r.data && !r.error && request_count() == count);
    assert(!memcmp(owned, r.data, sizeof(pixels)));
    /* UI ownership survives subsequent worker results and cancellation. */
    store_result_free(&r);
    store_worker_cancel();
    assert(!memcmp(owned, pixels, sizeof(pixels)));
    free(owned);

    char path[FS_PATH_MAX];
    snprintf(path, sizeof(path), STORE_CACHE "/first-%.16s.gzi", j.app.icon_sha256);
    assert(fs_write_file(path, "broken", 6) == 6);
    store_worker_submit(&j);
    r = wait_result();
    assert(!r.data && request_count() == count);
    store_result_free(&r);
    j.kind = STORE_ICON;
    store_worker_submit(&j);
    r = wait_result();
    assert(r.data && !r.error && request_count() == count + 1);
    store_result_free(&r);

    strcpy(j.app.id, "slow");
    strcpy(j.app.icon, "apps/slow/stall.gzi");
    count = request_count();
    store_worker_submit(&j);
    await_request(count + 1);
    uint32_t before = gz_time_ms();
    store_worker_cancel();
    assert(gz_time_ms() - before < 100);
    j.generation = 3;
    j.index = 9;
    strcpy(j.app.id, "replacement");
    strcpy(j.app.icon, "apps/replacement/icon.gzi");
    store_worker_submit(&j);
    r = wait_result();
    assert(r.generation == 3 && r.index == 9 && r.data);
    store_result_free(&r);

    strcpy(j.app.id, "failed");
    strcpy(j.app.icon, "apps/failed/fail.gzi");
    store_worker_submit(&j);
    r = wait_result();
    assert(r.error == WE_TIMEOUT && !r.data);
    store_result_free(&r);
    strcpy(j.app.icon, "apps/failed/icon.gzi");
    store_worker_submit(&j);
    r = wait_result();
    assert(!r.error && r.data);
    store_result_free(&r);

    /* Bad digest cannot populate either memory or the disk cache. */
    strcpy(j.app.id, "wrong-hash");
    memset(j.app.icon_sha256, '0', 64);
    store_worker_submit(&j);
    r = wait_result();
    assert(r.error && !r.data);
    store_result_free(&r);

    bool done[5] = {false};
    int priority[] = {4, 2, 3};
    assert(store_icon_next(5, priority, 3, done) == 4);
    done[4] = true;
    assert(store_icon_next(5, priority, 3, done) == 2);
    priority[0] = 1; /* Scroll/search changes the next pending request. */
    assert(store_icon_next(5, priority, 3, done) == 1);
    memset(done, 1, sizeof(done));
    assert(store_icon_next(5, priority, 3, done) == -1);
    done[0] = false;
    assert(store_icon_next(5, priority, 3, done) == 0);
    struct store_catalog *old = calloc(1, sizeof(*old));
    assert(old);
    old->count = 2;
    old->apps[1] = j.app;
    assert(store_icon_match(old, &j.app) == 1);
    j.app.icon_sha256[63] = '1';
    assert(store_icon_match(old, &j.app) == -1);
    struct store_catalog *next = calloc(1, sizeof(*next));
    assert(next);
    *next = *old;
    next->apps[0] = old->apps[1];
    memset(&next->apps[1], 0, sizeof(next->apps[1]));
    strcpy(next->apps[1].id, "new-app");
    strcpy(next->apps[0].name, "Changed metadata");
    struct store_icon icons[STORE_MAX_APPS] = {0};
    icons[0].data = malloc(16); /* Removed app is freed under ASan. */
    icons[1].data = malloc(16);
    icons[1].ok = icons[1].tried = true;
    void *retained = icons[1].data;
    store_icons_reconcile(old, next, icons);
    assert(icons[0].data == retained && icons[0].ok && !icons[0].tried);
    assert(!icons[1].data && !icons[1].ok);
    *old = *next;
    next->apps[0].icon_sha256[63] = '2';
    store_icons_reconcile(old, next, icons);
    assert(!icons[0].data && !icons[0].ok);
    free(next);
    free(old);

    assert(!fs_mkdirs("/Applications/first.app"));
    const char info[] = "id=first\nname=First\nversion=1.0\nexec=app\n";
    assert(fs_write_file("/Applications/first.app/Info", info, sizeof(info) - 1) ==
           sizeof(info) - 1);
    struct store_catalog *snapshot = calloc(1, sizeof(*snapshot));
    assert(snapshot);
    snapshot->count = 2;
    strcpy(snapshot->apps[0].id, "first");
    strcpy(snapshot->apps[0].version, "1.1");
    strcpy(snapshot->apps[1].id, "missing");
    j = (struct store_job){.kind = STORE_SCAN, .catalog = snapshot, .generation = 4};
    store_worker_submit(&j);
    r = wait_result();
    assert(r.local && r.generation == 4 && r.local[0].installed && r.local[0].update);
    assert(!r.local[1].installed && !r.local[1].update);
    assert(!strcmp(r.local[0].bundle, "/Applications/first.app"));
    store_result_free(&r);

    package("/fixture.gzp", "1.0", false);
    package_size = fs_read_file("/fixture.gzp", package_body, sizeof(package_body));
    assert(package_size > 0 && package_size < (int)sizeof(package_body));
    j = (struct store_job){.kind = STORE_DOWNLOAD, .generation = 5};
    strcpy(j.app.id, "demo");
    strcpy(j.app.version, "1.0");
    strcpy(j.app.package, "apps/demo/1.0/app.gzp");
    j.app.size = package_size;
    hash_data(package_body, package_size, j.app.sha256);
    store_worker_submit(&j);
    r = wait_result();
    assert(!r.error && r.received == package_size && *r.package);
    assert(!strcmp(r.package, "/Applications/.store-download"));
    assert(!fs_remove(r.package)); /* The consumer owns a successful package. */
    store_result_free(&r);

    /* Cancelling a completed but unconsumed package must remove its file.
     * The next result is a barrier after the worker's deferred cleanup. */
    count = request_count();
    store_worker_submit(&j);
    await_request(count + 1);
    usleep(50000);
    store_worker_cancel();
    struct store_job barrier = {.kind = STORE_ICON_CACHE, .generation = 6};
    store_worker_submit(&barrier);
    r = wait_result();
    struct fs_stat st;
    assert(fs_stat("/Applications/.store-download", &st) == GZ_ENOENT);
    store_result_free(&r);

    strcpy(j.app.package, "apps/demo/1.0/stall.gzp");
    count = request_count();
    store_worker_submit(&j);
    await_request(count + 1);
    before = gz_time_ms();
    store_worker_cancel();
    assert(gz_time_ms() - before < 100);
    store_worker_submit(&barrier);
    r = wait_result();
    assert(r.generation == 6 && fs_stat("/Applications/.store-download", &st) == GZ_ENOENT);
    store_result_free(&r);
    assert(!fs_remove("/fixture.gzp"));

    /* Stop the host-only thread after every result has been released. */
    pthread_cancel(worker_thread);
    pthread_join(worker_thread, NULL);
    assert(!fs_remove_all("/Library"));
    assert(!fs_remove_all("/Applications"));
    assert(!rmdir(root));
    puts("store worker: PASS (cache, priority, ownership, cancellation, failures)");
    return 0;
}
