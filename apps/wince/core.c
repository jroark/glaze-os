/* WinCE core services used by the original BE-300 applications. */
#include "runtime.h"

CW ce_error;

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

bool ce_path(char path[FS_PATH_MAX], const WCHAR *input)
{
    char raw[FS_PATH_MAX], relative[FS_PATH_MAX];
    ce_narrow(raw, sizeof(raw), input);
    for (char *p = raw; *p; p++)
        if (*p == '\\')
            *p = '/';
    const char *s = raw;
    if (!strncasecmp(s, "/Nand Disk/", 11))
        s += 10;
    if (!strncasecmp(s, "/My Documents", 13) && (!s[13] || s[13] == '/')) {
        snprintf(relative, sizeof(relative), "/Documents%s", s + 13);
        s = relative;
    }
    /* Canonical components are required before choosing a backing root. */
    for (const char *p = s; *p;) {
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
    int n;
    if (!strncasecmp(s, "/Documents", 10) && (!s[10] || s[10] == '/'))
        n = snprintf(path, FS_PATH_MAX, "/Documents%s", s + 10);
    else {
        n = snprintf(path, FS_PATH_MAX, "%s/data/drive%s%s", res_bundle(), *s == '/' ? "" : "/", s);
    }
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
CE_API(wcscpy)
{
    memcpy(PTR(void, a0), PTR(void, a1), (ce_wlen(PTR(WCHAR, a1)) + 1) * 2);
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
CE_API(Sleep)
{
    gz_sleep_ms(a0);
    return 0;
}
CE_API(GetTickCount)
{
    return gz_time_ms();
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

/* Persistent per-app registry. Values are typed bytes, and missing values
 * remain missing. The on-disk stream is validated before it is loaded. */
#define REG_KEYS 64
#define REG_VALUES 128
struct reg_key {
    char path[256];
};
struct reg_value {
    char key[256], name[128];
    CW type, size;
    uint8_t *bytes;
};
static struct reg_key reg_keys[REG_KEYS];
static struct reg_value reg_values[REG_VALUES];
static unsigned key_count, value_count;
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
    return handle >= 0x71000000u && handle - 0x71000000u < key_count ? reg_keys[handle - 0x71000000u].path
                                                                     : NULL;
}
static void reg_load(void)
{
    if (registry_loaded)
        return;
    registry_loaded = true;
    char path[FS_PATH_MAX];
    registry_file(path);
    struct fs_stat st;
    if (fs_stat(path, &st))
        return;
    if (st.size > 256 * 1024 || st.size < 8)
        ce_fatal("Invalid WinCE registry file");
    uint8_t *data = malloc(st.size);
    if (!data || fs_read_file(path, data, st.size) != (int)st.size)
        ce_fatal("Cannot read WinCE registry");
    if (memcmp(data, "GZCE", 4))
        ce_fatal("Unknown WinCE registry format");
    uint32_t count = ce_u32(data + 4), pos = 8;
    if (count > REG_VALUES)
        ce_fatal("WinCE registry has too many values");
    for (unsigned i = 0; i < count; i++) {
        if (!ce_range(pos, 16, st.size))
            ce_fatal("Truncated WinCE registry");
        uint32_t k = ce_u32(data + pos), n = ce_u32(data + pos + 4), size = ce_u32(data + pos + 12);
        if (!k || k >= 256 || !n || n >= 128 || size > 65536 || !ce_range(pos + 16, k + n + size, st.size))
            ce_fatal("Invalid WinCE registry entry");
        struct reg_value *v = &reg_values[value_count++];
        v->type = ce_u32(data + pos + 8);
        v->size = size;
        pos += 16;
        memcpy(v->key, data + pos, k);
        memcpy(v->name, data + pos + k, n);
        if (v->key[k - 1] || v->name[n - 1])
            ce_fatal("Unterminated WinCE registry name");
        v->bytes = malloc(size ? size : 1);
        if (!v->bytes)
            ce_fatal("Out of memory loading WinCE registry");
        memcpy(v->bytes, data + pos + k + n, size);
        pos += k + n + size;
    }
    if (pos != st.size)
        ce_fatal("Trailing bytes in WinCE registry");
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
    int fd = fs_open(temporary, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    if (fd < 0)
        return false;
    uint8_t header[16];
    memcpy(header, "GZCE", 4);
    ce_put32(header + 4, value_count);
    bool ok = fs_write(fd, header, 8) == 8;
    for (unsigned i = 0; ok && i < value_count; i++) {
        struct reg_value *v = &reg_values[i];
        unsigned k = strlen(v->key) + 1, n = strlen(v->name) + 1;
        ce_put32(header, k);
        ce_put32(header + 4, n);
        ce_put32(header + 8, v->type);
        ce_put32(header + 12, v->size);
        ok = fs_write(fd, header, 16) == 16 && fs_write(fd, v->key, k) == (int)k &&
             fs_write(fd, v->name, n) == (int)n && fs_write(fd, v->bytes, v->size) == (int)v->size;
    }
    if (fs_close(fd))
        ok = false;
    return ok && !fs_rename(temporary, path);
}
static CW reg_open(CW root, CW subkey, CW result, CW disposition, bool create)
{
    reg_load();
    const char *parent = reg_root(root);
    if (!parent || !result)
        return 6;
    char sub[256], path[256];
    ce_narrow(sub, sizeof(sub), PTR(WCHAR, subkey));
    if (snprintf(path, sizeof(path), "%s%s%s", parent, *sub ? "\\" : "", sub) >= (int)sizeof(path))
        return 87;
    unsigned i;
    for (i = 0; i < key_count; i++)
        if (!strcasecmp(reg_keys[i].path, path))
            break;
    bool existed = i < key_count;
    if (!existed) {
        for (unsigned j = 0; j < value_count; j++)
            if (!strcasecmp(reg_values[j].key, path)) {
                existed = true;
                break;
            }
        if (!create && !existed)
            return 2;
        if (key_count == REG_KEYS)
            return 8;
        strlcpy(reg_keys[key_count++].path, path, sizeof(reg_keys[0].path));
    }
    *PTR(CW, result) = 0x71000000u + i;
    if (disposition)
        *PTR(CW, disposition) = existed ? 2 : 1;
    return 0;
}
CE_API(RegCreateKeyExW)
{
    return reg_open(a0, a1, a7, a8, true);
}
CE_API(RegOpenKeyExW)
{
    return reg_open(a0, a1, a4, 0, false);
}
CE_API(RegCloseKey)
{
    return reg_root(a0) ? 0 : 6;
}
CE_API(RegQueryValueExW)
{
    reg_load();
    const char *key = reg_root(a0);
    if (!key || !a5)
        return 6;
    char name[128];
    ce_narrow(name, sizeof(name), PTR(WCHAR, a1));
    for (unsigned i = 0; i < value_count; i++) {
        struct reg_value *v = &reg_values[i];
        if (strcasecmp(key, v->key) || strcasecmp(name, v->name))
            continue;
        if (a3)
            *PTR(CW, a3) = v->type;
        CW capacity = *PTR(CW, a5);
        *PTR(CW, a5) = v->size;
        if (!a4)
            return 0;
        if (capacity < v->size)
            return 234;
        memcpy(PTR(void, a4), v->bytes, v->size);
        return 0;
    }
    return 2;
}
CE_API(RegSetValueExW)
{
    reg_load();
    const char *key = reg_root(a0);
    if (!key)
        return 6;
    if (a5 > 65536 || (a5 && !a4))
        return 87;
    char name[128];
    ce_narrow(name, sizeof(name), PTR(WCHAR, a1));
    unsigned i;
    for (i = 0; i < value_count; i++)
        if (!strcasecmp(key, reg_values[i].key) && !strcasecmp(name, reg_values[i].name))
            break;
    if (i == REG_VALUES)
        return 8;
    uint8_t *copy = malloc(a5 ? a5 : 1);
    if (!copy)
        return 8;
    memcpy(copy, PTR(void, a4), a5);
    if (i == value_count)
        value_count++;
    struct reg_value *v = &reg_values[i];
    free(v->bytes);
    v->bytes = copy;
    v->size = a5;
    v->type = a3;
    strlcpy(v->key, key, sizeof(v->key));
    strlcpy(v->name, name, sizeof(v->name));
    return reg_save() ? 0 : 5;
}
CE_API(RegDeleteValueW)
{
    reg_load();
    const char *key = reg_root(a0);
    if (!key)
        return 6;
    char name[128];
    ce_narrow(name, sizeof(name), PTR(WCHAR, a1));
    for (unsigned i = 0; i < value_count; i++)
        if (!strcasecmp(key, reg_values[i].key) && !strcasecmp(name, reg_values[i].name)) {
            free(reg_values[i].bytes);
            reg_values[i] = reg_values[--value_count];
            memset(&reg_values[value_count], 0, sizeof(reg_values[0]));
            return reg_save() ? 0 : 5;
        }
    return 2;
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
CE_API(CreateFileW)
{
    char path[FS_PATH_MAX];
    if (!ce_path(path, PTR(WCHAR, a0)))
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
            int fd = fs_open(path, flags);
            if (fd < 0) {
                file_error(fd);
                return 0xffffffffu;
            }
            files[i].fd = fd;
            files[i].active = true;
            strlcpy(files[i].path, path, sizeof(files[i].path));
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
    char path[FS_PATH_MAX];
    struct fs_stat st;
    if (!ce_path(path, PTR(WCHAR, a0)) || fs_stat(path, &st)) {
        ce_error = 2;
        return 0xffffffffu;
    }
    return st.type == FS_DIR ? 16 : 0x80;
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
CE_API(LoadLibraryW)
{
    char name[64];
    ce_narrow(name, sizeof(name), PTR(WCHAR, a0));
    if (!strcasecmp(name, "coredll.dll"))
        return 0x73000000u;
    return ce_load_library(name);
}
CE_API(GetProcAddressW)
{
    char name[128];
    ce_narrow(name, sizeof(name), PTR(WCHAR, a1));
    return a0 == 0x73000000u ? ce_import_address("coredll.dll", name) : ce_find_symbol(a0, name);
}
CE_API(FreeLibrary)
{
    return a0 == 0x73000000u || a0 == 0x100000u;
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
    CE_EXPORT(GetLastError),
    CE_EXPORT(SetLastError),
    CE_EXPORT(LocalAlloc),
    CE_EXPORT(LocalFree),
    CE_EXPORT(LocalReAlloc),
    CE_EXPORT(memcpy),
    CE_EXPORT(memmove),
    CE_EXPORT(memset),
    CE_EXPORT(memcmp),
    CE_EXPORT(wcslen),
    CE_EXPORT(wcscmp),
    CE_EXPORT(wcscpy),
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
    CE_EXPORT(CreateFileW),
    CE_EXPORT(CloseHandle),
    CE_EXPORT(ReadFile),
    CE_EXPORT(WriteFile),
    CE_EXPORT(GetFileSize),
    CE_EXPORT(GetFileAttributesW),
    CE_EXPORT(DeleteFileW),
    CE_EXPORT(SetFilePointer),
    CE_EXPORT(SetEndOfFile),
    CE_EXPORT(ExitThread),
    CE_EXPORT(ExitProcess),
    CE_EXPORT(Sleep),
    CE_EXPORT(GetTickCount),
    CE_EXPORT(LoadLibraryW),
    CE_EXPORT(GetProcAddressW),
    CE_EXPORT(FreeLibrary),
    CE_EXPORT(GetVersionEx),
    CE_EXPORT(GlobalMemoryStatus),
};
void *ce_resolve_core(const char *name)
{
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return NULL;
}
