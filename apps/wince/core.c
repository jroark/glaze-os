/* WinCE core services used by the original BE-300 applications. */
#include "registry.h"
#include "runtime.h"

CW ce_error;

/* Microsoft CE and GCC O32 use low/high register pairs for these integer
 * helpers. Keep the 64-bit return type so v1 is preserved by the bridge. */
static uint64_t ce_ll_mul(uint32_t alo, uint32_t ahi, uint32_t blo, uint32_t bhi)
{
    return ((uint64_t)ahi << 32 | alo) * ((uint64_t)bhi << 32 | blo);
}
static int64_t ce_ll_div(uint32_t alo, uint32_t ahi, uint32_t blo, uint32_t bhi)
{
    int64_t numerator = (int64_t)((uint64_t)ahi << 32 | alo);
    int64_t denominator = (int64_t)((uint64_t)bhi << 32 | blo);
    if (!denominator)
        ce_fatal("WinCE integer division by zero");
    if (numerator == INT64_MIN && denominator == -1)
        return INT64_MIN;
    return numerator / denominator;
}
static uint64_t ce_ull_div(uint32_t alo, uint32_t ahi, uint32_t blo, uint32_t bhi)
{
    uint64_t denominator = (uint64_t)bhi << 32 | blo;
    if (!denominator)
        ce_fatal("WinCE integer division by zero");
    return ((uint64_t)ahi << 32 | alo) / denominator;
}
static int64_t ce_ll_rem(uint32_t alo, uint32_t ahi, uint32_t blo, uint32_t bhi)
{
    int64_t numerator = (int64_t)((uint64_t)ahi << 32 | alo);
    int64_t denominator = (int64_t)((uint64_t)bhi << 32 | blo);
    if (!denominator)
        ce_fatal("WinCE integer division by zero");
    return denominator == -1 ? 0 : numerator % denominator;
}
static uint64_t ce_ull_rem(uint32_t alo, uint32_t ahi, uint32_t blo, uint32_t bhi)
{
    uint64_t denominator = (uint64_t)bhi << 32 | blo;
    if (!denominator)
        ce_fatal("WinCE integer division by zero");
    return ((uint64_t)ahi << 32 | alo) % denominator;
}
/* Shift helpers take the value pair and the count in a2. */
static uint64_t ce_ll_lshift(uint32_t lo, uint32_t hi, uint32_t count)
{
    return ((uint64_t)hi << 32 | lo) << (count & 63);
}
static int64_t ce_ll_rshift(uint32_t lo, uint32_t hi, uint32_t count)
{
    return (int64_t)((uint64_t)hi << 32 | lo) >> (count & 63);
}
static uint64_t ce_ull_rshift(uint32_t lo, uint32_t hi, uint32_t count)
{
    return ((uint64_t)hi << 32 | lo) >> (count & 63);
}

unsigned ce_wlen(const WCHAR *text)
{
    unsigned n = 0;
    if (text)
        while (text[n])
            n++;
    return n;
}

int ce_wcmp(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)*a - (int)*b;
}

void ce_narrow(char *dst, uint32_t capacity, const WCHAR *src)
{
    uint32_t n = 0;
    if (src)
        for (unsigned i = 0; src[i]; i++) {
            uint32_t c = src[i], bytes = c < 128 ? 1 : c < 2048 ? 2 : 3;
            if (n + bytes >= capacity)
                break;
            if (bytes == 3)
                dst[n++] = (char)(0xe0 | c >> 12);
            if (bytes >= 2)
                dst[n++] = (char)((bytes == 2 ? 0xc0 : 0x80) | ((c >> 6) & (bytes == 2 ? 31 : 63)));
            dst[n++] = (char)((bytes == 1 ? 0 : 0x80) | (c & (bytes == 1 ? 127 : 63)));
        }
    if (capacity)
        dst[n] = 0;
}

void ce_widen(WCHAR *dst, uint32_t capacity, const char *src)
{
    uint32_t n = 0;
    if (src)
        while (*src && n + 1 < capacity) {
            uint32_t c = (uint8_t)*src++;
            if (c >= 0xc0 && c < 0xe0 && (src[0] & 0xc0) == 0x80) {
                c = (c & 31) << 6 | ((uint8_t)*src++ & 63);
            } else if (c >= 0xe0 && c < 0xf0 && src[0] && src[1] && (src[0] & 0xc0) == 0x80 &&
                       (src[1] & 0xc0) == 0x80) {
                c = (c & 15) << 12 | ((uint8_t)src[0] & 63) << 6 | ((uint8_t)src[1] & 63);
                src += 2;
            } else if (c >= 128)
                c = 0xfffd;
            dst[n++] = (WCHAR)c;
        }
    if (capacity)
        dst[n] = 0;
}

static bool ce_canonical_path(char canonical[FS_PATH_MAX], const WCHAR *input)
{
    char raw[FS_PATH_MAX], norm[FS_PATH_MAX];
    ce_narrow(raw, sizeof(raw), input);
    unsigned n = 0;
    for (const char *p = raw; *p && n + 1 < sizeof(norm); p++) {
        char c = *p == '\\' ? '/' : *p;
        if (c == '/' && n > 0 && norm[n - 1] == '/')
            continue;
        norm[n++] = c;
    }
    while (n > 1 && norm[n - 1] == '/')
        n--;
    norm[n] = 0;
    const char *s = norm;
    if (*s == '/')
        s++;
    if (!strncasecmp(s, "Nand Disk", 9) && (!s[9] || s[9] == '/'))
        s += s[9] ? 10 : 9;
    else if (!strncasecmp(s, "Storage Card", 12) && (!s[12] || s[12] == '/'))
        s += s[12] ? 13 : 12;
    char mapped[FS_PATH_MAX];
    if (!strncasecmp(s, "Program Disk", 12) && (!s[12] || s[12] == '/')) {
        snprintf(mapped, sizeof(mapped), "/Program Files%s", s + 12);
    } else if (!strncasecmp(s, "My Documents", 12) && (!s[12] || s[12] == '/')) {
        snprintf(mapped, sizeof(mapped), "/Documents%s", s + 12);
    } else {
        snprintf(mapped, sizeof(mapped), "/%s", s);
    }
    for (const char *p = mapped; *p;) {
        if (*p == '/') {
            p++;
            continue;
        }
        const char *end = strchr(p, '/');
        if (!end)
            end = p + strlen(p);
        if ((end - p == 1 && *p == '.') || (end - p == 2 && p[0] == '.' && p[1] == '.') ||
            memchr(p, ':', end - p)) {
            ce_error = 123;
            return false;
        }
        p = end;
    }
    strlcpy(canonical, mapped, FS_PATH_MAX);
    return true;
}

bool ce_path(char path[FS_PATH_MAX], const WCHAR *input)
{
    char canonical[FS_PATH_MAX];
    if (!ce_canonical_path(canonical, input))
        return false;
    int n;
    if (!strncasecmp(canonical, "/Documents", 10) && (!canonical[10] || canonical[10] == '/'))
        n = snprintf(path, FS_PATH_MAX, "/Documents%s", canonical + 10);
    else
        n = snprintf(path, FS_PATH_MAX, "%s/data/drive%s", res_bundle(), canonical);
    if (n < 0 || n >= FS_PATH_MAX) {
        ce_error = 206;
        return false;
    }
    return true;
}

CE_API(GetLastError)
{
    return ce_error;
}
CE_API(SetLastError)
{
    ce_error = a0;
    return 0;
}
CE_API(LocalAlloc)
{
    return WORD((a0 & 0x40) ? calloc(1, a1) : malloc(a1));
}
CE_API(LocalFree)
{
    free(PTR(void, a0));
    return 0;
}
CE_API(LocalReAlloc)
{
    return WORD(realloc(PTR(void, a0), a1));
}
CE_API(malloc)
{
    return WORD(malloc(a0));
}
CE_API(free)
{
    free(PTR(void, a0));
    return 0;
}
/* Heap ownership matters: freeing an unrelated pointer must not reach libc. */
struct ce_heap_block {
    struct ce_heap_block *next;
    void *data;
};
static struct {
    CW handle;
    struct ce_heap_block *blocks;
} heaps[16];
static CW heap_serial;
CE_API(HeapCreate)
{
    if (a0 & ~1u || a2) {
        ce_error = 50;
        return 0;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(heaps); i++)
        if (!heaps[i].handle)
            return heaps[i].handle = 0x79000000u | ++heap_serial;
    ce_error = 8;
    return 0;
}
CE_API(HeapAlloc)
{
    if (a1 & ~9u) {
        ce_error = 50;
        return 0;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(heaps); i++) {
        if (!a0 || heaps[i].handle != a0)
            continue;
        struct ce_heap_block *b = malloc(sizeof(*b));
        if (!b)
            break;
        b->data = (a1 & 8) ? calloc(1, a2 ? a2 : 1) : malloc(a2 ? a2 : 1);
        if (!b->data) {
            free(b);
            break;
        }
        b->next = heaps[i].blocks;
        heaps[i].blocks = b;
        return WORD(b->data);
    }
    ce_error = 8;
    return 0;
}
CE_API(HeapFree)
{
    for (unsigned i = 0; i < ARRAY_SIZE(heaps); i++) {
        if (!a0 || heaps[i].handle != a0)
            continue;
        if (!a2)
            return 1;
        struct ce_heap_block **link = &heaps[i].blocks;
        while (*link) {
            struct ce_heap_block *b = *link;
            if (WORD(b->data) == a2) {
                *link = b->next;
                free(b->data);
                free(b);
                return 1;
            }
            link = &b->next;
        }
    }
    ce_error = 87;
    return 0;
}
CE_API(HeapDestroy)
{
    for (unsigned i = 0; i < ARRAY_SIZE(heaps); i++) {
        if (!a0 || heaps[i].handle != a0)
            continue;
        while (heaps[i].blocks) {
            struct ce_heap_block *b = heaps[i].blocks;
            heaps[i].blocks = b->next;
            free(b->data);
            free(b);
        }
        heaps[i].handle = 0;
        return 1;
    }
    ce_error = 6;
    return 0;
}
CE_API(strlen)
{
    return strlen(PTR(char, a0));
}
CE_API(mbstowcs)
{
    const uint8_t *s = PTR(uint8_t, a1);
    unsigned n = 0;
    while (s[n] && (!a0 || n < a2)) {
        if (s[n] >= 128)
            return 0xffffffffu;
        if (a0)
            PTR(WCHAR, a0)[n] = s[n];
        n++;
    }
    if (a0 && n < a2)
        PTR(WCHAR, a0)[n] = 0;
    return n;
}
CE_API(wcstombs)
{
    const WCHAR *s = PTR(WCHAR, a1);
    unsigned n = 0;
    while (s[n] && (!a0 || n < a2)) {
        if (s[n] >= 128)
            return 0xffffffffu;
        if (a0)
            PTR(char, a0)[n] = (char)s[n];
        n++;
    }
    if (a0 && n < a2)
        PTR(char, a0)[n] = 0;
    return n;
}
CE_API(wcsrchr)
{
    WCHAR *p = PTR(WCHAR, a0), *found = NULL;
    do {
        if (*p == a1)
            found = p;
    } while (*p++);
    return WORD(found);
}
CE_API(wcsstr)
{
    WCHAR *p = PTR(WCHAR, a0), *q = PTR(WCHAR, a1);
    unsigned n = ce_wlen(q);
    for (; *p; p++) {
        unsigned i = 0;
        while (i < n && p[i] && p[i] == q[i])
            i++;
        if (i == n)
            return WORD(p);
    }
    return n ? 0 : WORD(p);
}
static CW wcasecmp(CW left, CW right, unsigned n)
{
    const WCHAR *a = PTR(WCHAR, left), *b = PTR(WCHAR, right);
    for (unsigned i = 0; i < n; i++) {
        unsigned x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y)
            return x - y;
        if (!x)
            break;
    }
    return 0;
}
CE_API(_wcsicmp)
{
    return wcasecmp(a0, a1, 0xffffffffu);
}
CE_API(_wcsnicmp)
{
    return wcasecmp(a0, a1, a2);
}
CE_API(_wtol)
{
    const WCHAR *p = PTR(WCHAR, a0);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    bool negative = *p == '-';
    if (*p == '-' || *p == '+')
        p++;
    uint32_t value = 0;
    while (*p >= '0' && *p <= '9')
        value = value * 10u + *p++ - '0';
    return negative ? 0u - value : value;
}
CE_API(memcpy)
{
    return WORD(memcpy(PTR(void, a0), PTR(void, a1), a2));
}
CE_API(memmove)
{
    return WORD(memmove(PTR(void, a0), PTR(void, a1), a2));
}
CE_API(memset)
{
    return WORD(memset(PTR(void, a0), (int)a1, a2));
}
CE_API(memcmp)
{
    return (CW)memcmp(PTR(void, a0), PTR(void, a1), a2);
}
CE_API(wcslen)
{
    return ce_wlen(PTR(WCHAR, a0));
}
CE_API(wcscmp)
{
    return (CW)ce_wcmp(PTR(WCHAR, a0), PTR(WCHAR, a1));
}
CE_API(wcsncmp)
{
    const WCHAR *a = PTR(WCHAR, a0), *b = PTR(WCHAR, a1);
    for (unsigned i = 0; i < a2; i++) {
        if (a[i] != b[i] || !a[i])
            return (CW)((int)a[i] - (int)b[i]);
    }
    return 0;
}
CE_API(wcscpy)
{
    memcpy(PTR(void, a0), PTR(void, a1), (ce_wlen(PTR(WCHAR, a1)) + 1) * 2);
    return a0;
}
CE_API(wcsncpy)
{
    WCHAR *dst = PTR(WCHAR, a0);
    const WCHAR *src = PTR(WCHAR, a1);
    unsigned i = 0;
    for (; i < a2 && src[i]; i++)
        dst[i] = src[i];
    for (; i < a2; i++)
        dst[i] = 0;
    return a0;
}
CE_API(wcscat)
{
    memcpy(PTR(WCHAR, a0) + ce_wlen(PTR(WCHAR, a0)), PTR(void, a1), (ce_wlen(PTR(WCHAR, a1)) + 1) * 2);
    return a0;
}
CE_API(wcschr)
{
    WCHAR *p = PTR(WCHAR, a0);
    while (*p && *p != a1)
        p++;
    return *p == a1 ? WORD(p) : 0;
}
CE_API(lstrcmpW)
{
    return (CW)ce_wcmp(PTR(WCHAR, a0), PTR(WCHAR, a1));
}
CE_API(lstrcmpiW)
{
    char a[512], b[512];
    ce_narrow(a, sizeof(a), PTR(WCHAR, a0));
    ce_narrow(b, sizeof(b), PTR(WCHAR, a1));
    return (CW)strcasecmp(a, b);
}
CE_API(lstrlenW)
{
    return ce_wlen(PTR(WCHAR, a0));
}
CE_API(lstrcpyW)
{
    memcpy(PTR(void, a0), PTR(void, a1), (ce_wlen(PTR(WCHAR, a1)) + 1) * 2);
    return a0;
}
CE_API(ExitThread)
{
    ce_shutdown();
    gz_exit((int)a0);
    return 0;
}
CE_API(ExitProcess)
{
    ce_shutdown();
    gz_exit((int)a0);
    return 0;
}
CE_API(TerminateProcess)
{
    /* WinCE's current-process pseudo handle, used by its CRT exit thunk. */
    if (a0 != 0x42) {
        ce_error = 5;
        return 0;
    }
    ce_shutdown();
    gz_exit((int)a1);
    return 1;
}
CE_API(Sleep)
{
    gz_sleep_ms(a0);
    return 0;
}
CE_API(GetTickCount)
{
    return gz_time_ms();
}
CE_API(GetLocalTime)
{
    struct gz_datetime dt;
    if (!a0 || gz_rtc_get(&dt))
        return 0;
    uint16_t *out = PTR(uint16_t, a0);
    out[0] = dt.year;
    out[1] = dt.month;
    out[2] = dt.weekday;
    out[3] = dt.day;
    out[4] = dt.hour;
    out[5] = dt.minute;
    out[6] = dt.second;
    out[7] = 0;
    return 0;
}
CE_API(GetModuleFileNameW)
{
    if ((a0 && a0 != ce_main.base) || !a1 || !a2) {
        ce_error = 87;
        return 0;
    }
    uint32_t length;
    const char *source = res_map("source.txt", &length);
    const char *fallback = "\\Program Files\\program.exe";
    if (!source) {
        source = fallback;
        length = strlen(fallback);
    }
    if (length > 255 || memchr(source, 0, length)) {
        ce_error = 87;
        return 0;
    }
    if (a2 <= length) {
        ce_error = 122;
        return 0;
    }
    char name[256];
    memcpy(name, source, length);
    name[length] = 0;
    ce_widen(PTR(WCHAR, a1), a2, name);
    return length;
}

/* CE's wide printf uses 16-bit strings. Formatting is bounded internally;
 * unsupported conversions abort instead of pretending formatting succeeded. */
CE_API(wsprintfW)
{
    CW args[] = {a2, a3, a4, a5, a6, a7, a8, a9, a10, a11};
    WCHAR *out = PTR(WCHAR, a0), *fmt = PTR(WCHAR, a1);
    unsigned pos = 0, arg = 0;
    while (*fmt && pos < 1023) {
        if (*fmt != '%') {
            out[pos++] = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            out[pos++] = *fmt++;
            continue;
        }
        bool zero = *fmt == '0';
        unsigned width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = MIN(64u, width * 10u + (uint32_t)(*fmt++ - '0'));
        if (*fmt == 'l')
            fmt++;
        if (arg == ARRAY_SIZE(args))
            ce_missing("wsprintfW: too many arguments");
        CW value = args[arg++];
        WCHAR spec = *fmt++;
        if (spec == 's') {
            WCHAR *s = PTR(WCHAR, value);
            while (s && *s && pos < 1023)
                out[pos++] = *s++;
        } else if (spec == 'c')
            out[pos++] = (WCHAR)value;
        else {
            char buffer[80], format[16];
            if (spec != 'd' && spec != 'i' && spec != 'u' && spec != 'x' && spec != 'X')
                ce_missing("wsprintfW conversion");
            snprintf(format, sizeof(format), "%%%s%u%c", zero ? "0" : "", width, (char)spec);
            snprintf(buffer, sizeof(buffer), format, value);
            for (char *s = buffer; *s && pos < 1023;)
                out[pos++] = (WCHAR)(uint8_t)*s++;
        }
    }
    out[pos] = 0;
    return pos;
}

CE_API(MultiByteToWideChar)
{
    const char *input = PTR(char, a2);
    int length = (int)a3;
    if (a0 != 0 && a0 != 1252 && a0 != 65001) {
        ce_error = 87;
        return 0;
    }
    if (!input || !length) {
        ce_error = 87;
        return 0;
    }
    if (length < 0)
        length = (int)strlen(input) + 1;
    if (!a4)
        return (CW)length;
    if (a5 < (CW)length) {
        ce_error = 122;
        return 0;
    }
    WCHAR *out = PTR(WCHAR, a4);
    if (a0 == 65001) {
        char *copy = malloc((unsigned)length + 1);
        if (!copy)
            return 0;
        memcpy(copy, input, (unsigned)length);
        copy[length] = 0;
        ce_widen(out, a5, copy);
        CW count = ce_wlen(out) + (input[length - 1] == 0);
        free(copy);
        return count;
    }
    for (int i = 0; i < length; i++)
        out[i] = (uint8_t)input[i];
    return (CW)length;
}
CE_API(WideCharToMultiByte)
{
    const WCHAR *input = PTR(WCHAR, a2);
    int length = (int)a3;
    if (a0 != 0 && a0 != 1252 && a0 != 65001) {
        ce_error = 87;
        return 0;
    }
    if (!input || !length) {
        ce_error = 87;
        return 0;
    }
    if (length < 0)
        length = (int)ce_wlen(input) + 1;
    if (!a4)
        return (CW)length * (a0 == 65001 ? 3 : 1);
    if (a0 == 65001) {
        ce_narrow(PTR(char, a4), a5, input);
        return (CW)strlen(PTR(char, a4)) + (input[length - 1] == 0);
    }
    if (a5 < (CW)length) {
        ce_error = 122;
        return 0;
    }
    char *out = PTR(char, a4);
    bool replaced = false;
    for (int i = 0; i < length; i++) {
        if (input[i] > 255)
            replaced = true;
        out[i] = input[i] > 255 ? '?' : (char)input[i];
    }
    if (a7)
        *PTR(CW, a7) = replaced;
    return (CW)length;
}
CE_API(CompareStringW)
{
    const WCHAR *s = PTR(WCHAR, a2), *t = PTR(WCHAR, a4);
    unsigned n = (int)a3 < 0 ? ce_wlen(s) : a3, m = (int)a5 < 0 ? ce_wlen(t) : a5;
    for (unsigned i = 0; i < MIN(n, m); i++) {
        WCHAR x = s[i], y = t[i];
        if (a1 & 1) {
            if (x >= 'A' && x <= 'Z')
                x += 32;
            if (y >= 'A' && y <= 'Z')
                y += 32;
        }
        if (x != y)
            return x < y ? 1 : 3;
    }
    return n == m ? 2 : n < m ? 1 : 3;
}

/* Registry handles are separate from persistent keys and can be closed/reused.
 */
static struct ce_registry registry;
static struct {
    CW handle;
    char path[256];
} reg_handles[64];
static CW reg_serial;
static bool registry_loaded;
static void registry_file(char path[FS_PATH_MAX])
{
    snprintf(path, FS_PATH_MAX, "%s/data/registry", res_bundle());
}
static const char *reg_root(CW handle)
{
    if (handle >= 0x80000000u && handle <= 0x80000003u) {
        static const char *const roots[] = {"HKCR", "HKCU", "HKLM", "HKU"};
        return roots[handle - 0x80000000u];
    }
    for (unsigned i = 0; i < ARRAY_SIZE(reg_handles); i++)
        if (handle && reg_handles[i].handle == handle)
            return reg_handles[i].path;
    return NULL;
}
static void reg_load(void)
{
    if (registry_loaded)
        return;
    registry_loaded = true;
    char path[FS_PATH_MAX];
    registry_file(path);
    struct fs_stat st;
    if (fs_stat(path, &st)) {
        uint32_t seed_size = 0;
        const void *seed = res_map("registry", &seed_size);
        if (seed && (seed_size > CE_REG_FILE_MAX || !ce_reg_decode(&registry, seed, seed_size)))
            ce_fatal("Invalid WinCE initial registry");
        return;
    }
    if (st.size > CE_REG_FILE_MAX || st.size < 8)
        ce_fatal("Invalid WinCE registry file");
    uint8_t *data = malloc(st.size);
    if (!data || fs_read_file(path, data, st.size) != (int)st.size)
        ce_fatal("Cannot read WinCE registry");
    if (!ce_reg_decode(&registry, data, st.size))
        ce_fatal("Invalid WinCE registry stream");
    free(data);
}
static bool reg_save(void)
{
    char path[FS_PATH_MAX], temporary[FS_PATH_MAX], folder[FS_PATH_MAX];
    registry_file(path);
    snprintf(folder, sizeof(folder), "%s/data", res_bundle());
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (fs_mkdirs(folder))
        return false;
    uint32_t size;
    uint8_t *data = ce_reg_encode(&registry, &size);
    if (!data)
        return false;
    int fd = fs_open(temporary, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    bool ok = fd >= 0;
    if (ok) {
        ok = fs_write(fd, data, size) == (int)size;
        if (fs_close(fd))
            ok = false;
    }
    free(data);
    return ok && !fs_rename(temporary, path);
}
uint32_t ce_registry_dword(const char *key, const char *name, uint32_t fallback)
{
    reg_load();
    struct ce_reg_value *v = ce_reg_find(&registry, key, name);
    return v && v->type == 4 && v->size == 4 ? ce_u32(v->bytes) : fallback;
}
bool ce_registry_set_dword(const char *key, const char *name, uint32_t value)
{
    reg_load();
    uint8_t bytes[4];
    ce_put32(bytes, value);
    return !ce_reg_create(&registry, key) && !ce_reg_set(&registry, key, name, 4, bytes, 4) && reg_save();
}
static bool reg_utf8(char *out, unsigned capacity, const WCHAR *in)
{
    unsigned bytes = 0;
    if (in)
        for (unsigned i = 0; in[i]; i++) {
            unsigned ch = in[i];
            bytes += ch < 128 ? 1 : ch < 2048 ? 2 : 3;
            if (bytes >= capacity)
                return false;
        }
    ce_narrow(out, capacity, in);
    return true;
}
static CW reg_open(CW root, CW subkey, CW result, CW disposition, bool create)
{
    reg_load();
    const char *parent = reg_root(root);
    if (!parent || !result)
        return 6;
    char sub[256], path[256];
    if (!reg_utf8(sub, sizeof(sub), PTR(WCHAR, subkey)))
        return 87;
    if (snprintf(path, sizeof(path), "%s%s%s", parent, *sub ? "\\" : "", sub) >= (int)sizeof(path))
        return 87;
    bool existed = ce_reg_exists(&registry, path);
    if (!create && !existed)
        return 2;
    unsigned i;
    for (i = 0; i < ARRAY_SIZE(reg_handles) && reg_handles[i].handle; i++)
        ;
    if (i == ARRAY_SIZE(reg_handles))
        return 8;
    if (create && !existed) {
        CW error = ce_reg_create(&registry, path);
        if (error)
            return error;
        if (!reg_save())
            return 5;
    }
    reg_handles[i].handle = 0x71000000u | (++reg_serial & 0xfffffu);
    strlcpy(reg_handles[i].path, path, sizeof(reg_handles[i].path));
    *PTR(CW, result) = reg_handles[i].handle;
    if (disposition)
        *PTR(CW, disposition) = existed ? 2 : 1;
    return 0;
}
CE_API(RegCreateKeyExW)
{
    /* Legacy apps supply a descriptive class even when they never query it.
     * Retain key creation; this subset returns an empty class on queries. */
    return reg_open(a0, a1, a7, a8, true);
}
CE_API(RegOpenKeyExW)
{
    return reg_open(a0, a1, a4, 0, false);
}
CE_API(RegCloseKey)
{
    if (a0 >= 0x80000000u && a0 <= 0x80000003u)
        return 0;
    for (unsigned i = 0; i < ARRAY_SIZE(reg_handles); i++)
        if (a0 && reg_handles[i].handle == a0) {
            reg_handles[i].handle = 0;
            return 0;
        }
    return 6;
}
static CW reg_data(struct ce_reg_value *v, CW type, CW data, CW length)
{
    if (!v)
        return 2;
    if (!length && data)
        return 87;
    if (type)
        *PTR(CW, type) = v->type;
    CW capacity = length ? *PTR(CW, length) : 0;
    if (length)
        *PTR(CW, length) = v->size;
    if (!data)
        return 0;
    if (capacity < v->size)
        return 234;
    if (v->size)
        memcpy(PTR(void, data), v->bytes, v->size);
    return 0;
}
CE_API(RegQueryValueExW)
{
    reg_load();
    const char *key = reg_root(a0);
    if (!key)
        return 6;
    char name[128];
    if (!reg_utf8(name, sizeof(name), PTR(WCHAR, a1)))
        return 87;
    return reg_data(ce_reg_find(&registry, key, name), a3, a4, a5);
}
CE_API(RegSetValueExW)
{
    reg_load();
    const char *key = reg_root(a0);
    if (!key)
        return 6;
    char name[128];
    if (!reg_utf8(name, sizeof(name), PTR(WCHAR, a1)))
        return 87;
    CW error = ce_reg_set(&registry, key, name, a3, PTR(void, a4), a5);
    return error ? error : reg_save() ? 0 : 5;
}
CE_API(RegDeleteValueW)
{
    reg_load();
    const char *key = reg_root(a0);
    if (!key)
        return 6;
    char name[128];
    if (!reg_utf8(name, sizeof(name), PTR(WCHAR, a1)))
        return 87;
    CW error = ce_reg_remove_value(&registry, key, name);
    return error ? error : reg_save() ? 0 : 5;
}
static CW reg_name(const char *name, CW buffer, CW length)
{
    if (!length)
        return 87;
    WCHAR text[256];
    ce_widen(text, ARRAY_SIZE(text), name);
    unsigned n = ce_wlen(text), capacity = *PTR(CW, length);
    *PTR(CW, length) = n;
    if (!buffer || capacity <= n)
        return 234;
    memcpy(PTR(void, buffer), text, (n + 1) * 2);
    return 0;
}
CE_API(RegEnumKeyExW)
{
    reg_load();
    const char *key = reg_root(a0);
    if (!key)
        return 6;
    const char *name = ce_reg_child(&registry, key, a1);
    if (!name)
        return 259;
    CW error = reg_name(name, a2, a3);
    if (a7)
        memset(PTR(void, a7), 0, 8);
    if (a5 && (!a6 || !*PTR(CW, a6)))
        return 234;
    if (a5)
        *PTR(WCHAR, a5) = 0;
    if (a6)
        *PTR(CW, a6) = 0;
    return error;
}
CE_API(RegEnumValueW)
{
    reg_load();
    const char *key = reg_root(a0);
    if (!key)
        return 6;
    struct ce_reg_value *v = ce_reg_value_at(&registry, key, a1);
    if (!v)
        return 259;
    CW error = reg_name(v->name, a2, a3);
    CW data_error = reg_data(v, a5, error ? 0 : a6, a7);
    return error ? error : data_error;
}
CE_API(RegQueryInfoKeyW)
{
    reg_load();
    const char *key = reg_root(a0);
    if (!key)
        return 6;
    if (a1 && (!a2 || !*PTR(CW, a2)))
        return 234;
    if (a1)
        *PTR(WCHAR, a1) = 0;
    if (a2)
        *PTR(CW, a2) = 0;
    CW children = 0, max_key = 0, values = 0, max_name = 0, max_data = 0;
    const char *name;
    WCHAR text[256];
    while ((name = ce_reg_child(&registry, key, children))) {
        ce_widen(text, ARRAY_SIZE(text), name);
        max_key = MAX(max_key, ce_wlen(text));
        children++;
    }
    struct ce_reg_value *v;
    while ((v = ce_reg_value_at(&registry, key, values))) {
        ce_widen(text, ARRAY_SIZE(text), v->name);
        max_name = MAX(max_name, ce_wlen(text));
        max_data = MAX(max_data, v->size);
        values++;
    }
    if (a4)
        *PTR(CW, a4) = children;
    if (a5)
        *PTR(CW, a5) = max_key;
    if (a6)
        *PTR(CW, a6) = 0;
    if (a7)
        *PTR(CW, a7) = values;
    if (a8)
        *PTR(CW, a8) = max_name;
    if (a9)
        *PTR(CW, a9) = max_data;
    if (a10)
        *PTR(CW, a10) = 0;
    if (a11)
        memset(PTR(void, a11), 0, 8);
    return 0;
}
CE_API(RegDeleteKeyW)
{
    reg_load();
    const char *key = reg_root(a0);
    if (!key)
        return 6;
    if (!a1 || !*PTR(WCHAR, a1))
        return 87;
    char name[256], path[256];
    if (!reg_utf8(name, sizeof(name), PTR(WCHAR, a1)))
        return 87;
    if (snprintf(path, sizeof(path), "%s\\%s", key, name) >= (int)sizeof(path))
        return 87;
    for (unsigned i = 0; i < ARRAY_SIZE(reg_handles); i++)
        if (reg_handles[i].handle && !strcasecmp(reg_handles[i].path, path))
            return 5;
    CW error = ce_reg_delete(&registry, path);
    return error ? error : reg_save() ? 0 : 5;
}

/* Bundled CBEA files (res/files.txt: "<res_entry> <canonical_path>\n"). */
struct bundled_entry {
    char res[48];
    char path[FS_PATH_MAX];
};
static struct bundled_entry bundled_files[64];
static unsigned bundled_count;
static bool bundled_loaded;

static void load_bundled_files(void)
{
    if (bundled_loaded)
        return;
    bundled_loaded = true;
    uint32_t size = 0;
    const char *text = res_map("files.txt", &size);
    if (!text || size > 32768)
        return;
    for (uint32_t pos = 0; pos < size && bundled_count < ARRAY_SIZE(bundled_files);) {
        uint32_t end = pos;
        while (end < size && text[end] != '\n')
            end++;
        uint32_t line_end = (end > pos && text[end - 1] == '\r') ? end - 1 : end;
        const char *sp = memchr(text + pos, ' ', line_end - pos);
        if (sp) {
            size_t rlen = (size_t)(sp - (text + pos));
            size_t plen = (size_t)((text + line_end) - (sp + 1));
            if (rlen > 0 && rlen < sizeof(bundled_files[0].res) && plen > 0 &&
                plen < sizeof(bundled_files[0].path)) {
                struct bundled_entry *b = &bundled_files[bundled_count++];
                memcpy(b->res, text + pos, rlen);
                b->res[rlen] = 0;
                memcpy(b->path, sp + 1, plen);
                b->path[plen] = 0;
            }
        }
        pos = end + 1;
    }
}

bool ce_bundled_res_name(const char *leaf, char out[48])
{
    load_bundled_files();
    for (unsigned i = 0; i < bundled_count; i++) {
        const char *slash = strrchr(bundled_files[i].path, '/');
        const char *base = slash ? slash + 1 : bundled_files[i].path;
        if (!strcasecmp(bundled_files[i].res, leaf) || !strcasecmp(base, leaf) ||
            !strcasecmp(bundled_files[i].path, leaf)) {
            strlcpy(out, bundled_files[i].res, 48);
            return true;
        }
    }
    return false;
}

static bool ce_bundled_file(const char *canonical, char res_path[FS_PATH_MAX])
{
    load_bundled_files();
    if (!res_bundle()[0])
        return false;
    for (unsigned i = 0; i < bundled_count; i++) {
        if (!strcasecmp(bundled_files[i].path, canonical)) {
            int n = snprintf(res_path, FS_PATH_MAX, "%s/res/%s", res_bundle(), bundled_files[i].res);
            return n > 0 && n < FS_PATH_MAX;
        }
    }
    return false;
}

static bool ce_bundled_dir(const char *canonical)
{
    load_bundled_files();
    if (!strcmp(canonical, "/"))
        return bundled_count > 0;
    size_t len = strlen(canonical);
    for (unsigned i = 0; i < bundled_count; i++) {
        if (!strncasecmp(bundled_files[i].path, canonical, len) && bundled_files[i].path[len] == '/')
            return true;
    }
    return false;
}

static bool copy_bundled_to_drive(const char *res_path, const char *drive_path, bool copy_bytes)
{
    char dir[FS_PATH_MAX];
    strlcpy(dir, drive_path, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        if (fs_mkdirs(dir))
            return false;
    }
    if (!copy_bytes)
        return true;
    int in = fs_open(res_path, FS_O_READ);
    if (in < 0)
        return false;
    int out = fs_open(drive_path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    if (out < 0) {
        fs_close(in);
        return false;
    }
    uint8_t buf[1024];
    int n;
    bool ok = true;
    while ((n = fs_read(in, buf, sizeof(buf))) > 0) {
        if (fs_write(out, buf, (uint32_t)n) != n) {
            ok = false;
            break;
        }
    }
    if (n < 0)
        ok = false;
    fs_close(in);
    if (fs_close(out))
        ok = false;
    return ok;
}

struct file_handle {
    int fd;
    bool active;
    char path[FS_PATH_MAX];
};
static struct file_handle files[32];
static struct file_handle *file_handle(CW id)
{
    return id >= 0x72000000u && id - 0x72000000u < ARRAY_SIZE(files) && files[id - 0x72000000u].active
               ? &files[id - 0x72000000u]
               : NULL;
}
static CW file_error(int error)
{
    ce_error = error == GZ_ENOENT ? 2 : error == GZ_ENOSPC ? 112 : error == GZ_EEXIST ? 80 : 5;
    return 0;
}
struct find_handle {
    bool active, fs_done;
    uint32_t index, bundled_index;
    char path[FS_PATH_MAX], canonical_dir[FS_PATH_MAX], pattern[FS_NAME_MAX];
};
static struct find_handle searches[8];
static bool match_name(const char *pattern, const char *name)
{
    const char *star = NULL, *retry = NULL;
    if (!strcmp(pattern, "*.*"))
        pattern = "*";
    while (*name) {
        if (*pattern == '*') {
            star = ++pattern;
            retry = name;
        } else if (*pattern == '?' || ((*pattern >= 'A' && *pattern <= 'Z' ? *pattern + 32 : *pattern) ==
                                       (*name >= 'A' && *name <= 'Z' ? *name + 32 : *name))) {
            pattern++;
            name++;
        } else if (star) {
            pattern = star;
            name = ++retry;
        } else
            return false;
    }
    while (*pattern == '*')
        pattern++;
    return !*pattern;
}
static CW find_next(struct find_handle *search, CW out)
{
    if (!out) {
        ce_error = 87;
        return 0;
    }
    struct fs_dirent entry;
    while (!search->fs_done) {
        int count = fs_list(search->path, search->index++, &entry, 1);
        if (count <= 0) {
            search->fs_done = true;
            if (count < 0 && !ce_bundled_dir(search->canonical_dir)) {
                ce_error = 3;
                return 0;
            }
            break;
        }
        if (!match_name(search->pattern, entry.name))
            continue;
        /* CE WIN32_FIND_DATA is 560 bytes: dwOID precedes the UTF-16 name
         * at offset 40. The backing FS supplies no timestamps or OIDs. */
        memset(PTR(void, out), 0, 560);
        ce_put32(PTR(void, out), entry.type == FS_DIR ? 16 : 128);
        ce_put32(PTR(uint8_t, out) + 32, entry.size);
        ce_widen(PTR(WCHAR, out + 40), 260, entry.name);
        return 1;
    }
    load_bundled_files();
    size_t dlen = strcmp(search->canonical_dir, "/") ? strlen(search->canonical_dir) : 0;
    while (search->bundled_index < bundled_count) {
        unsigned idx = search->bundled_index++;
        const char *bpath = bundled_files[idx].path;
        if (dlen && (strncasecmp(bpath, search->canonical_dir, dlen) || bpath[dlen] != '/'))
            continue;
        const char *rel = bpath + dlen + 1;
        const char *slash = strchr(rel, '/');
        char child[FS_NAME_MAX];
        size_t clen = slash ? (size_t)(slash - rel) : strlen(rel);
        if (!clen || clen >= sizeof(child))
            continue;
        memcpy(child, rel, clen);
        child[clen] = 0;
        if (!match_name(search->pattern, child))
            continue;
        bool dup = false;
        for (unsigned j = 0; j < idx; j++) {
            const char *prev = bundled_files[j].path;
            if (dlen && (strncasecmp(prev, search->canonical_dir, dlen) || prev[dlen] != '/'))
                continue;
            const char *prel = prev + dlen + 1;
            if (!strncasecmp(prel, child, clen) && (!prel[clen] || prel[clen] == '/')) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        char existing[FS_PATH_MAX];
        struct fs_stat st;
        if (snprintf(existing, sizeof(existing), "%s/%s", search->path, child) < (int)sizeof(existing) &&
            fs_stat(existing, &st) == 0)
            continue;
        uint32_t fsize = 0;
        if (!slash && res_bundle()[0]) {
            char rpath[FS_PATH_MAX];
            snprintf(rpath, sizeof(rpath), "%s/res/%s", res_bundle(), bundled_files[idx].res);
            if (fs_stat(rpath, &st) == 0)
                fsize = st.size;
        }
        memset(PTR(void, out), 0, 560);
        ce_put32(PTR(void, out), slash ? 16 : 128);
        ce_put32(PTR(uint8_t, out) + 32, fsize);
        ce_widen(PTR(WCHAR, out + 40), 260, child);
        return 1;
    }
    ce_error = 18;
    return 0;
}
CE_API(FindFirstFileW)
{
    char path[FS_PATH_MAX], canonical[FS_PATH_MAX];
    if (!a0 || !a1) {
        ce_error = 87;
        return 0xffffffffu;
    }
    if (!ce_canonical_path(canonical, PTR(WCHAR, a0)) || !ce_path(path, PTR(WCHAR, a0)))
        return 0xffffffffu;
    char *last = strrchr(path, '/');
    char *clast = strrchr(canonical, '/');
    if (!last || !clast || strlen(last + 1) >= FS_NAME_MAX) {
        ce_error = 123;
        return 0xffffffffu;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(searches); i++)
        if (!searches[i].active) {
            struct find_handle *search = &searches[i];
            memset(search, 0, sizeof(*search));
            strlcpy(search->pattern, last + 1, sizeof(search->pattern));
            *last = 0;
            if (clast == canonical)
                strlcpy(search->canonical_dir, "/", sizeof(search->canonical_dir));
            else {
                *clast = 0;
                strlcpy(search->canonical_dir, canonical, sizeof(search->canonical_dir));
            }
            strlcpy(search->path, path, sizeof(search->path));
            if (!find_next(search, a1))
                return 0xffffffffu;
            search->active = true;
            return 0x78000000u + i;
        }
    ce_error = 4;
    return 0xffffffffu;
}
CE_API(FindNextFileW)
{
    unsigned i = a0 - 0x78000000u;
    if (i >= ARRAY_SIZE(searches) || !searches[i].active) {
        ce_error = 6;
        return 0;
    }
    return find_next(&searches[i], a1);
}
CE_API(FindClose)
{
    unsigned i = a0 - 0x78000000u;
    if (i >= ARRAY_SIZE(searches) || !searches[i].active) {
        ce_error = 6;
        return 0;
    }
    searches[i].active = false;
    return 1;
}
CE_API(CreateFileW)
{
    char path[FS_PATH_MAX], canonical[FS_PATH_MAX];
    if (!ce_canonical_path(canonical, PTR(WCHAR, a0)) || !ce_path(path, PTR(WCHAR, a0)))
        return 0xffffffffu;
    int flags = (a1 & 0x80000000u ? FS_O_READ : 0) | (a1 & 0x40000000u ? FS_O_WRITE : 0);
    if (a4 == 1)
        flags |= FS_O_CREATE | FS_O_EXCL;
    else if (a4 == 2)
        flags |= FS_O_CREATE | FS_O_TRUNC;
    else if (a4 == 4)
        flags |= FS_O_CREATE;
    else if (a4 == 5)
        flags |= FS_O_TRUNC;
    else if (a4 != 3) {
        ce_error = 87;
        return 0xffffffffu;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(files); i++)
        if (!files[i].active) {
            const char *open_path = path;
            char res_path[FS_PATH_MAX];
            struct fs_stat st;
            if (fs_stat(path, &st) != 0) {
                if (ce_bundled_file(canonical, res_path)) {
                    if (a4 == 1) {
                        ce_error = 80;
                        return 0xffffffffu;
                    }
                    if (!(flags & (FS_O_WRITE | FS_O_TRUNC))) {
                        open_path = res_path;
                        flags = FS_O_READ;
                    } else if (!copy_bundled_to_drive(res_path, path, !(flags & FS_O_TRUNC))) {
                        ce_error = 5;
                        return 0xffffffffu;
                    }
                } else if (flags & FS_O_CREATE) {
                    char parent[FS_PATH_MAX];
                    strlcpy(parent, canonical, sizeof(parent));
                    char *slash = strrchr(parent, '/');
                    if (slash) {
                        if (slash == parent)
                            parent[1] = 0;
                        else
                            *slash = 0;
                        if (ce_bundled_dir(parent))
                            copy_bundled_to_drive("", path, false);
                    }
                }
            }
            int fd = fs_open(open_path, flags);
            if (fd < 0) {
                file_error(fd);
                return 0xffffffffu;
            }
            files[i].fd = fd;
            files[i].active = true;
            strlcpy(files[i].path, open_path, sizeof(files[i].path));
            ce_error = 0;
            return 0x72000000u + i;
        }
    ce_error = 4;
    return 0xffffffffu;
}
CE_API(CloseHandle)
{
    struct file_handle *f = file_handle(a0);
    if (!f) {
        ce_error = 6;
        return 0;
    }
    int rv = fs_close(f->fd);
    f->active = false;
    return rv < 0 ? file_error(rv) : 1;
}
CE_API(ReadFile)
{
    struct file_handle *f = file_handle(a0);
    if (!f || a4) {
        ce_error = 87;
        return 0;
    }
    int n = fs_read(f->fd, PTR(void, a1), a2);
    if (!strncmp(f->path, "/Documents/", 11))
        printf("wince: read %s (%d bytes)\n", f->path, n);
    if (a3)
        *PTR(CW, a3) = n < 0 ? 0 : (CW)n;
    return n < 0 ? file_error(n) : 1;
}
CE_API(WriteFile)
{
    struct file_handle *f = file_handle(a0);
    if (!f || a4) {
        ce_error = 87;
        return 0;
    }
    int n = fs_write(f->fd, PTR(void, a1), a2);
    if (!strncmp(f->path, "/Documents/", 11))
        printf("wince: wrote %s (%d bytes)\n", f->path, n);
    if (a3)
        *PTR(CW, a3) = n < 0 ? 0 : (CW)n;
    return n < 0 ? file_error(n) : 1;
}
CE_API(GetFileSize)
{
    struct file_handle *f = file_handle(a0);
    struct fs_stat st;
    if (!f) {
        ce_error = 6;
        return 0xffffffffu;
    }
    if (fs_stat(f->path, &st)) {
        ce_error = 5;
        return 0xffffffffu;
    }
    if (a1)
        *PTR(CW, a1) = 0;
    return st.size;
}
CE_API(GetFileAttributesW)
{
    char path[FS_PATH_MAX], canonical[FS_PATH_MAX], res_path[FS_PATH_MAX];
    struct fs_stat st;
    if (!ce_canonical_path(canonical, PTR(WCHAR, a0)) || !ce_path(path, PTR(WCHAR, a0)))
        return 0xffffffffu;
    if (!fs_stat(path, &st))
        return st.type == FS_DIR ? 16 : 0x80;
    if (ce_bundled_file(canonical, res_path) && !fs_stat(res_path, &st))
        return 0x80;
    if (ce_bundled_dir(canonical))
        return 16;
    ce_error = 2;
    return 0xffffffffu;
}
CE_API(CreateDirectoryW)
{
    char path[FS_PATH_MAX];
    if (!ce_path(path, PTR(WCHAR, a0)))
        return 0;
    struct fs_stat st;
    if (!fs_stat(path, &st)) {
        ce_error = 183;
        return 0;
    }
    int rv = fs_mkdirs(path);
    return rv < 0 ? file_error(rv) : 1;
}
CE_API(RemoveDirectoryW)
{
    char path[FS_PATH_MAX];
    if (!ce_path(path, PTR(WCHAR, a0)))
        return 0;
    int rv = fs_remove(path);
    return rv < 0 ? file_error(rv) : 1;
}
CE_API(MoveFileW)
{
    char src[FS_PATH_MAX], dst[FS_PATH_MAX];
    if (!ce_path(src, PTR(WCHAR, a0)) || !ce_path(dst, PTR(WCHAR, a1)))
        return 0;
    int rv = fs_rename(src, dst);
    return rv < 0 ? file_error(rv) : 1;
}
CE_API(DeleteFileW)
{
    char path[FS_PATH_MAX];
    if (!ce_path(path, PTR(WCHAR, a0)))
        return 0;
    int rv = fs_remove(path);
    return rv < 0 ? file_error(rv) : 1;
}
CE_API(SetFilePointer)
{
    struct file_handle *f = file_handle(a0);
    if (!f || (a2 && *PTR(CW, a2)) || a3 > 2) {
        ce_error = 87;
        return 0xffffffffu;
    }
    int rv = fs_seek(f->fd, (int32_t)a1, (int)a3);
    if (rv < 0) {
        file_error(rv);
        return 0xffffffffu;
    }
    return (CW)rv;
}
CE_API(SetEndOfFile)
{
    struct file_handle *f = file_handle(a0);
    if (!f) {
        ce_error = 6;
        return 0;
    }
    int rv = fs_set_end(f->fd);
    return rv < 0 ? file_error(rv) : 1;
}
CE_API(GetModuleHandleW)
{
    if (!a0)
        return ce_main.base;
    char name[64];
    ce_narrow(name, sizeof(name), PTR(WCHAR, a0));
    CW handle = ce_module_handle(name);
    if (!handle)
        ce_error = 126;
    return handle;
}
CE_API(LoadLibraryW)
{
    char name[64];
    ce_narrow(name, sizeof(name), PTR(WCHAR, a0));
    if (!strcasecmp(name, "coredll.dll"))
        return 0x73000000u;
    return ce_load_library(name);
}
static CW proc_address(CW module, const char *name, CW ordinal)
{
    CW address = module == 0x73000000u ? ce_core_proc(name, (uint16_t)ordinal)
                 : name                ? ce_find_symbol(module, name)
                                       : ce_find_ordinal(module, (uint16_t)ordinal);
    if (!address)
        ce_error = 127;
    return address;
}
CE_API(GetProcAddressW)
{
    if (a1 < 65536)
        return proc_address(a0, NULL, a1);
    char name[128];
    ce_narrow(name, sizeof(name), PTR(WCHAR, a1));
    return proc_address(a0, name, 0);
}
CE_API(GetProcAddressA)
{
    return proc_address(a0, a1 < 65536 ? NULL : PTR(char, a1), a1);
}
CE_API(FreeLibrary)
{
    return a0 != 0;
}
CE_API(GetVersionEx)
{
    uint8_t *p = PTR(uint8_t, a0);
    if (!p || ce_u32(p) < 20)
        return 0;
    ce_put32(p + 4, 3);
    ce_put32(p + 8, 0);
    ce_put32(p + 12, 11171);
    ce_put32(p + 16, 3);
    return 1;
}
CE_API(GlobalMemoryStatus)
{
    CW *p = PTR(CW, a0);
    struct gz_sysinfo si;
    gz_sysinfo(&si);
    p[0] = 32;
    p[1] = 100 - si.free_pages * 100 / MAX(si.total_pages, 1u);
    p[2] = si.total_pages * 4096;
    p[3] = si.free_pages * 4096;
    p[4] = p[5] = 0;
    p[6] = p[2];
    p[7] = p[3];
    return 0;
}

static const struct ce_export exports[] = {
    {"__ll_mul", (void *)ce_ll_mul},
    {"__ll_div", (void *)ce_ll_div},
    {"__ull_div", (void *)ce_ull_div},
    {"__ll_rem", (void *)ce_ll_rem},
    {"__ull_rem", (void *)ce_ull_rem},
    {"__ll_lshift", (void *)ce_ll_lshift},
    {"__ll_rshift", (void *)ce_ll_rshift},
    {"__ull_rshift", (void *)ce_ull_rshift},
    CE_EXPORT(GetLastError),
    CE_EXPORT(SetLastError),
    CE_EXPORT(LocalAlloc),
    CE_EXPORT(LocalFree),
    CE_EXPORT(LocalReAlloc),
    CE_EXPORT(malloc),
    CE_EXPORT(HeapCreate),
    CE_EXPORT(HeapAlloc),
    CE_EXPORT(HeapFree),
    CE_EXPORT(HeapDestroy),
    CE_EXPORT(strlen),
    CE_EXPORT(mbstowcs),
    CE_EXPORT(wcstombs),
    CE_EXPORT(wcsrchr),
    CE_EXPORT(wcsstr),
    CE_EXPORT(_wcsicmp),
    CE_EXPORT(_wcsnicmp),
    CE_EXPORT(_wtol),

    CE_EXPORT(free),
    {"??2@YAPAXI@Z", (void *)ce_malloc},
    {"??3@YAXPAX@Z", (void *)ce_free},
    CE_EXPORT(memcpy),
    CE_EXPORT(memmove),
    CE_EXPORT(memset),
    CE_EXPORT(memcmp),
    CE_EXPORT(wcslen),
    CE_EXPORT(wcscmp),
    CE_EXPORT(wcsncmp),
    CE_EXPORT(wcscpy),
    CE_EXPORT(wcsncpy),
    CE_EXPORT(wcscat),
    CE_EXPORT(wcschr),
    CE_EXPORT(lstrcmpW),
    CE_EXPORT(lstrcmpiW),
    CE_EXPORT(lstrlenW),
    CE_EXPORT(lstrcpyW),
    CE_EXPORT(wsprintfW),
    {"swprintf", (void *)ce_wsprintfW},
    CE_EXPORT(MultiByteToWideChar),
    CE_EXPORT(WideCharToMultiByte),
    CE_EXPORT(CompareStringW),
    CE_EXPORT(RegCreateKeyExW),
    CE_EXPORT(RegOpenKeyExW),
    CE_EXPORT(RegCloseKey),
    CE_EXPORT(RegQueryValueExW),
    CE_EXPORT(RegSetValueExW),
    CE_EXPORT(RegDeleteValueW),
    CE_EXPORT(RegEnumKeyExW),
    CE_EXPORT(RegEnumValueW),
    CE_EXPORT(RegQueryInfoKeyW),
    CE_EXPORT(RegDeleteKeyW),
    CE_EXPORT(CreateFileW),
    CE_EXPORT(FindFirstFileW),
    CE_EXPORT(FindNextFileW),
    CE_EXPORT(FindClose),
    CE_EXPORT(CloseHandle),
    CE_EXPORT(ReadFile),
    CE_EXPORT(WriteFile),
    CE_EXPORT(GetFileSize),
    CE_EXPORT(GetFileAttributesW),
    CE_EXPORT(CreateDirectoryW),
    CE_EXPORT(RemoveDirectoryW),
    CE_EXPORT(MoveFileW),
    CE_EXPORT(DeleteFileW),
    CE_EXPORT(SetFilePointer),
    CE_EXPORT(SetEndOfFile),
    CE_EXPORT(ExitThread),
    CE_EXPORT(ExitProcess),
    CE_EXPORT(TerminateProcess),
    CE_EXPORT(Sleep),
    CE_EXPORT(GetTickCount),
    CE_EXPORT(GetLocalTime),
    CE_EXPORT(GetModuleFileNameW),
    CE_EXPORT(GetModuleHandleW),
    CE_EXPORT(LoadLibraryW),
    CE_EXPORT(GetProcAddressW),
    CE_EXPORT(GetProcAddressA),
    CE_EXPORT(FreeLibrary),
    CE_EXPORT(GetVersionEx),
    {"GetVersionExW", (void *)ce_GetVersionEx},
    CE_EXPORT(GlobalMemoryStatus),
};
void *ce_resolve_core(const char *name)
{
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return NULL;
}
