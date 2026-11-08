/* C runtime and small system queries exported by COREDLL beyond string.c. */
#include "runtime.h"
#include "text.h"

CE_API(strncat)
{
    return WORD(strncat(PTR(char, a0), PTR(char, a1), a2));
}
CE_API(strncmp)
{
    return (CW)strncmp(PTR(char, a0), PTR(char, a1), a2);
}
CE_API(_itoa)
{
    char *out = PTR(char, a1);
    unsigned radix = a2 >= 2 && a2 <= 36 ? a2 : 10;
    uint32_t value = a0;
    char digits[34];
    unsigned n = 0;
    bool negative = radix == 10 && (int32_t)a0 < 0;
    if (negative)
        value = 0u - value;
    do {
        unsigned d = value % radix;
        digits[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        value /= radix;
    } while (value);
    char *p = out;
    if (negative)
        *p++ = '-';
    while (n)
        *p++ = digits[--n];
    *p = 0;
    return a1;
}
static uint32_t random_state = 1;
CE_API(rand)
{
    random_state = random_state * 214013u + 2531011u; /* Microsoft CRT LCG */
    return (random_state >> 16) & 0x7fff;
}
CE_API(srand)
{
    random_state = a0;
    return 0;
}
CE_API(realloc)
{
    return WORD(realloc(PTR(void, a0), a1));
}
CE_API(_purecall)
{
    ce_fatal("WinCE pure virtual function call");
}
/* Pointers are not probed: the runtime cannot recover from a fault, so these
 * report only the NULL pointers an application could test for. */
CE_API(IsBadReadPtr)
{
    return !a0 && a1;
}
CE_API(IsBadCodePtr)
{
    return !a0;
}
CE_API(GetACP)
{
    return 1252;
}
CE_API(GetOEMCP)
{
    return 437;
}
CE_API(IsValidCodePage)
{
    return a0 == 1252 || a0 == 437 || a0 == 65001;
}

/* MEM_RESERVE and MEM_COMMIT both back the whole region at once; commits
 * inside a reserved region return its address. */
static struct {
    uint8_t *base, *block;
    uint32_t size;
} regions[32];
CE_API(VirtualAlloc)
{
    if (!a1 || a1 > 16u * 1024 * 1024 || !(a2 & 0x3000u)) {
        ce_error = 87;
        return 0;
    }
    for (unsigned i = 0; a0 && i < ARRAY_SIZE(regions); i++)
        if (regions[i].base && a0 >= WORD(regions[i].base) && a0 - WORD(regions[i].base) < regions[i].size)
            return a0 - WORD(regions[i].base) + a1 <= regions[i].size ? a0 : 0;
    if (a0) {
        ce_error = 487;
        return 0;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(regions); i++)
        if (!regions[i].base) {
            uint32_t size = ALIGN_UP(a1, 4096u);
            uint8_t *block = calloc(1, size + 4095);
            if (!block)
                break;
            regions[i].block = block;
            regions[i].base = PTR(uint8_t, ALIGN_UP(WORD(block), 4096u));
            regions[i].size = size;
            return WORD(regions[i].base);
        }
    ce_error = 8;
    return 0;
}
CE_API(VirtualFree)
{
    for (unsigned i = 0; i < ARRAY_SIZE(regions); i++)
        if (regions[i].base && WORD(regions[i].base) == a0) {
            if (a2 & 0x8000u) { /* MEM_RELEASE; MEM_DECOMMIT keeps the memory */
                free(regions[i].block);
                regions[i].base = regions[i].block = NULL;
            }
            return 1;
        }
    for (unsigned i = 0; i < ARRAY_SIZE(regions); i++)
        if (regions[i].base && a0 > WORD(regions[i].base) && a0 - WORD(regions[i].base) < regions[i].size &&
            (a2 & 0x4000u))
            return 1; /* decommit inside a region */
    ce_error = 87;
    return 0;
}

/* C++ operator new/delete (and their array forms) from Microsoft's CRT. new
 * of zero bytes still returns a unique pointer. */
CE_API(operator_new)
{
    return WORD(malloc(a0 ? a0 : 1));
}
CE_API(operator_delete)
{
    free(PTR(void, a0));
    return 0;
}

CE_API(_ltow)
{
    WCHAR *out = PTR(WCHAR, a1);
    unsigned radix = a2 >= 2 && a2 <= 36 ? a2 : 10;
    bool negative = radix == 10 && (int32_t)a0 < 0;
    uint32_t value = negative ? 0u - a0 : a0;
    WCHAR digits[33];
    unsigned n = 0;
    do {
        unsigned d = value % radix;
        digits[n++] = (WCHAR)(d < 10 ? '0' + d : 'a' + d - 10);
        value /= radix;
    } while (value);
    WCHAR *p = out;
    if (negative)
        *p++ = '-';
    while (n)
        *p++ = digits[--n];
    *p = 0;
    return a1;
}
static unsigned digit_value(unsigned c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
}
CE_API(strtol)
{
    const char *p = PTR(char, a0);
    unsigned base = a2;
    while (*p == ' ' || (*p >= 9 && *p <= 13))
        p++;
    bool negative = *p == '-';
    if (*p == '-' || *p == '+')
        p++;
    if ((!base || base == 16) && p[0] == '0' && (p[1] | 32) == 'x' && digit_value((uint8_t)p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (!base) {
        base = *p == '0' ? 8 : 10;
    }
    const char *start = p;
    uint32_t value = 0;
    bool overflow = false;
    for (unsigned d; base <= 36 && (d = digit_value((uint8_t)*p)) < base; p++) {
        if (value > (UINT32_MAX - d) / base)
            overflow = true;
        value = value * base + d;
    }
    if (a1)
        *PTR(CW, a1) = WORD(p == start ? PTR(char, a0) : p);
    uint32_t limit = negative ? 0x80000000u : 0x7fffffffu;
    if (overflow || value > limit) {
        ce_error = 34; /* ERANGE */
        return negative ? 0x80000000u : 0x7fffffffu;
    }
    return negative ? 0u - value : value;
}
CE_API(atol)
{
    return ce_strtol(a0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
/* GetTempPathW(capacity, buffer): CE's \Temp\, which the runtime keeps in
 * the app's private drive. Returns the length, or the size needed. */
CE_API(GetTempPathW)
{
    static const WCHAR temp[] = u"\\Temp\\";
    unsigned n = ARRAY_SIZE(temp) - 1;
    if (a0 <= n || !a1)
        return n + 1;
    memcpy(PTR(WCHAR, a1), temp, sizeof(temp));
    return n;
}
CE_API(strrchr)
{
    return WORD(strrchr(PTR(char, a0), (int)(a1 & 255)));
}
CE_API(toupper)
{
    return a0 >= 'a' && a0 <= 'z' ? a0 - 32 : a0;
}
CE_API(tolower)
{
    return a0 >= 'A' && a0 <= 'Z' ? a0 + 32 : a0;
}
CE_API(wcsspn)
{
    const WCHAR *s = PTR(WCHAR, a0), *set = PTR(WCHAR, a1);
    unsigned n = 0;
    for (; s[n]; n++) {
        const WCHAR *c = set;
        while (*c && *c != s[n])
            c++;
        if (!*c)
            break;
    }
    return n;
}

/* Latin-1 case mapping, as the runtime's towupper/_wcslwr do. */
static WCHAR upper(WCHAR c)
{
    return (c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7) ? c - 32 : c == 0xff ? 0x178 : c;
}
static WCHAR lower(WCHAR c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7) ? c + 32 : c == 0x178 ? 0xff : c;
}
/* A pointer whose high word is zero holds a single character instead. */
CE_API(CharUpperW)
{
    if (a0 < 0x10000u)
        return upper((WCHAR)a0);
    for (WCHAR *p = PTR(WCHAR, a0); *p; p++)
        *p = upper(*p);
    return a0;
}
CE_API(CharLowerW)
{
    if (a0 < 0x10000u)
        return lower((WCHAR)a0);
    for (WCHAR *p = PTR(WCHAR, a0); *p; p++)
        *p = lower(*p);
    return a0;
}
CE_API(CharUpperBuffW)
{
    for (CW i = 0; i < a1; i++)
        PTR(WCHAR, a0)[i] = upper(PTR(WCHAR, a0)[i]);
    return a1;
}
CE_API(CharLowerBuffW)
{
    for (CW i = 0; i < a1; i++)
        PTR(WCHAR, a0)[i] = lower(PTR(WCHAR, a0)[i]);
    return a1;
}
/* LCMapStringW(locale, flags, source, length, dest, capacity): only case
 * mapping (LCMAP_LOWERCASE 0x100, LCMAP_UPPERCASE 0x200) is supported. */
CE_API(LCMapStringW)
{
    const WCHAR *src = PTR(WCHAR, a2);
    uint32_t n = (int32_t)a3 < 0 ? ce_wlen(src) + 1 : a3;
    if (!src || (a1 & ~0x300u) || (a1 & 0x300u) == 0x300u) {
        ce_error = 1004; /* ERROR_INVALID_FLAGS */
        return 0;
    }
    if (!a5)
        return n;
    if (a5 < n) {
        ce_error = 122;
        return 0;
    }
    WCHAR *dst = PTR(WCHAR, a4);
    for (uint32_t i = 0; i < n; i++)
        dst[i] = a1 & 0x200u ? upper(src[i]) : a1 & 0x100u ? lower(src[i]) : src[i];
    return n;
}

/* _vsnwprintf(buffer, count, format, va_list): CE's va_list points at the
 * argument words. Returns -1 without a terminator when the text is truncated. */
CE_API(_vsnwprintf)
{
    WCHAR text[1024];
    uintptr_t words[64];
    for (unsigned i = 0; i < ARRAY_SIZE(words); i++)
        words[i] = PTR(CW, a3)[i];
    int n = ce_wformat(text, ARRAY_SIZE(text), PTR(WCHAR, a2), words, ARRAY_SIZE(words));
    if (n < 0)
        ce_missing("_vsnwprintf conversion");
    if ((CW)n > a1) {
        memcpy(PTR(WCHAR, a0), text, a1 * 2);
        return (CW)-1;
    }
    memcpy(PTR(WCHAR, a0), text, ((CW)n + ((CW)n < a1)) * 2);
    return (CW)n;
}

/* Minimal stdio for wide paths: _wfopen/fclose/fwprintf/fputws/fflush.
 * Text streams store ANSI (Windows-1252) bytes with CR LF line ends, as
 * Microsoft's CRT writes wide output to a non-Unicode stream. */
CW ce_CreateFileW(CW, CW, CW, CW, CW, CW, CW, CW, CW, CW, CW, CW);
CW ce_WriteFile(CW, CW, CW, CW, CW, CW, CW, CW, CW, CW, CW, CW);
CW ce_ReadFile(CW, CW, CW, CW, CW, CW, CW, CW, CW, CW, CW, CW);
CW ce_CloseHandle(CW, CW, CW, CW, CW, CW, CW, CW, CW, CW, CW, CW);
CW ce_SetFilePointer(CW, CW, CW, CW, CW, CW, CW, CW, CW, CW, CW, CW);
struct ce_stream {
    CW handle;
    bool binary, console; /* console: _getstdfilex's stdin/out/err, the serial log */
};
CE_API(_wfopen)
{
    const WCHAR *mode = PTR(WCHAR, a1);
    if (!a0 || !mode) {
        ce_error = 87;
        return 0;
    }
    bool plus = false, binary = false;
    for (const WCHAR *m = mode + 1; *m; m++) {
        plus |= *m == '+';
        binary |= *m == 'b';
    }
    CW access = 0, disposition;
    switch (mode[0]) {
    case 'r': access = 0x80000000u | (plus ? 0x40000000u : 0); disposition = 3; break;
    case 'w': access = 0x40000000u | (plus ? 0x80000000u : 0); disposition = 2; break;
    case 'a': access = 0x40000000u | (plus ? 0x80000000u : 0); disposition = 4; break;
    default: ce_error = 87; return 0;
    }
    CW handle = ce_CreateFileW(a0, access, 1, 0, disposition, 0x80, 0, 0, 0, 0, 0, 0);
    if (handle == 0xffffffffu)
        return 0;
    if (mode[0] == 'a')
        ce_SetFilePointer(handle, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0);
    struct ce_stream *s = malloc(sizeof(*s));
    if (!s) {
        ce_CloseHandle(handle, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        ce_error = 8;
        return 0;
    }
    s->handle = handle;
    s->binary = binary;
    return WORD(s);
}
CE_API(fclose)
{
    struct ce_stream *s = PTR(struct ce_stream, a0);
    if (!s || s->console)
        return s ? 0 : (CW)-1;
    CW ok = ce_CloseHandle(s->handle, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    free(s);
    return ok ? 0 : (CW)-1;
}
CE_API(fflush)
{
    return 0;
}
/* fopen(path, mode): _wfopen with Windows-1252 names. */
CE_API(fopen)
{
    WCHAR path[FS_PATH_MAX], mode[8];
    const char *p = PTR(char, a0), *m = PTR(char, a1);
    if (!p || !m) {
        ce_error = 87;
        return 0;
    }
    unsigned i = 0;
    for (; p[i] && i + 1 < ARRAY_SIZE(path); i++)
        path[i] = ce_cp1252_decode((uint8_t)p[i]);
    path[i] = 0;
    for (i = 0; m[i] && i + 1 < ARRAY_SIZE(mode); i++)
        mode[i] = (uint8_t)m[i];
    mode[i] = 0;
    return ce__wfopen(WORD(path), WORD(mode), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
/* _getstdfilex(0 stdin, 1 stdout, 2 stderr): writes go to the serial log. */
CE_API(_getstdfilex)
{
    static struct ce_stream console[3] = {{0, true, true}, {0, true, true}, {0, true, true}};
    return a0 < 3 ? WORD(&console[a0]) : 0;
}
/* fread/fwrite(buffer, size, count, stream) return whole items. */
CE_API(fread)
{
    struct ce_stream *s = PTR(struct ce_stream, a3);
    CW bytes = a1 * a2, got = 0;
    if (!s || s->console || !a1 || (a2 && bytes / a2 != a1))
        return 0;
    if (!ce_ReadFile(s->handle, a0, bytes, WORD(&got), 0, 0, 0, 0, 0, 0, 0, 0))
        return 0;
    return got / a1;
}
CE_API(fwrite)
{
    struct ce_stream *s = PTR(struct ce_stream, a3);
    CW bytes = a1 * a2, wrote = 0;
    if (!s || !a1 || (a2 && bytes / a2 != a1))
        return 0;
    if (s->console) {
        printf("wince: stdout: %.*s\n", (int)MIN(bytes, 256u), PTR(char, a0));
        return a2;
    }
    if (!ce_WriteFile(s->handle, a0, bytes, WORD(&wrote), 0, 0, 0, 0, 0, 0, 0, 0))
        return 0;
    return wrote / a1;
}
/* fseek(stream, offset, SEEK_SET 0 / CUR 1 / END 2); ftell. */
CE_API(fseek)
{
    struct ce_stream *s = PTR(struct ce_stream, a0);
    if (!s || s->console || a2 > 2)
        return (CW)-1;
    return ce_SetFilePointer(s->handle, a1, 0, a2, 0, 0, 0, 0, 0, 0, 0, 0) == 0xffffffffu ? (CW)-1 : 0;
}
CE_API(ftell)
{
    struct ce_stream *s = PTR(struct ce_stream, a0);
    if (!s || s->console)
        return (CW)-1;
    return ce_SetFilePointer(s->handle, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0);
}
static int stream_write(struct ce_stream *s, const WCHAR *text, unsigned n)
{
    if (s->console) {
        char line[1024];
        unsigned k = 0;
        for (; k < n && k + 1 < sizeof(line); k++)
            line[k] = ce_glyph(text[k]);
        line[k] = 0;
        printf("wince: stdout: %s\n", line);
        return (int)n;
    }
    char out[2048];
    unsigned used = 0;
    for (unsigned i = 0; i < n; i++) {
        if (used + 2 > sizeof(out)) {
            CW wrote = 0;
            if (!ce_WriteFile(s->handle, WORD(out), used, WORD(&wrote), 0, 0, 0, 0, 0, 0, 0, 0))
                return -1;
            used = 0;
        }
        if (text[i] == '\n' && !s->binary)
            out[used++] = '\r';
        int c = ce_cp1252_encode(text[i]);
        out[used++] = (char)(c < 0 ? '?' : c);
    }
    CW wrote = 0;
    return !used || ce_WriteFile(s->handle, WORD(out), used, WORD(&wrote), 0, 0, 0, 0, 0, 0, 0, 0) ? (int)n : -1;
}
/* fprintf(stream, format, ...): a narrow format is widened, with %s and %c
 * naming narrow arguments (%hs and %hc in wsprintf's terms). */
CE_API(fprintf)
{
    struct ce_stream *s = PTR(struct ce_stream, a0);
    const char *f = PTR(char, a1);
    WCHAR format[512], text[1024];
    unsigned n = 0;
    for (unsigned i = 0; f && f[i] && n + 2 < ARRAY_SIZE(format); i++) {
        format[n++] = ce_cp1252_decode((uint8_t)f[i]);
        if (f[i] != '%')
            continue;
        while (f[i + 1] && strchr("-+ #0123456789.*l", f[i + 1]) && n + 2 < ARRAY_SIZE(format))
            format[n++] = (uint8_t)f[++i];
        if (f[i + 1] == 's' || f[i + 1] == 'c')
            format[n++] = 'h';
    }
    format[n] = 0;
    uintptr_t words[] = {a2, a3, a4, a5, a6, a7, a8, a9, a10, a11};
    int len = s && f ? ce_wformat(text, ARRAY_SIZE(text), format, words, ARRAY_SIZE(words)) : -1;
    if (len < 0)
        return (CW)-1;
    return (CW)stream_write(s, text, (unsigned)len);
}
CE_API(fwprintf)
{
    struct ce_stream *s = PTR(struct ce_stream, a0);
    WCHAR text[1024];
    uintptr_t words[] = {a2, a3, a4, a5, a6, a7, a8, a9, a10, a11};
    int n = s ? ce_wformat(text, ARRAY_SIZE(text), PTR(WCHAR, a1), words, ARRAY_SIZE(words)) : -1;
    if (n < 0)
        return (CW)-1;
    return (CW)stream_write(s, text, (unsigned)n);
}
CE_API(fputws)
{
    struct ce_stream *s = PTR(struct ce_stream, a1);
    return s && stream_write(s, PTR(WCHAR, a0), ce_wlen(PTR(WCHAR, a0))) >= 0 ? 0 : (CW)-1;
}

static const struct ce_export exports[] = {
    {"??2@YAPAXI@Z", (void *)ce_operator_new},
    {"??_U@YAPAXI@Z", (void *)ce_operator_new},
    {"??3@YAXPAX@Z", (void *)ce_operator_delete},
    {"??_V@YAXPAX@Z", (void *)ce_operator_delete},
    CE_EXPORT(_ltow),
    CE_EXPORT(strtol),
    CE_EXPORT(atol),
    CE_EXPORT(GetTempPathW),
    CE_EXPORT(strrchr),
    CE_EXPORT(toupper),
    CE_EXPORT(tolower),
    CE_EXPORT(wcsspn),
    CE_EXPORT(CharUpperW),
    CE_EXPORT(CharLowerW),
    CE_EXPORT(CharUpperBuffW),
    CE_EXPORT(CharLowerBuffW),
    CE_EXPORT(LCMapStringW),
    CE_EXPORT(_vsnwprintf),
    CE_EXPORT(_wfopen),
    CE_EXPORT(fopen),
    CE_EXPORT(_getstdfilex),
    CE_EXPORT(fread),
    CE_EXPORT(fwrite),
    CE_EXPORT(fseek),
    CE_EXPORT(ftell),
    CE_EXPORT(fprintf),
    CE_EXPORT(fclose),
    CE_EXPORT(fflush),
    CE_EXPORT(fwprintf),
    CE_EXPORT(fputws),
    CE_EXPORT(strncat),
    CE_EXPORT(strncmp),
    CE_EXPORT(_itoa),
    CE_EXPORT(rand),
    CE_EXPORT(srand),
    CE_EXPORT(realloc),
    CE_EXPORT(_purecall),
    CE_EXPORT(IsBadReadPtr),
    {"IsBadWritePtr", (void *)ce_IsBadReadPtr},
    CE_EXPORT(IsBadCodePtr),
    CE_EXPORT(GetACP),
    CE_EXPORT(GetOEMCP),
    CE_EXPORT(IsValidCodePage),
    CE_EXPORT(VirtualAlloc),
    CE_EXPORT(VirtualFree),
};
void *ce_resolve_crt(const char *name)
{
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return NULL;
}
