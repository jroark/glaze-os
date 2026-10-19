/* Guest tests: exercise actual MIPS GOTs, physical mappings and lifecycle. */
#include <glaze/sys.h>
#include <glaze/thread.h>
#include <glaze/fs.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int failures, events[256], nevents;
static struct gz_mutex event_lock = GZ_MUTEX_INIT;
void ldtest_event(int n) {
    gz_mutex_lock(&event_lock);
    if (nevents < (int)ARRAY_SIZE(events)) events[nevents++] = n;
    gz_mutex_unlock(&event_lock);
}
int ldtest_callback(int n) { return 2 * n; }
static void check(bool ok, const char *name) { printf("ldtest: %s %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; }
static struct gz_image_stats stats(void) { struct gz_image_stats s = {0}; gz_image_stats(&s); return s; }
static void wait_exit(int pid) { for (int i = 0; i < 1000 && gz_proc_alive(pid); i++) gz_sleep_ms(2); }
static bool last_events(int a, int b) { return nevents >= 2 && events[nevents - 2] == a && events[nevents - 1] == b; }
static void basic(void)
{
    void *h = dlopen("libld-plugin.so.1", RTLD_NOW);
    check(h != NULL, "dlopen builtin dependency graph");
    if (!h) { printf("ldtest: error %s\n", dlerror()); return; }
    check(last_events(1, 2), "dependency constructors first");
    int (*step)(int) = dlsym(h, "plugin_step");
    int (*weak)(void) = dlsym(h, "plugin_weak");
    void *(*alloc)(int) = dlsym(h, "plugin_alloc");
    check(step && step(3) == 53, "GOT, pointers, data and executable callbacks");
    check(weak && weak(), "unresolved weak symbol is zero");
    check(dlsym(RTLD_DEFAULT, "plugin_step") == NULL, "RTLD_LOCAL visibility");
    check(dlerror() && dlerror() == NULL, "dlerror clears per-thread error");
    void *p = alloc ? alloc(80) : NULL;
    check(p != NULL, "plugin uses process allocator"); free(p);
    void *again = dlopen("libld-plugin.so.1", RTLD_NOW);
    check(again == h, "duplicate opens reuse the object");
    check(dlclose(again) == 0 && step(3) == 53, "close retains referenced object");
    check(dlclose(h) == 0 && last_events(-2, -1), "reverse destructors and final close");
    check(dlclose(h) == -1, "stale handle rejected"); dlerror();
    check(dlopen("does-not-exist.so", RTLD_NOW) == NULL, "missing dependency failure"); dlerror();
    check(dlopen("libld-missing.so.1", RTLD_NOW) == NULL, "unresolved strong symbol failure"); dlerror();
    check(dlopen("libld-simple.so.1", 1) == NULL, "unsupported lazy binding rejected"); dlerror();
    h = dlopen(NULL, RTLD_NOW);
    check(h && dlsym(h, "ldtest_callback") == ldtest_callback, "main-program handle");
    if (h) dlclose(h);
}
static void scope_and_cycles(void)
{
    void *h = dlopen("libld-global.so.1", RTLD_NOW);
    check(h != NULL, "local provider open");
    check(dlopen("libld-consumer.so.1", RTLD_NOW) == NULL, "local object does not satisfy unrelated import"); dlerror();
    void *g = dlopen("libld-global.so.1", RTLD_NOW | RTLD_GLOBAL);
    void *consumer = dlopen("libld-consumer.so.1", RTLD_NOW);
    int (*get)(void) = consumer ? dlsym(consumer, "consumer_value") : NULL;
    check(g == h && get && get() == 33, "RTLD_GLOBAL promotion");
    if (g) dlclose(g);
    if (h) dlclose(h);
    check(get && get() == 33, "symbol binding keeps provider alive");
    if (consumer) dlclose(consumer);
    check(!dlsym(RTLD_DEFAULT, "ldtest_global"), "unload removes global exports"); dlerror();
    void *dep = dlopen("libld-scope_dep.so.1", RTLD_NOW);
    void *parent = dlopen("libld-scope_parent.so.1", RTLD_NOW);
    get = parent ? dlsym(parent, "scope_value") : NULL;
    check(dep && get && get() == 20, "handle scope searches object before preloaded dependency");
    if (parent) dlclose(parent);
    if (dep) dlclose(dep);
    struct gz_image_stats before = stats();
    h = dlopen("libld-cycle_a.so.1", RTLD_NOW);
    get = h ? dlsym(h, "cycle_a") : NULL;
    check(get && get() == 20, "cyclic dependencies");
    if (h) dlclose(h);
    check(stats().mappings == before.mappings, "cyclic dependencies reclaimed");
    h = dlopen("libld-reentry.so.1", RTLD_NOW);
    get = h ? dlsym(h, "reentry_ok") : NULL;
    check(get && get(), "constructor reenters dlopen");
    if (h) dlclose(h);
    check(stats().mappings == before.mappings, "destructor reenters dlclose");
}
static int done, thread_errors;
static void worker(void *unused)
{
    (void)unused;
    for (int i = 0; i < 12; i++) {
        void *h = dlopen("libld-simple.so.1", RTLD_NOW);
        int (*get)(void) = h ? dlsym(h, "simple_value") : NULL;
        if (!get || get() != 7 || dlclose(h)) { gz_mutex_lock(&event_lock); thread_errors++; gz_mutex_unlock(&event_lock); }
        dlerror();
        if (dlsym(RTLD_DEFAULT, "missing_per_thread")) { gz_mutex_lock(&event_lock); thread_errors++; gz_mutex_unlock(&event_lock); }
        gz_sleep_ms(1);
        if (!dlerror() || dlerror()) { gz_mutex_lock(&event_lock); thread_errors++; gz_mutex_unlock(&event_lock); }
    }
    for (int i = 0; i < 4; i++) {
        void *h = dlopen("libld-slow.so.1", RTLD_NOW);
        int (*get)(void) = h ? dlsym(h, "slow_value") : NULL;
        if (!get || get() != 123 || dlclose(h)) { gz_mutex_lock(&event_lock); thread_errors++; gz_mutex_unlock(&event_lock); }
    }
    gz_sem_post(done);
}
static void concurrent(void)
{
    done = gz_sem_create(0);
    for (int i = 0; i < 4; i++) check(gz_thread_create(worker, NULL) > 0, "loader thread create");
    for (int i = 0; i < 4; i++) gz_sem_wait(done, 5000);
    gz_sleep_ms(20);
    check(thread_errors == 0, "concurrent loads/closes and thread-local errors");
    gz_sem_destroy(done);
}
static void shared(void)
{
    void *h = dlopen("libld-simple.so.1", RTLD_NOW);
    int (*get)(void) = h ? dlsym(h, "simple_value") : NULL;
    void (*set)(int) = h ? dlsym(h, "simple_set") : NULL;
    if (!get || !set) { check(false, "sharing fixture loaded"); return; }
    set(99); struct gz_image_page page;
    check(!gz_image_query(get, &page) && !page.writable, "library text is mapped read-only");
    int port = gz_port_create(); gz_port_register(port, "ldtest");
    int pid = gz_spawn("bin/ldtest_peer", 0);
    struct gz_msg m, r = {{0}};
    int token = gz_recv(port, &m, 3000);
    check(pid > 0 && token > 0 && m.w[1] == page.frame && m.w[2] >= 2 && !m.w[3], "two processes share physical code frames");
    check(token > 0 && m.w[0] == 7 && get() == 99, "writable state is process private");
    if (token > 0) gz_reply(token, &r);
    token = gz_recv(port, &m, 3000);
    check(token > 0 && m.w[0] == 22 && get() == 99, "peer writes leave parent data intact");
    if (token > 0) gz_reply(token, &r);
    wait_exit(pid);
    check(!gz_proc_alive(pid) && get() == 99, "peer exit preserves shared code");
    pid = gz_spawn("bin/ldtest_ro", 0); wait_exit(pid);
    check(pid > 0 && !gz_proc_alive(pid) && get() == 99, "text writes fault without corrupting another process");
    pid = gz_spawn("bin/ldtest_peer", 0); token = gz_recv(port, &m, 3000);
    r.w[0] = 1; if (token > 0) gz_reply(token, &r);
    check(pid > 0 && token > 0 && !gz_kill(pid) && get() == 99, "killed process releases shared mappings safely");
    dlclose(h);
}
static void files(void)
{
    uint32_t size;
    const void *bytes = gz_romfs_map("lib/libld-simple.so.1", &size);
    check(bytes != NULL, "filesystem fixture bytes"); if (!bytes) return;
    fs_mkdirs("/Library/Libraries");
    const char *path = "/Library/Libraries/ldtest.so";
    check(fs_write_file(path, bytes, size) == (int)size, "write filesystem library");
    void *h = dlopen(path, RTLD_NOW);
    int (*get)(void) = h ? dlsym(h, "simple_value") : NULL;
    check(get && get() == 7, "dlopen from writable filesystem");
    unsigned char bad[64] = {0};
    fs_write_file(path, bad, sizeof(bad));
    check(get && get() == 7, "file replacement does not change loaded snapshot");
    if (h) dlclose(h);
    check(!dlopen(path, RTLD_NOW), "reload sees replaced invalid file"); dlerror();
    check(!dlopen("/Library/Libraries/missing.so", RTLD_NOW), "filesystem library not found"); dlerror();
    fs_remove(path);
}
static void out_of_memory(void)
{
    void *h = dlopen("libld-bss.so.1", RTLD_NOW);
    check(h != NULL, "large BSS fixture loaded");
    int port = gz_port_lookup("ldtest");
    struct gz_sysinfo before, after; gz_sysinfo(&before);
    int pid = gz_spawn("bin/ldtest_oom", 0);
    struct gz_msg m, r = {{0}};
    int token = gz_recv(port, &m, 5000);
    check(pid > 0 && token > 0 && m.w[0], "partial mapping OOM rolls back all pages");
    if (token > 0) gz_reply(token, &r);
    wait_exit(pid); gz_sysinfo(&after);
    check(!gz_proc_alive(pid) && after.free_pages == before.free_pages, "OOM process exit returns its mappings");
    if (h) dlclose(h);
}
static void malformed(void)
{
    uint32_t size;
    const void *bytes = gz_romfs_map("lib/libld-simple.so.1", &size);
    if (!bytes) { check(false, "malformed fixture available"); return; }
    unsigned char *copy = malloc(size);
    struct gz_image_info info;
    struct gz_image_stats before = stats();
    bool ok = copy != NULL;
    for (int kind = 0; copy && kind < 11; kind++) {
        memcpy(copy, bytes, size);
        struct gz_elf_ehdr *e = (void *)copy;
        struct gz_elf_phdr *p = (void *)(copy + e->phoff), *load = NULL;
        for (int i = 0; i < e->phnum; i++) if (p[i].type == GZ_PT_LOAD) { load = &p[i]; break; }
        switch (kind) {
        case 0: e->ident[0] = 0; break;
        case 1: e->phoff = UINT32_MAX; break;
        case 2: e->phoff++; break;
        case 3: e->phnum = 33; break;
        case 4: e->flags |= 0x04000000u; break;
        case 5: e->machine = 3; break;
        case 6: load->filesz = load->memsz + 1; break;
        case 7: load->memsz = UINT32_MAX; break;
        case 8: load->flags = 7; break;
        case 9: load->type = GZ_PT_TLS; break;
        case 10: load->offset = UINT32_MAX; break;
        }
        int rv = gz_image_map(copy, size, GZ_IMAGE_BYTES, &info);
        if (!rv) { ok = false; gz_image_release(info.base); }
    }
    check(ok && stats().mappings == before.mappings, "malformed ELF ranges, ABI and segments rejected");
    const char *path = "/Library/Libraries/ldtest-bad.so";
    ok = copy != NULL;
    for (int kind = 0; copy && kind < 7; kind++) {
        memcpy(copy, bytes, size);
        struct gz_elf_ehdr *e = (void *)copy;
        struct gz_elf_phdr *p = (void *)(copy + e->phoff), *dp = NULL;
        for (int i = 0; i < e->phnum; i++) if (p[i].type == GZ_PT_DYNAMIC) dp = &p[i];
        struct gz_elf_dyn *d = (void *)(copy + dp->offset);
        uint32_t tag = kind == 0 ? 5 : kind == 1 ? 6 : kind == 2 ? 4 : kind == 3 ? 10 : kind == 4 ? 11 : kind == 5 ? 0x70000011 : 0x7000000a;
        for (unsigned i = 0; d[i].tag; i++) if (d[i].tag == tag) {
            d[i].val = kind == 3 ? UINT32_MAX : kind == 4 ? 1 : UINT32_MAX;
        }
        if (fs_write_file(path, copy, size) != (int)size) ok = false;
        void *h = dlopen(path, RTLD_NOW);
        if (h) { ok = false; dlclose(h); } dlerror();
    }
    fs_remove(path); free(copy);
    check(ok && stats().mappings == before.mappings, "malformed dynamic tables fail without leaking mappings");
}
static void repeated(void)
{
    void *h = dlopen("libld-simple.so.1", RTLD_NOW); if (h) dlclose(h);
    struct gz_image_stats before = stats(); struct gz_sysinfo a, b; gz_sysinfo(&a);
    bool ok = true;
    for (int i = 0; i < 80; i++) {
        h = dlopen("libld-simple.so.1", RTLD_NOW);
        if (!h || dlclose(h)) ok = false;
        h = dlopen("libld-missing.so.1", RTLD_NOW);
        if (h) { ok = false; dlclose(h); } dlerror();
    }
    gz_sysinfo(&b);
    check(ok && stats().mappings == before.mappings, "repeated load/failure/unload reuses address space");
    check(a.free_pages == b.free_pages, "repeated loads return physical pages and metadata");
}
int main(void)
{
    puts("ldtest: starting");
    basic(); scope_and_cycles(); concurrent(); shared(); files(); malformed(); out_of_memory(); repeated();
    printf("ldtest: %s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
