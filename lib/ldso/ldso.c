/* Glaze ELF interpreter and runtime linker. No shared-libc dependency: startup
 * linking uses only kernel image mappings. Runtime FS reads are libc callbacks.
 * O32 ABI: MIPS GOT tags describe implicit relocations; REL32 with symbol zero
 * adds the load bias, other symbols use S+A. See the MIPS ELF ABI, ch.5.
 */
#include <glaze/sys.h>
#include <glaze/thread.h>
#include <dlfcn.h>
#include <string.h>

#define MODULES 32
#define READY 2
#define INITIALIZING 3
#define CLOSING 4
#define PATH_MAX 192
struct module {
    struct gz_image_info image;
    const struct gz_elf_sym *sym;
    const char *str;
    const uint32_t *hash;
    struct gz_elf_rel *rel;
    uint32_t *got;
    uint32_t symno, strsz, relno, localgot, gotsym, gotno;
    uint32_t init, fini, init_array, init_count, fini_array, fini_count;
    uint32_t needs, binds, roots, handle, state, ordinal;
    uint8_t needed_order[MODULES], needed_count;
    int owner;
    bool global, pinned, initialized, relocated, marked;
    char path[PATH_MAX];
    const char *soname;
};
_Static_assert(sizeof(struct module) <= 4096, "module metadata fits a page");
static struct module *mods[MODULES];
static uint32_t generations, init_order;
static int lock_sem, lock_owner, lock_depth;
static bool collecting, finishing;
static const struct gz_ld_hooks *hooks;
extern uintptr_t ld_call(uintptr_t fn, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d);

static void error(const char *s)
{
    struct gz_tcb *t = gz_self();
    strlcpy(t->dl_error, s, sizeof(t->dl_error)); t->dl_pending = true;
}
static char *ld_error(void)
{
    struct gz_tcb *t = gz_self();
    if (!t->dl_pending) return NULL;
    t->dl_pending = false; return t->dl_error;
}
static void lock(void)
{
    int tid = gz_gettid();
    if (lock_owner == tid) { lock_depth++; return; }
    gz_sem_wait(lock_sem, -1); lock_owner = tid; lock_depth = 1;
}
static void unlock(void)
{
    if (--lock_depth == 0) { lock_owner = 0; gz_sem_post(lock_sem); }
}
static int index_of(const struct module *m)
{
    for (int i = 0; i < MODULES; i++) if (mods[i] == m) return i;
    return -1;
}
static struct module *handle_module(void *h)
{
    uint32_t v = (uint32_t)(uintptr_t)h, slot = v & 63u;
    if (!slot || slot > MODULES) return NULL;
    struct module *m = mods[slot - 1];
    return m && m->state && m->handle == v ? m : NULL;
}
static struct module *new_module(void)
{
    for (int i = 0; i < MODULES; i++) {
        if (!mods[i]) mods[i] = gz_mmap_anon(4096);
        struct module *m = mods[i];
        if (!m) { error("dynamic linker: out of memory"); return NULL; }
        if (!m->state) {
            memset(m, 0, sizeof(*m)); m->state = 1;
            m->handle = (++generations << 6) | (uint32_t)(i + 1); return m;
        }
    }
    error("dynamic linker: too many libraries"); return NULL;
}
static bool contains(struct module *m, uint32_t va, uint32_t n, uint32_t flags)
{
    if (va < m->image.bias) return false;
    uint32_t off = va - m->image.bias;
    for (unsigned i = 0; i < m->image.phnum; i++) {
        const struct gz_elf_phdr *p = &m->image.ph[i];
        if (p->type == GZ_PT_LOAD && (p->flags & flags) == flags && off >= p->vaddr &&
            off - p->vaddr <= p->memsz && n <= p->memsz - (off - p->vaddr)) return true;
    }
    return false;
}
static const char *symbol_name(struct module *m, uint32_t off)
{
    if (off >= m->strsz || !memchr(m->str + off, 0, m->strsz - off)) return NULL;
    return m->str + off;
}
static bool parse(struct module *m)
{
    struct gz_elf_dyn *d = (void *)(uintptr_t)m->image.dynamic;
    uint32_t dynsize = 0, low[38] = {0}, soname = 0;
    bool ended = false;
    for (unsigned i = 0; i < m->image.phnum; i++)
        if (m->image.ph[i].type == GZ_PT_DYNAMIC) dynsize = m->image.ph[i].memsz;
    if (!d || ((uint32_t)d & 3u) || !contains(m, (uint32_t)d, dynsize, GZ_PF_R)) goto bad;
    for (uint32_t i = 0; i < dynsize / sizeof(*d); i++) {
        uint32_t tag = d[i].tag, val = d[i].val;
        if (!tag) { ended = true; break; }
        if (tag < ARRAY_SIZE(low) && tag != 1) low[tag] = val;
        switch (tag) {
        case 14: soname = val; break;
        case 22: case 0x6ffffff0u: case 0x6ffffffcu: case 0x6ffffffeu: goto unsupported;
        case 0x7000000a: m->localgot = val; break;
        case 0x70000011: m->symno = val; break;
        case 0x70000013: m->gotsym = val; break;
        }
    }
    if (!ended || low[11] != sizeof(struct gz_elf_sym) || !low[4] || !low[5] || !low[6] ||
        ((low[4] | low[6] | low[3] | low[17] | low[25] | low[26]) & 3u) ||
        !low[10] || !m->symno || m->symno > 65536u || (low[30] & 4) || low[7] || low[8] ||
        low[20] || low[23]) goto unsupported; /* RELA / PLT / text relocations */
    uint32_t b = m->image.bias;
    m->str = (void *)(b + low[5]); m->strsz = low[10]; m->sym = (void *)(b + low[6]);
    m->hash = (void *)(b + low[4]);
    if (!contains(m, (uint32_t)m->str, m->strsz, GZ_PF_R) ||
        !contains(m, (uint32_t)m->sym, m->symno * sizeof(*m->sym), GZ_PF_R) ||
        !contains(m, (uint32_t)m->hash, 8, GZ_PF_R) || !m->hash[0] ||
        m->hash[0] > 65536u || m->hash[1] != m->symno ||
        !contains(m, (uint32_t)m->hash, (2 + m->hash[0] + m->symno) * 4, GZ_PF_R)) goto bad;
    for (uint32_t i = 0; i < m->symno; i++) {
        const struct gz_elf_sym *s = &m->sym[i];
        if (!symbol_name(m, s->name) || (s->info & 15) == 6) goto unsupported; /* STT_TLS */
        if (s->shndx && s->shndx != 0xfff1 && !contains(m, b + s->value, s->size, GZ_PF_R)) goto bad;
    }
    if (soname && !(m->soname = symbol_name(m, soname))) goto bad;
    if (low[3]) {
        if (m->localgot < 2 || m->gotsym > m->symno || m->localgot > 65536u) goto bad;
        m->gotno = m->localgot + m->symno - m->gotsym;
        m->got = (void *)(b + low[3]);
        if (!contains(m, (uint32_t)m->got, m->gotno * 4, GZ_PF_W)) goto bad;
    }
    if (low[18]) {
        if (!low[17] || low[19] != sizeof(struct gz_elf_rel) || low[18] % sizeof(struct gz_elf_rel)) goto bad;
        m->rel = (void *)(b + low[17]); m->relno = low[18] / sizeof(struct gz_elf_rel);
        if (!contains(m, (uint32_t)m->rel, low[18], GZ_PF_R)) goto bad;
    }
    m->init = low[12] ? b + low[12] : 0; m->fini = low[13] ? b + low[13] : 0;
    m->init_array = low[25] ? b + low[25] : 0; m->init_count = low[27] / 4;
    m->fini_array = low[26] ? b + low[26] : 0; m->fini_count = low[28] / 4;
    if ((m->init && !contains(m, m->init, 4, GZ_PF_X)) || (m->fini && !contains(m, m->fini, 4, GZ_PF_X)) ||
        low[27] % 4 || low[28] % 4 ||
        (m->init_count && !contains(m, m->init_array, low[27], GZ_PF_R)) ||
        (m->fini_count && !contains(m, m->fini_array, low[28], GZ_PF_R))) goto bad;
    return true;
unsupported: error("dynamic linker: unsupported ELF ABI or relocation format"); return false;
bad: error("dynamic linker: malformed ELF dynamic tables"); return false;
}

static uint32_t elf_hash(const char *name)
{
    uint32_t h = 0;
    while (*name) { h = (h << 4) + (uint8_t)*name++; uint32_t g = h & 0xf0000000u; h ^= g >> 24; h &= ~g; }
    return h;
}
static const struct gz_elf_sym *lookup(struct module *m, const char *name)
{
    uint32_t n = m->hash[2 + elf_hash(name) % m->hash[0]];
    const uint32_t *chains = m->hash + 2 + m->hash[0];
    for (uint32_t limit = 0; n && limit++ < m->symno; n = chains[n]) {
        if (n >= m->symno) return NULL;
        const struct gz_elf_sym *s = &m->sym[n];
        unsigned bind = s->info >> 4, visibility = s->other & 3;
        if (s->shndx && (bind == 1 || bind == 2) && visibility != 1 && visibility != 2 &&
            !strcmp(m->str + s->name, name)) return s;
    }
    return NULL;
}
static uintptr_t value(struct module *m, const struct gz_elf_sym *s)
{ return s->shndx == 0xfff1 ? s->value : m->image.bias + s->value; }
static uint32_t closure(uint32_t bits)
{
    for (;;) {
        uint32_t prev = bits;
        for (int i = 0; i < MODULES; i++) if (bits & (1u << i)) bits |= mods[i]->needs;
        if (prev == bits) return bits;
    }
}
static void scope_walk(int root, uint32_t scope, uint32_t *seen, int *order, int *count)
{
    int queue[MODULES], head = 0, tail = 0;
    if (root < 0 || !(scope & (1u << root)) || (*seen & (1u << root))) return;
    *seen |= 1u << root; queue[tail++] = root;
    while (head < tail) {
        int i = queue[head++]; order[(*count)++] = i;
        for (unsigned n = 0; n < mods[i]->needed_count; n++) {
            int j = mods[i]->needed_order[n];
            uint32_t bit = 1u << j;
            if ((scope & bit) && !(*seen & bit)) {
                *seen |= bit; queue[tail++] = j;
            }
        }
    }
}
static bool find_symbol(const char *name, uint32_t scope, struct module *root, bool globals,
                        struct module **provider, const struct gz_elf_sym **sym)
{
    int order[MODULES], count = 0;
    uint32_t seen = 0, global = 0;
    if (globals) {
        for (int i = 0; i < MODULES; i++) if (mods[i] && mods[i]->state && mods[i]->global) global |= 1u << i;
        scope_walk(0, global & scope, &seen, order, &count);
        for (;;) {
            int oldest = -1;
            for (int i = 0; i < MODULES; i++)
                if ((global & scope & ~seen & (1u << i)) && (oldest < 0 || mods[i]->handle < mods[oldest]->handle)) oldest = i;
            if (oldest < 0) break;
            scope_walk(oldest, global & scope, &seen, order, &count);
        }
    }
    if (root) scope_walk(index_of(root), scope, &seen, order, &count);
    for (;;) {
        int oldest = -1;
        for (int i = 0; i < MODULES; i++)
            if ((scope & ~seen & (1u << i)) && (oldest < 0 || mods[i]->handle < mods[oldest]->handle)) oldest = i;
        if (oldest < 0) break;
        scope_walk(oldest, scope, &seen, order, &count);
    }
    const struct gz_elf_sym *weak = NULL; struct module *wm = NULL;
    for (int i = 0; i < count; i++) {
        struct module *m = mods[order[i]];
        if (!m->state || m->state == CLOSING) continue;
        const struct gz_elf_sym *s = lookup(m, name);
        if (!s) continue;
        if ((s->info >> 4) == 2) { if (!weak) { weak = s; wm = m; } }
        else { *provider = m; *sym = s; return true; }
    }
    if (weak) { *provider = wm; *sym = weak; return true; }
    return false;
}
static uint32_t global_scope(void)
{
    uint32_t scope = 0;
    for (int i = 0; i < MODULES; i++) if (mods[i] && mods[i]->state && mods[i]->global) scope |= 1u << i;
    return scope;
}
static bool resolve(struct module *from, uint32_t index, uint32_t scope, struct module *root, uintptr_t *out)
{
    if (index >= from->symno) { error("dynamic linker: invalid relocation symbol"); return false; }
    const struct gz_elf_sym *s = &from->sym[index];
    if (s->shndx && ((s->info >> 4) == 0 || (s->other & 3) == 2 || (s->other & 3) == 3)) {
        *out = value(from, s); return true;
    }
    struct module *provider; const struct gz_elf_sym *found;
    if (find_symbol(from->str + s->name, scope, root, true, &provider, &found)) {
        *out = value(provider, found);
        if (provider != from) from->binds |= 1u << index_of(provider);
        return true;
    }
    if ((s->info >> 4) == 2) { *out = 0; return true; }
    error(from->str + s->name); return false;
}
static bool relocate(struct module *m, uint32_t scope, struct module *root)
{
    if (m->relocated) return true;
    if (m->got) {
        for (uint32_t i = 2; i < m->localgot; i++) m->got[i] += m->image.bias;
        for (uint32_t i = m->gotsym; i < m->symno; i++) {
            uintptr_t val;
            if (!resolve(m, i, scope, root, &val)) return false;
            m->got[m->localgot + i - m->gotsym] = (uint32_t)val;
        }
    }
    for (uint32_t i = 0; i < m->relno; i++) {
        const struct gz_elf_rel *r = &m->rel[i];
        uint32_t type = r->info & 255, sym = r->info >> 8;
        if (!type) continue;
        uint32_t addr = m->image.bias + r->offset;
        if ((type != 2 && type != 3) || (addr & 3) || !contains(m, addr, 4, GZ_PF_W)) {
            error("dynamic linker: unsupported or read-only relocation"); return false;
        }
        uintptr_t v = type == 3 ? m->image.bias : 0;
        if (sym && !resolve(m, sym, scope, root, &v)) return false;
        *(uint32_t *)(uintptr_t)addr += (uint32_t)v;
    }
    if (gz_image_seal(m->image.base)) { error("dynamic linker: failed to seal image"); return false; }
    m->relocated = true; return true;
}

static struct module *existing(const char *path)
{
    bool bare = strchr(path, '/') == NULL;
    for (int i = 0; i < MODULES; i++) {
        struct module *m = mods[i];
        if (m && m->state && (!strcmp(path, m->path) || (bare && m->soname && !strcmp(path, m->soname)))) return m;
    }
    return NULL;
}
static void path_join(char *out, const char *prefix, const char *name)
{ strlcpy(out, prefix, PATH_MAX); strlcat(out, name, PATH_MAX); }
static void add_needed(struct module *m, struct module *child)
{
    int i = index_of(child);
    if (!(m->needs & (1u << i))) {
        m->needs |= 1u << i;
        m->needed_order[m->needed_count++] = (uint8_t)i;
    }
}
static struct module *load(const char *path)
{
    struct module *m = existing(path);
    if (m) return m;
    if (!*path || strlen(path) >= PATH_MAX - 32) { error("dynamic linker: invalid library path"); return NULL; }
    m = new_module(); if (!m) return NULL;
    strlcpy(m->path, path, sizeof(m->path));
    bool bare = strchr(path, '/') == NULL;
    int rv = GZ_ENOENT;
    char file[PATH_MAX];
    if (bare && strlen(path) + 4 < 48) {
        path_join(file, "lib/", path);
        rv = gz_image_map(file, 0, GZ_IMAGE_ROMFS, &m->image);
    }
    if (rv == GZ_ENOENT && hooks) {
        if (bare) path_join(file, "/Library/Libraries/", path);
        else strlcpy(file, path, sizeof(file));
        void *bytes = NULL; uint32_t size = 0;
        rv = (int)ld_call((uintptr_t)hooks->read_file, (uintptr_t)file, (uintptr_t)&bytes, (uintptr_t)&size, 0);
        if (!rv) rv = gz_image_map(bytes, size, GZ_IMAGE_BYTES, &m->image);
        if (bytes) ld_call((uintptr_t)hooks->free_file, (uintptr_t)bytes, 0, 0, 0);
    }
    if (rv) { m->state = 0; error(rv == GZ_ENOENT ? "dynamic linker: library not found" : "dynamic linker: invalid image or out of memory"); return NULL; }
    if (!parse(m)) return NULL;
    if (m->soname) {
        struct module *same = existing(m->soname);
        if (same && same != m) {
            gz_image_release(m->image.base); m->state = 0; return same;
        }
    }
    struct gz_elf_dyn *d = (void *)(uintptr_t)m->image.dynamic;
    for (; d->tag; d++) if (d->tag == 1) {
        const char *dep = symbol_name(m, d->val);
        if (!dep) { error("dynamic linker: invalid dependency name"); return NULL; }
        struct module *child = load(dep);
        if (!child || child->state == CLOSING) return NULL;
        add_needed(m, child);
    }
    return m;
}
static void callback(uintptr_t fn)
{
    if (!fn || fn == UINT32_MAX) return;
    /* Constructor/destructor code may call back into the loader. */
    unlock(); ld_call(fn, 0, 0, 0, 0); lock();
}
static bool valid_code(uint32_t address)
{
    if (!address || address == UINT32_MAX) return true;
    for (int i = 0; i < MODULES; i++)
        if (mods[i] && mods[i]->state && contains(mods[i], address, 4, GZ_PF_X)) return true;
    return false;
}
static bool validate_callbacks(struct module *m)
{
    const uint32_t *init = (const void *)(uintptr_t)m->init_array;
    const uint32_t *fini = (const void *)(uintptr_t)m->fini_array;
    for (uint32_t i = 0; i < m->init_count; i++) if (!valid_code(init[i])) goto bad;
    for (uint32_t i = 0; i < m->fini_count; i++) if (!valid_code(fini[i])) goto bad;
    return true;
bad: error("dynamic linker: invalid initializer/finalizer address"); return false;
}
static bool wait_ready(struct module *m);
static bool initialize(struct module *m)
{
    if (m->initialized) return true;
    if (m->state == INITIALIZING) return m->owner == gz_gettid() || wait_ready(m);
    m->state = INITIALIZING; m->owner = gz_gettid();
    for (unsigned n = 0; n < m->needed_count; n++)
        if (!initialize(mods[m->needed_order[n]])) return false;
    callback(m->init);
    uint32_t *init = (void *)(uintptr_t)m->init_array;
    for (uint32_t i = 0; i < m->init_count; i++) {
        callback(init[i]);
    }
    m->initialized = true; m->state = READY; m->owner = 0; m->ordinal = ++init_order;
    return true;
}
static void finalize(struct module *m)
{
    if (!m->initialized) return;
    m->initialized = false;
    uint32_t *fini = (void *)(uintptr_t)m->fini_array;
    for (uint32_t i = m->fini_count; i; i--)
        if (fini[i - 1] && fini[i - 1] != UINT32_MAX && valid_code(fini[i - 1])) callback(fini[i - 1]);
    callback(m->fini);
}
static void collect(void)
{
    if (collecting) return;
    collecting = true;
    for (;;) {
        uint32_t reach = 0;
        for (int i = 0; i < MODULES; i++) {
            struct module *m = mods[i];
            if (m && m->state && (m->pinned || m->roots || m->state == INITIALIZING)) reach |= 1u << i;
        }
        for (;;) {
            uint32_t prev = reach;
            for (int i = 0; i < MODULES; i++) if (reach & (1u << i)) reach |= mods[i]->needs | mods[i]->binds;
            if (prev == reach) break;
        }
        struct module *victim = NULL;
        for (int i = 0; i < MODULES; i++) {
            struct module *m = mods[i];
            if (m && m->state && !(reach & (1u << i)) && (!victim || m->ordinal > victim->ordinal)) victim = m;
        }
        if (!victim) break;
        victim->state = CLOSING; victim->owner = gz_gettid();
        finalize(victim);
        gz_image_release(victim->image.base); victim->state = 0;
    }
    collecting = false;
}
static bool wait_ready(struct module *m)
{
    while (m->state == INITIALIZING && m->owner != gz_gettid()) { unlock(); gz_sleep_ms(1); lock(); }
    if (!m->state || m->state == CLOSING) { error("dynamic linker: library is closing"); return false; }
    return true;
}
static void *ld_open(const char *path, int flags)
{
    lock();
    if (finishing || (flags & ~(RTLD_NOW | RTLD_GLOBAL)) || !(flags & RTLD_NOW)) {
        error("dynamic linker: unsupported mode"); unlock(); return NULL;
    }
    struct module *m;
    for (;;) {
        m = path ? existing(path) : mods[0];
        if (!m || m->state != CLOSING || m->owner == gz_gettid()) break;
        unlock(); gz_sleep_ms(1); lock();
    }
    if (m && m->state == CLOSING) { error("dynamic linker: recursive open during finalization"); unlock(); return NULL; }
    if (!m) m = load(path);
    if (!m) { collect(); unlock(); return NULL; }
    /* Pin before waiting for another thread's constructors: its first caller
     * may close immediately after initialization completes. */
    m->roots++;
    if (!wait_ready(m)) { m->roots--; collect(); unlock(); return NULL; }
    uint32_t group = closure(1u << index_of(m)), scope = global_scope() | group;
    bool ok = true;
    for (int i = 0; i < MODULES && ok; i++) if (group & (1u << i)) ok = relocate(mods[i], scope, m);
    for (int i = 0; i < MODULES && ok; i++) if (group & (1u << i)) ok = validate_callbacks(mods[i]);
    if (ok && (flags & RTLD_GLOBAL))
        for (int i = 0; i < MODULES; i++) if (group & (1u << i)) mods[i]->global = true;
    if (ok) ok = initialize(m);
    if (!ok) { m->roots--; collect(); unlock(); return NULL; }
    void *h = (void *)(uintptr_t)m->handle; unlock(); return h;
}
static void *ld_symbol(void *handle, const char *name)
{
    lock();
    struct module *m = handle ? handle_module(handle) : NULL;
    if (!name || (handle && (!m || !wait_ready(m)))) { error("dynamic linker: invalid symbol handle"); unlock(); return NULL; }
    struct module *provider; const struct gz_elf_sym *s;
    uint32_t scope = handle ? closure(1u << index_of(m)) : global_scope();
    void *ptr = NULL;
    if (find_symbol(name, scope, m, !handle, &provider, &s)) ptr = (void *)value(provider, s);
    else error("dynamic linker: symbol not found");
    unlock(); return ptr;
}
static int ld_close(void *handle)
{
    lock(); struct module *m = handle_module(handle);
    if (!m || !m->roots || !wait_ready(m)) { error("dynamic linker: invalid close handle"); unlock(); return -1; }
    m->roots--; collect(); unlock(); return 0;
}
static void ld_finish(void)
{
    lock();
    if (finishing) { unlock(); return; }
    finishing = true;
    for (;;) {
        struct module *victim = NULL;
        for (int i = 0; i < MODULES; i++) {
            struct module *m = mods[i];
            if (m && m->state && m->initialized && (!victim || m->ordinal > victim->ordinal)) victim = m;
        }
        if (!victim) break;
        finalize(victim);
    }
    unlock();
}
static const struct gz_ld_api api = { ld_open, ld_symbol, ld_close, ld_error, ld_finish };
static void die(void)
{
    const char *s = ld_error(); if (!s) s = "dynamic linker: startup failure";
    gz_log(s, (uint32_t)strlen(s)); gz_log("\n", 1); gz_exit(127);
}
void ld_start(int pid, const struct gz_ld_start *start)
{
    if (!start || start->abi != GZ_LD_ABI || start->pid != (uint32_t)pid) die();
    lock_sem = gz_sem_create(1); if (lock_sem < 0) die();
    lock();
    struct module *main = new_module(); if (!main) die();
    main->image = start->main; main->pinned = true; main->global = true;
    strlcpy(main->path, "<main>", sizeof(main->path));
    if (!parse(main)) die();
    struct gz_elf_dyn *d = (void *)(uintptr_t)main->image.dynamic;
    for (; d->tag; d++) if (d->tag == 1) {
        const char *name = symbol_name(main, d->val);
        struct module *dep = name ? load(name) : NULL;
        if (!dep) die();
        add_needed(main, dep);
    }
    uint32_t group = closure(1u);
    for (int i = 0; i < MODULES; i++) if (group & (1u << i)) mods[i]->global = true;
    for (int i = 0; i < MODULES; i++) if ((group & (1u << i)) && !relocate(mods[i], group, main)) die();
    for (int i = 0; i < MODULES; i++) if ((group & (1u << i)) && !validate_callbacks(mods[i])) die();
    struct module *provider; const struct gz_elf_sym *sym;
    if (!find_symbol("__glaze_ld_init", group, main, true, &provider, &sym)) die();
    ld_call(value(provider, sym), (uintptr_t)&api, (uintptr_t)&hooks, 0, 0);
    if (!initialize(main)) die();
    unlock();
    ld_call(main->image.entry, (uint32_t)pid, 0, 0, 0);
    ld_finish(); gz_exit(0);
}
