/* Userspace PE loader. Every imported function gets a tiny MIPS tail-call
 * bridge: Microsoft's caller may use any register for jalr, whereas GCC's
 * O32 PIC prologue requires the destination in t9 to establish gp. */
#include "runtime.h"
#include "ordinals.h"
#include <glaze/vm.h>

struct ce_module ce_main;
#define CE_MAX_LIBRARIES 16
static struct ce_module libraries[CE_MAX_LIBRARIES];
static char library_names[CE_MAX_LIBRARIES][32];
static bool library_initialized[CE_MAX_LIBRARIES];
static unsigned library_count;
static uint32_t next_dll_base = 0x02000000u;
static char *ordinals;
static uint32_t ordinal_size;
static uint32_t *bridges;
static unsigned bridge_count, published;
#define CE_BRIDGE_BASE 0x0e000000u
#define CE_BRIDGE_BYTES 65536u
#define CE_BRIDGE_WORDS 8u

void ce_fatal(const char *reason)
{
    printf("wince: error: %s\n", reason);
    gfx_fill(&ui.surf, gfx_r(0, 0, ui.surf.w, ui.surf.h), 0xeeeeee);
    gfx_text(&ui.surf, 12, 18, ui_font(FONT_B14), "Cannot Run WinCE App", 0x222222, 255);
    char line[35];
    for (unsigned pos = 0, y = 54; reason[pos] && y < 280; y += 18) {
        unsigned n = MIN(strlen(reason + pos), sizeof(line) - 1);
        memcpy(line, reason + pos, n);
        line[n] = 0;
        pos += n;
        gfx_text(&ui.surf, 12, (int)y, ui_font(FONT_R11), line, 0x333333, 255);
    }
    gfx_text(&ui.surf, 12, 296, ui_font(FONT_R11), "Press Home to return to Glaze.", 0x333333, 255);
    ui_invalidate_all();
    ui_present();
    for (;;) {
        struct gz_input_event event;
        ui_next_event(&event, 1000);
    }
}

void ce_missing(const char *symbol)
{
    char reason[160];
    snprintf(reason, sizeof(reason), "Unsupported WinCE call: %s", symbol);
    ce_fatal(reason);
}

CW ce_call(CW proc, CW a0, CW a1, CW a2, CW a3)
{
    if (!proc)
        return 0;
    return ((CW(*)(CW, CW, CW, CW))(uintptr_t)proc)(a0, a1, a2, a3);
}

static void publish_bridges(void)
{
    unsigned bytes = ALIGN_UP(bridge_count * CE_BRIDGE_WORDS * 4u, 4096u);
    /* Bridges remain writable until loading (including runtime LoadLibrary)
     * has finished. Never replace an executable bridge while it is in use. */
    if (bytes && gz_vm_publish(CE_BRIDGE_BASE, bytes, 1))
        ce_fatal("Cannot publish WinCE call bridges");
    published = bridge_count;
}

static CW bridge(void *address, const char *missing)
{
    CW fn = WORD(address);
    /* One bridge per service: repeated GetProcAddress calls must not use up
     * the bridge page. */
    if (!missing)
        for (unsigned i = 0; i < bridge_count; i++) {
            const uint32_t *p = bridges + i * CE_BRIDGE_WORDS;
            if (!p[0] && p[2] == (0x3c190000u | (fn >> 16)) && p[3] == (0x37390000u | (fn & 65535u)))
                return WORD(p);
        }
    if (bridge_count >= CE_BRIDGE_BYTES / (CE_BRIDGE_WORDS * 4u))
        ce_fatal("Too many WinCE imports");
    uint32_t *p = bridges + bridge_count++ * CE_BRIDGE_WORDS;
    p[0] = missing ? 0x3c040000u | (WORD(missing) >> 16) : 0;
    p[1] = missing ? 0x34840000u | (WORD(missing) & 65535u) : 0;
    p[2] = 0x3c190000u | (fn >> 16);
    p[3] = 0x37390000u | (fn & 65535u);
    p[4] = 0x03200008u;
    p[5] = p[6] = p[7] = 0;
    return WORD(p);
}

static void *resolve_service(const char *dll, const char *name)
{
    void *fn = NULL;
    if (!strcasecmp(dll, "coredll.dll")) {
        fn = ce_resolve_core(name);
        if (!fn)
            fn = ce_resolve_time(name);
        if (!fn)
            fn = ce_resolve_window(name);
        if (!fn)
            fn = ce_resolve_gdi(name);
    } else if (!strcasecmp(dll, "commctrl.dll") || !strcasecmp(dll, "CSO.dll")) {
        fn = ce_resolve_window(name);
        if (!fn)
            fn = ce_resolve_gdi(name);
    }
    if (!fn)
        fn = ce_resolve_property(name);
    if (!fn)
        fn = ce_resolve_shell(dll, name);
    return fn;
}

static CW resolve_import(const char *dll, const char *name, uint16_t ordinal, void *arg)
{
    (void)arg;
    char ordinal_symbol[256];
    if (!name && ordinals && ce_ordinal_find(ordinals, ordinal_size, dll, ordinal,
            ordinal_symbol, sizeof(ordinal_symbol)))
        name = ordinal_symbol;
    void *fn = name ? resolve_service(dll, name) : NULL;
    if (fn)
        return bridge(fn, NULL);
    CW module = ce_load_library(dll);
    if (module) {
        CW address = name ? ce_find_symbol(module, name) : ce_find_ordinal(module, ordinal);
        if (address)
            return address;
    }
    if (!name) {
        snprintf(ordinal_symbol, sizeof(ordinal_symbol), "#%u", ordinal);
        name = ordinal_symbol;
    }
    char label[128];
    snprintf(label, sizeof(label), "%s!%s", dll, name);
    printf("wince: unresolved import %s\n", label);
    char *copy = strdup(label);
    if (!copy)
        return 0;
    return bridge((void *)ce_missing, copy);
}

/* GetProcAddress on COREDLL: unlike an import, an unimplemented name yields
 * NULL so applications can fall back, as they do for an absent export. */
CW ce_core_proc(const char *name, uint16_t ordinal)
{
    char symbol[256];
    if (!name) {
        if (!ordinals || !ce_ordinal_find(ordinals, ordinal_size, "coredll.dll", ordinal, symbol, sizeof(symbol)))
            return 0;
        name = symbol;
    }
    void *fn = resolve_service("coredll.dll", name);
    if (!fn)
        return 0;
    CW address = bridge(fn, NULL);
    if (published != bridge_count)
        publish_bridges();
    return address;
}

static void map_module(struct ce_module *m, const void *file, uint32_t size)
{
    const char *error = ce_pe_parse(&m->pe, file, size);
    if (error)
        ce_fatal(error);
    if (ce_range(m->pe.base, m->pe.size, 0x0f000000u) && !gz_vm_map(m->pe.base, m->pe.size)) {
        m->base = m->pe.base;
    } else if (m->pe.dll && m->pe.directory[5].size) {
        for (uint32_t cand = ALIGN_UP(next_dll_base, 65536u);
             ce_range(cand, m->pe.size, 0x0df00000u); cand += 65536u) {
            if (!gz_vm_map(cand, m->pe.size)) {
                m->base = cand;
                next_dll_base = ALIGN_UP(cand + m->pe.size, 65536u);
                break;
            }
        }
        if (!m->base)
            ce_fatal("WinCE DLL address space exhausted");
    } else {
        ce_fatal("WinCE preferred address is unavailable");
    }
    m->image = PTR(void, m->base);
    ce_pe_copy(&m->pe, m->image);
    if (m->base != m->pe.base) {
        error = ce_pe_relocate(&m->pe, m->image, m->base);
        if (error)
            ce_fatal(error);
    }
}

static void finish_module(struct ce_module *m, const char *resource)
{
    const char *error = ce_pe_imports(&m->pe, m->image, resolve_import, m);
    if (error)
        ce_fatal(error);
    error = ce_pe_direct_calls(&m->pe, m->image, m->base, resolve_import, m);
    if (error)
        ce_fatal(error);
    if (gz_vm_publish(m->base, ALIGN_UP(m->pe.headers, 4096u), 0))
        ce_fatal("Cannot publish PE header");
    for (unsigned i = 0; i < m->pe.count; i++) {
        struct ce_section *s = &m->pe.section[i];
        uint32_t extent = ALIGN_UP(MAX(s->size, s->bytes), 4096u);
        if (extent && gz_vm_publish(m->base + s->rva, extent, (s->flags >> 31) & 1))
            ce_fatal("Cannot publish PE section");
    }
    printf("wince: loaded %s at %08x, entry %08x\n", resource, m->base, m->base + m->pe.entry);
}

static void load(struct ce_module *m, const char *resource)
{
    uint32_t size;
    const void *file = res_map(resource, &size);
    if (!file)
        ce_fatal("Missing original executable; import a local BE-300 image first");
    map_module(m, file, size);
    finish_module(m, resource);
}

static void normalize_dll_name(const char *raw, char out[32])
{
    const char *base = raw;
    for (const char *p = raw; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    if (strchr(base, '.'))
        strlcpy(out, base, 32);
    else
        snprintf(out, 32, "%s.dll", base);
}

CW ce_module_handle(const char *name)
{
    if (!name || !*name)
        return ce_main.base;
    char base[32];
    normalize_dll_name(name, base);
    if (!strcasecmp(base, "coredll.dll"))
        return 0x73000000u;
    for (unsigned i = 0; i < library_count; i++)
        if (!strcasecmp(library_names[i], base))
            return libraries[i].base;
    return 0;
}

CW ce_load_library(const char *name)
{
    if (!name || !*name) {
        ce_error = 87;
        return 0;
    }
    char base[32];
    normalize_dll_name(name, base);
    for (unsigned i = 0; i < library_count; i++)
        if (!strcasecmp(library_names[i], base))
            return libraries[i].base;
    uint32_t size = 0;
    const void *file = res_map(base, &size);
    char mapped[48];
    if (!file && ce_bundled_res_name(base, mapped))
        file = res_map(mapped, &size);
    if (!file) {
        ce_error = 126;
        return 0;
    }
    if (library_count == ARRAY_SIZE(libraries)) {
        ce_error = 8;
        return 0;
    }
    unsigned i = library_count++;
    strlcpy(library_names[i], base, sizeof(library_names[i]));
    map_module(&libraries[i], file, size);
    if (!libraries[i].pe.dll)
        ce_fatal("Expected a WinCE library");
    finish_module(&libraries[i], base);
    publish_bridges();
    if (!library_initialized[i]) {
        library_initialized[i] = true;
        if (libraries[i].pe.entry &&
            !ce_call(libraries[i].base + libraries[i].pe.entry, libraries[i].base, 1, 0, 0))
            ce_fatal("WinCE library initialization failed");
    }
    return libraries[i].base;
}

CW ce_find_ordinal(CW handle, uint16_t ordinal)
{
    for (unsigned i = 0; i < library_count; i++) {
        struct ce_module *m = &libraries[i];
        if (m->base != handle)
            continue;
        uint8_t *dir = ce_pe_at(&m->pe, m->image, m->pe.directory[0].rva, 40);
        if (!dir)
            return 0;
        uint32_t base_ord = ce_u32(dir + 16), func_count = ce_u32(dir + 20);
        if (ordinal < base_ord || (uint32_t)(ordinal - base_ord) >= func_count || func_count > 65536)
            return 0;
        uint32_t idx = (uint32_t)(ordinal - base_ord);
        uint8_t *fp = ce_pe_at(&m->pe, m->image, ce_u32(dir + 28) + idx * 4, 4);
        if (!fp)
            return 0;
        uint32_t rva = ce_u32(fp);
        if (!rva || !ce_pe_at(&m->pe, m->image, rva, 4))
            return 0;
        return m->base + rva;
    }
    return 0;
}

CW ce_find_symbol(CW handle, const char *name)
{
    for (unsigned i = 0; i < library_count; i++) {
        struct ce_module *m = &libraries[i];
        if (m->base != handle)
            continue;
        uint8_t *dir = ce_pe_at(&m->pe, m->image, m->pe.directory[0].rva, 40);
        if (!dir)
            return 0;
        uint32_t count = ce_u32(dir + 24);
        if (count > 4096)
            return 0;
        for (uint32_t j = 0; j < count; j++) {
            uint8_t *np = ce_pe_at(&m->pe, m->image, ce_u32(dir + 32) + j * 4, 4);
            uint8_t *op = ce_pe_at(&m->pe, m->image, ce_u32(dir + 36) + j * 2, 2);
            if (!np || !op)
                return 0;
            const char *symbol = ce_pe_string(&m->pe, m->image, ce_u32(np));
            if (!symbol || strcmp(name, symbol))
                continue;
            uint32_t ordinal = ce_u16(op);
            if (ordinal >= ce_u32(dir + 20))
                return 0;
            uint8_t *fp = ce_pe_at(&m->pe, m->image, ce_u32(dir + 28) + ordinal * 4, 4);
            if (!fp)
                return 0;
            uint32_t rva = ce_u32(fp);
            if (!ce_pe_at(&m->pe, m->image, rva, 4))
                return 0;
            return m->base + rva;
        }
    }
    return 0;
}

uint32_t ce_module_extent(CW address)
{
    for (unsigned i = 0; i <= library_count; i++) {
        const struct ce_module *m = i ? &libraries[i - 1] : &ce_main;
        if (m->image && address >= m->base && address - m->base < m->pe.size)
            return m->pe.size - (address - m->base);
    }
    return 0;
}

const void *ce_resource(CW handle, uint16_t type, uint16_t id, uint32_t *size)
{
    struct ce_module *m = &ce_main;
    for (unsigned i = 0; i < library_count; i++)
        if (libraries[i].base == handle)
            m = &libraries[i];
    return ce_pe_resource(&m->pe, m->image, type, id, size);
}

void ce_shutdown(void)
{
    static bool closing;
    if (closing)
        return;
    closing = true;
    for (unsigned i = library_count; i > 0; i--) {
        struct ce_module *m = &libraries[i - 1];
        if (m->pe.entry)
            ce_call(m->base + m->pe.entry, m->base, 0, 0, 0);
    }
}

int main(void)
{
    ui_init(SB_STATUS_FULLSCREEN);
    ui_keyboard_init(&ce_keyboard);
    if (ui.surf.h != 320)
        ce_fatal("This Glaze build does not support full-screen compatibility apps");
    ordinal_size = 0;
    const void *map = res_map("ordinals.txt", &ordinal_size);
    if (!map || ordinal_size > 128 * 1024)
        ce_fatal("Missing matching WinCE export names");
    ordinals = malloc(ordinal_size);
    if (!ordinals)
        ce_fatal("Out of memory reading WinCE export names");
    memcpy(ordinals, map, ordinal_size);
    if (gz_vm_map(CE_BRIDGE_BASE, CE_BRIDGE_BYTES))
        ce_fatal("Cannot allocate WinCE call bridges");
    bridges = PTR(uint32_t, CE_BRIDGE_BASE);
    load(&ce_main, "program.exe");
    if (ce_main.pe.dll)
        ce_fatal("Expected a WinCE application");
    if (published != bridge_count)
        publish_bridges();
    static WCHAR command[1];
    printf("wince: entering original executable\n");
    CW result = ce_call(ce_main.base + ce_main.pe.entry, ce_main.base, 0, WORD(command), 1);
    ce_shutdown();
    printf("wince: original executable returned %u\n", result);
    return (int)result;
}
