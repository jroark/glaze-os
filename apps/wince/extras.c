/* COREDLL services third-party BE-300 shells and their apps use (BeeV2,
 * BeShell, eXpod, MFC applications), and TOOLHELP.DLL. Each either does
 * what CE does with Glaze's equivalent or reports what CE reports when the
 * device lacks the feature (no COM port, no SIP input methods, no task bar
 * shell). Contracts: Windows CE 3.0 Platform Builder reference. */
#include "runtime.h"
#include "source.h"
#include "text.h"

CE_API(LocalAlloc);
CE_API(CreateFileW);
CE_API(ReadFile);
CE_API(WriteFile);
CE_API(CloseHandle);
CE_API(LoadLibraryW);
CE_API(GetFocus);
CE_API(swscanf);

/* ---- wide and narrow strings --------------------------------------------- */

static unsigned digit(unsigned c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
}
static CW wide_number(CW text, CW end, CW base_word, bool is_signed)
{
    const WCHAR *p = PTR(WCHAR, text);
    unsigned base = base_word;
    if (!p)
        return 0;
    while (*p == ' ' || (*p >= 9 && *p <= 13))
        p++;
    bool negative = *p == '-';
    if (*p == '-' || *p == '+')
        p++;
    if ((!base || base == 16) && p[0] == '0' && (p[1] | 32) == 'x' && digit(p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (!base) {
        base = *p == '0' ? 8 : 10;
    }
    const WCHAR *start = p;
    uint64_t value = 0;
    bool overflow = false;
    for (unsigned d; base >= 2 && base <= 36 && (d = digit(*p)) < base; p++) {
        value = value * base + d;
        if (value > 0xffffffffull)
            overflow = true;
    }
    if (end)
        *PTR(CW, end) = WORD(p == start ? PTR(WCHAR, text) : p);
    if (is_signed) {
        if (overflow || value > (negative ? 0x80000000ull : 0x7fffffffull)) {
            ce_error = 34;
            return negative ? 0x80000000u : 0x7fffffffu;
        }
        return negative ? (CW)(0 - (uint32_t)value) : (CW)value;
    }
    if (overflow) {
        ce_error = 34;
        return 0xffffffffu;
    }
    return negative ? (CW)(0 - (uint32_t)value) : (CW)value;
}
CE_API(wcstol) { return wide_number(a0, a1, a2, true); }
CE_API(wcstoul) { return wide_number(a0, a1, a2, false); }
CE_API(wcscspn)
{
    const WCHAR *s = PTR(WCHAR, a0), *reject = PTR(WCHAR, a1);
    unsigned n = 0;
    for (; s[n]; n++)
        for (const WCHAR *r = reject; *r; r++)
            if (*r == s[n])
                return n;
    return n;
}
CE_API(towlower)
{
    return (a0 >= 'A' && a0 <= 'Z') || (a0 >= 0xc0 && a0 <= 0xde && a0 != 0xd7) ? a0 + 32 : a0 & 0xffff;
}
/* _isctype(c, mask): _UPPER 1, _LOWER 2, _DIGIT 4, _SPACE 8, _PUNCT 0x10,
 * _CONTROL 0x20, _BLANK 0x40, _HEX 0x80, _ALPHA 0x103. */
CE_API(_isctype)
{
    unsigned c = a0 & 0xff, bits = 0;
    if (c >= 'A' && c <= 'Z') bits |= 0x101;
    if (c >= 'a' && c <= 'z') bits |= 0x102;
    if (c >= '0' && c <= '9') bits |= 4 | 0x80;
    if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) bits |= 0x80;
    if (c == ' ' || (c >= 9 && c <= 13)) bits |= 8;
    if (c == ' ' || c == 9) bits |= 0x40;
    if (c < 32 || c == 127) bits |= 0x20;
    if (c > 32 && c < 127 && !(bits & 0x107)) bits |= 0x10;
    return bits & a1;
}
CE_API(_ultow)
{
    WCHAR *out = PTR(WCHAR, a1), digits[33];
    unsigned base = a2 >= 2 && a2 <= 36 ? a2 : 10, n = 0;
    uint32_t v = a0;
    do {
        unsigned d = v % base;
        digits[n++] = (WCHAR)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v);
    for (unsigned i = 0; i < n; i++)
        out[i] = digits[n - 1 - i];
    out[n] = 0;
    return a1;
}
CE_API(_stricmp)
{
    return (CW)strcasecmp(PTR(char, a0), PTR(char, a1));
}
CE_API(memchr)
{
    return WORD(memchr(PTR(void, a0), (int)(a1 & 0xff), a2));
}
CE_API(CharNextW)
{
    const WCHAR *p = PTR(WCHAR, a0);
    return p && *p ? a0 + 2 : a0;
}
CE_API(CharPrevW)
{
    return a1 > a0 ? a1 - 2 : a0;
}
CE_API(IsDBCSLeadByte) { return 0; }
/* Narrow printf-family formats: widened (with %s and %c naming narrow
 * arguments), formatted by wsprintf's engine and narrowed to Windows-1252. */
static int narrow_format(char *out, unsigned capacity, const char *f, const CW *args, unsigned count, bool odd)
{
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
    uintptr_t words[12];
    for (unsigned i = 0; i < count && i < 12; i++)
        words[i] = args[i];
    int len = f ? ce_wformat_slots(text, ARRAY_SIZE(text), format, words, MIN(count, 12u), odd) : -1;
    if (len < 0)
        return -1;
    unsigned k = 0;
    for (; k < (unsigned)len && k + 1 < capacity; k++) {
        int c = ce_cp1252_encode(text[k]);
        out[k] = (char)(c < 0 ? '?' : c);
    }
    if (capacity)
        out[k] = 0;
    return (int)k;
}
CE_API(sprintf)
{
    CW args[] = {a2, a3, a4, a5, a6, a7, a8, a9, a10, a11};
    return (CW)narrow_format(PTR(char, a0), 4096, PTR(char, a1), args, ARRAY_SIZE(args), false);
}
/* vprintf(format, va_list): a CE va_list points at the argument words. */
CE_API(vprintf)
{
    char text[1024];
    int n = narrow_format(text, sizeof(text), PTR(char, a0), PTR(CW, a1), 10, (a1 >> 2) & 1);
    if (n > 0)
        printf("wince: stdout: %s\n", text);
    return (CW)n;
}
/* sscanf: the input and format widened for swscanf's engine. */
CE_API(sscanf)
{
    const char *in = PTR(char, a0), *f = PTR(char, a1);
    if (!in || !f)
        return (CW)-1;
    WCHAR win[512], wf[256];
    unsigned i = 0;
    for (; in[i] && i + 1 < ARRAY_SIZE(win); i++)
        win[i] = ce_cp1252_decode((uint8_t)in[i]);
    win[i] = 0;
    for (i = 0; f[i] && i + 1 < ARRAY_SIZE(wf); i++)
        wf[i] = ce_cp1252_decode((uint8_t)f[i]);
    wf[i] = 0;
    return ce_swscanf(WORD(win), WORD(wf), a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
}

/* ---- numbers and time ---------------------------------------------------- */

static double modf_(double x, double *whole)
{
    double w = x < 0 ? -__builtin_floor(-x) : __builtin_floor(x);
    if (whole)
        *whole = w;
    return x - w;
}
struct ce_div { int quot, rem; };
/* div returns its two words through the caller's hidden result pointer, as
 * GCC's O32 ABI and CE's compiler both pass small structures. */
static struct ce_div div_(int num, int den)
{
    struct ce_div d = {den ? num / den : 0, den ? num % den : 0};
    return d;
}
/* __dpcmp/__fpcmp: -1, 0 or 1 (unordered compares as greater). */
static int dpcmp(double a, double b) { return a < b ? -1 : a == b ? 0 : 1; }
static int fpcmp(float a, float b) { return a < b ? -1 : a == b ? 0 : 1; }
CE_API(Random)
{
    static uint32_t seed = 0x2545f491;
    seed = seed * 1103515245u + 12345u;
    return seed >> 1;
}
CE_API(CompareFileTime)
{
    const uint8_t *a = PTR(uint8_t, a0), *b = PTR(uint8_t, a1);
    uint64_t x = (uint64_t)ce_u32(a) | (uint64_t)ce_u32(a + 4) << 32, y = (uint64_t)ce_u32(b) | (uint64_t)ce_u32(b + 4) << 32;
    return x < y ? (CW)-1 : x > y;
}
/* GetNumberFormatW(lcid, flags, value, NUMBERFMT or NULL, out, capacity):
 * the default locale is English (US): two decimals, groups of three. */
CE_API(GetNumberFormatW)
{
    const WCHAR *value = PTR(WCHAR, a2);
    const uint8_t *fmt = PTR(uint8_t, a3);
    unsigned digits = fmt ? ce_u32(fmt) : 2, leading = fmt ? ce_u32(fmt + 4) : 1, group = fmt ? ce_u32(fmt + 8) : 3;
    const WCHAR *dec = fmt ? PTR(WCHAR, ce_u32(fmt + 12)) : NULL, *thou = fmt ? PTR(WCHAR, ce_u32(fmt + 16)) : NULL;
    unsigned negative_order = fmt ? ce_u32(fmt + 20) : 1;
    static const WCHAR dot[] = {'.', 0}, comma[] = {',', 0};
    if (!dec)
        dec = dot;
    if (!thou)
        thou = comma;
    if (!value) {
        ce_error = 87;
        return 0;
    }
    bool negative = *value == '-';
    if (negative)
        value++;
    char whole[64], frac[64];
    unsigned nw = 0, nf = 0;
    while (*value >= '0' && *value <= '9' && nw < sizeof(whole) - 1)
        whole[nw++] = (char)*value++;
    if (*value == '.')
        for (value++; *value >= '0' && *value <= '9' && nf < sizeof(frac) - 1;)
            frac[nf++] = (char)*value++;
    if (*value) {
        ce_error = 87;
        return 0;
    }
    /* Round to `digits` decimals. */
    while (nf < digits && nf < sizeof(frac) - 1)
        frac[nf++] = '0';
    if (nf > digits && frac[digits] >= '5') {
        int i = (int)digits - 1;
        for (; i >= 0 && frac[i] == '9'; i--)
            frac[i] = '0';
        if (i >= 0)
            frac[i]++;
        else {
            int j = (int)nw - 1;
            for (; j >= 0 && whole[j] == '9'; j--)
                whole[j] = '0';
            if (j >= 0)
                whole[j]++;
            else if (nw < sizeof(whole) - 1) {
                memmove(whole + 1, whole, nw++);
                whole[0] = '1';
            }
        }
    }
    nf = digits;
    WCHAR out[160];
    unsigned n = 0;
    if (negative && negative_order == 0)
        out[n++] = '(';
    if (negative && negative_order == 1)
        out[n++] = '-';
    if (!nw && leading)
        out[n++] = '0';
    for (unsigned i = 0; i < nw && n < 120; i++) {
        out[n++] = (WCHAR)whole[i];
        unsigned left = nw - 1 - i;
        if (group && left && !(left % group))
            for (const WCHAR *t = thou; *t && n < 120; t++)
                out[n++] = *t;
    }
    if (nf) {
        for (const WCHAR *t = dec; *t && n < 130; t++)
            out[n++] = *t;
        for (unsigned i = 0; i < nf && n < 150; i++)
            out[n++] = (WCHAR)frac[i];
    }
    if (negative && negative_order == 0)
        out[n++] = ')';
    out[n++] = 0;
    if (!a5)
        return n;
    if (a5 < n) {
        ce_error = 122;
        return 0;
    }
    memcpy(PTR(void, a4), out, n * 2);
    return n;
}

/* ---- messages and memory ------------------------------------------------- */

/* FormatMessageW(flags, source, id, language, buffer, size, arguments):
 * FROM_STRING with %1..%99 inserts (strings unless !fmt!), FROM_SYSTEM as
 * "Error <id>", ALLOCATE_BUFFER via LocalAlloc. */
CE_API(FormatMessageW)
{
    WCHAR text[1024];
    unsigned n = 0;
    if (a0 & 0x400) { /* FORMAT_MESSAGE_FROM_STRING */
        const WCHAR *s = PTR(WCHAR, a1);
        const CW *args = PTR(CW, a6);
        for (; s && *s && n + 1 < ARRAY_SIZE(text); s++) {
            if (*s != '%' || (a0 & 0x200)) {
                text[n++] = *s;
                continue;
            }
            s++;
            if (*s >= '1' && *s <= '9') {
                unsigned index = *s - '0';
                if (s[1] >= '0' && s[1] <= '9')
                    index = index * 10 + (*++s - '0');
                if (s[1] == '!') /* !format! is taken as a string */
                    for (s += 2; *s && *s != '!'; s++)
                        ;
                const WCHAR *insert = args ? PTR(WCHAR, args[index - 1]) : NULL;
                for (; insert && *insert && n + 1 < ARRAY_SIZE(text); insert++)
                    text[n++] = *insert;
            } else if (*s == 'n') {
                text[n++] = '\r';
                if (n + 1 < ARRAY_SIZE(text))
                    text[n++] = '\n';
            } else if (*s) {
                text[n++] = *s;
            } else {
                break;
            }
        }
    } else if (a0 & 0x1000) { /* FORMAT_MESSAGE_FROM_SYSTEM */
        char line[32];
        snprintf(line, sizeof(line), "Error %u", (unsigned)a2);
        ce_widen(text, ARRAY_SIZE(text), line);
        n = ce_wlen(text);
    } else {
        ce_error = 87;
        return 0;
    }
    text[n] = 0;
    if (a0 & 0x100) { /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
        CW block = ce_LocalAlloc(0, (MAX(n + 1, a5)) * 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        if (!block)
            return 0;
        memcpy(PTR(void, block), text, (n + 1) * 2);
        *PTR(CW, a4) = block;
        return n;
    }
    if (n + 1 > a5) {
        ce_error = 122;
        return 0;
    }
    memcpy(PTR(void, a4), text, (n + 1) * 2);
    return n;
}
/* One flat address space, readable and writable: VirtualProtect reports
 * PAGE_READWRITE and changes nothing; VirtualQuery a committed page. */
CE_API(VirtualProtect)
{
    if (a3)
        *PTR(CW, a3) = 4;
    return 1;
}
CE_API(VirtualQuery)
{
    if (!a1 || a2 < 28)
        return 0;
    CW *m = PTR(CW, a1);
    m[0] = a0 & ~0xfffu;     /* BaseAddress */
    m[1] = m[0];             /* AllocationBase */
    m[2] = 4;                /* AllocationProtect: PAGE_READWRITE */
    m[3] = 0x1000;           /* RegionSize */
    m[4] = 0x1000;           /* State: MEM_COMMIT */
    m[5] = 4;                /* Protect */
    m[6] = 0x20000;          /* Type: MEM_PRIVATE */
    return 28;
}
CE_API(FlushInstructionCache) { return 1; }
CE_API(CompactAllHeaps) { return 1; }
CE_API(GetCallerProcess) { return 0x42; }
CE_API(MapPtrToProcess) { return a0; }
CE_API(SetProcPermissions) { return 1; }
CE_API(SetObjectOwner) { return 1; }
/* GetStoreInformation(STORE_INFORMATION *): the size and free space of
 * the RAM volume that holds CE files written under Glaze. */
CE_API(GetStoreInformation)
{
    struct fs_info info;
    if (!a0 || fs_statvfs("/", &info)) {
        ce_error = 87;
        return 0;
    }
    PTR(CW, a0)[0] = info.total;
    PTR(CW, a0)[1] = info.total > info.used ? info.total - info.used : 0;
    return 1;
}
CE_API(LoadLibraryExW)
{
    return ce_LoadLibraryW(a0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
/* CreateFileForMappingW: file-backed mappings are refused by
 * CreateFileMappingW, so this is an ordinary file handle. */
CE_API(CreateFileForMappingW)
{
    return ce_CreateFileW(a0, a1, a2, a3, a4, a5, a6, 0, 0, 0, 0, 0);
}

/* ---- locale --------------------------------------------------------------- */

CE_API(GetSystemDefaultLangID) { return 0x409; }
CE_API(IsValidLocale) { return a0 == 0x409 || a0 == 0x400 || a0 == 0x800 || a0 == 0; }
/* EnumSystemCodePagesW(callback, flags): only 1252 is installed. */
CE_API(EnumSystemCodePagesW)
{
    static const WCHAR page[] = {'1', '2', '5', '2', 0};
    if (!a0) {
        ce_error = 87;
        return 0;
    }
    ce_call(a0, WORD(page), 0, 0, 0);
    return 1;
}

/* ---- serial ports: the BE-300's cradle port isn't a CE COM device here --- */

CE_API(GetCommState)
{
    uint8_t *dcb = PTR(uint8_t, a1);
    if (!dcb) {
        ce_error = 87;
        return 0;
    }
    memset(dcb + 4, 0, 24);
    ce_put32(dcb, 28);
    ce_put32(dcb + 4, 115200);
    ce_put32(dcb + 8, 1); /* fBinary */
    dcb[18] = 8;          /* ByteSize */
    return 1;
}
CE_API(SetCommState) { return 1; }
CE_API(GetCommTimeouts)
{
    if (a1)
        memset(PTR(void, a1), 0, 20);
    return 1;
}
CE_API(SetCommTimeouts) { return 1; }
CE_API(EscapeCommFunction) { return 1; }
CE_API(GetCommModemStatus)
{
    if (a1)
        *PTR(CW, a1) = 0;
    return 1;
}

/* ---- the system shell's hooks: Glaze's SpringBoard is the shell --------- */

CE_API(SystemIdleTimerReset) { return 0; }
CE_API(SetCleanRebootFlag) { return 0; }
CE_API(RegRestoreFile)
{
    ce_error = 5;
    return 0;
}
CE_API(GwesPowerOffSystem) { return 0; }
CE_API(EnableEUDC) { return 1; }
CE_API(AddFontResourceW) { return 1; }
CE_API(RemoveFontResourceW) { return 1; }
CE_API(RegisterTaskBar) { return 1; }
CE_API(SignalStarted) { return 0; }
CE_API(SystemStarted) { return 1; }
/* A replacement shell's own API sets are never called by Glaze apps. */
CE_API(CreateAPISet) { return 0x7e000001u; }
CE_API(RegisterAPISet) { return 1; }
/* GetForegroundInfo(GET_FOREGROUND_INFO *): nothing in the foreground. */
CE_API(GetForegroundInfo)
{
    if (a0)
        memset(PTR(void, a0), 0, 44);
    return 1;
}
CE_API(CeRunAppAtEvent) { return 1; }
CE_API(mouse_event) { return 0; }
/* PostKeybdMessage(window or -1, vk, state, count, shifts, characters):
 * the characters arrive as WM_CHAR. */
CE_API(PostKeybdMessage)
{
    CW window = a0 == 0xffffffffu || !a0 ? ce_GetFocus(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) : a0;
    const CW *chars = PTR(CW, a5);
    for (unsigned i = 0; chars && i < a3 && i < 64; i++)
        ce_post(window, 0x102, chars[i] & 0xffff, 1);
    return 1;
}
CE_API(GetDoubleClickTime) { return 500; }
/* GetCPInfo(code page, CPINFO): Windows-1252, one byte per character. */
CE_API(GetCPInfo)
{
    if (!a1 || (a0 > 2 && a0 != 1252)) {
        ce_error = 87;
        return 0;
    }
    uint8_t *p = PTR(uint8_t, a1);
    memset(p, 0, 18);
    ce_put32(p, 1);
    p[4] = '?';
    return 1;
}
CE_API(_vsnprintf)
{
    char text[1024];
    int n = narrow_format(text, sizeof(text), PTR(char, a2), PTR(CW, a3), 10, (a3 >> 2) & 1);
    if (n < 0)
        return (CW)-1;
    if ((CW)n >= a1) {
        memcpy(PTR(void, a0), text, a1);
        return (CW)-1; /* truncated, unterminated, as Microsoft's CRT does */
    }
    memcpy(PTR(void, a0), text, (unsigned)n + 1);
    return (CW)n;
}
CE_API(strpbrk)
{
    const char *s = PTR(char, a0), *accept = PTR(char, a1);
    for (; s && *s; s++)
        if (strchr(accept, *s))
            return WORD(s);
    return 0;
}
/* GetStringTypeExW/GetStringTypeW(..., CT_CTYPE1 1, text, count, types). */
static CW string_types(CW kind, CW text, CW count, CW out)
{
    const WCHAR *s = PTR(WCHAR, text);
    uint16_t *t = PTR(uint16_t, out);
    if (kind != 1 || !s || !t) {
        ce_error = kind != 1 ? 50 : 87;
        return 0;
    }
    unsigned n = count == 0xffffffffu ? ce_wlen(s) + 1 : count;
    for (unsigned i = 0; i < n; i++) {
        unsigned c = s[i], bits = 0;
        bool upper = (c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7);
        bool lower = (c >= 'a' && c <= 'z') || (c >= 0xdf && c <= 0xff && c != 0xf7);
        if (upper) bits |= 0x101;
        if (lower) bits |= 0x102;
        if (c >= '0' && c <= '9') bits |= 4 | 0x80;
        if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) bits |= 0x80;
        if (c == ' ' || (c >= 9 && c <= 13)) bits |= 8;
        if (c == ' ' || c == 9) bits |= 0x40;
        if (c < 32 || c == 127) bits |= 0x20;
        if (c > 32 && c < 127 && !(bits & 0x107)) bits |= 0x10;
        t[i] = (uint16_t)bits;
    }
    return 1;
}
CE_API(GetStringTypeExW) { return string_types(a1, a2, a3, a4); }
CE_API(GetStringTypeW) { return string_types(a0, a1, a2, a3); }
/* MapVirtualKeyW(code, type): virtual keys to characters (2) and back;
 * scan codes aren't modelled. */
CE_API(MapVirtualKeyW)
{
    if (a1 == 2)
        return (a0 >= 'A' && a0 <= 'Z') || (a0 >= '0' && a0 <= '9') || a0 == ' ' ? a0 : 0;
    return 0;
}
CE_API(QueryPerformanceCounter)
{
    if (!a0)
        return 0;
    ce_put32(PTR(void, a0), gz_time_ms());
    ce_put32(PTR(uint8_t, a0) + 4, 0);
    return 1;
}
CE_API(QueryPerformanceFrequency)
{
    if (!a0)
        return 0;
    ce_put32(PTR(void, a0), 1000);
    ce_put32(PTR(uint8_t, a0) + 4, 0);
    return 1;
}
CE_API(CeGetRandomSeed) { return ce_Random(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) ^ gz_time_ms(); }
CE_API(IsEncryptionPermitted) { return 1; }
CE_API(FlushViewOfFile) { return 1; }
CE_API(DisableThreadLibraryCalls) { return 1; }
CE_API(GetMouseMovePoints) { return 0; }
CE_API(CeEventHasOccurred) { return 0; }
CE_API(ImmEscapeW) { return 0; }
CE_API(GetCurrentPermissions) { return 1; }
CE_API(CeSetThreadPriority) { return 1; }
CE_API(SetCommMask) { return 1; }
CE_API(WaitCommEvent)
{
    ce_error = 6;
    return 0;
}
/* Object-store database volumes: the object store is the only one, so
 * the *Ex calls act on it whatever volume GUID they name. */
CE_API(CeCreateDatabase);
CE_API(CeOpenDatabase);
CE_API(CeDeleteDatabase);
CE_API(CeOidGetInfo);
CE_API(CeSetDatabaseInfo);
CE_API(CeFindFirstDatabase);
CE_API(CeFindNextDatabase);
CE_API(CeMountDBVol)
{
    if (a0)
        memset(PTR(void, a0), 0xff, 16); /* the object store's GUID */
    return 1;
}
CE_API(CeUnmountDBVol) { return 1; }
CE_API(CeFreeNotification) { return 1; }
CE_API(CeGetReplChangeBitsEx) { return 0; }
/* CeCreateDatabaseEx(volume, CEDBASEINFO): its name, type and sorts. */
CE_API(CeCreateDatabaseEx)
{
    const uint8_t *info = PTR(uint8_t, a1);
    if (!info) {
        ce_error = 87;
        return 0;
    }
    unsigned sorts = ce_u16(info + 74);
    return ce_CeCreateDatabase(WORD(info + 4), ce_u32(info + 68), sorts, sorts ? WORD(info + 88) : 0, 0, 0, 0, 0, 0,
                               0, 0, 0);
}
CE_API(CeOpenDatabaseEx) { return ce_CeOpenDatabase(a1, a2, a3, a4, 0, 0, 0, 0, 0, 0, 0, 0); }
CE_API(CeDeleteDatabaseEx) { return ce_CeDeleteDatabase(a1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
CE_API(CeOidGetInfoEx) { return ce_CeOidGetInfo(a1, a2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
CE_API(CeSetDatabaseInfoEx) { return ce_CeSetDatabaseInfo(a1, a2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
CE_API(CeFindFirstDatabaseEx) { return ce_CeFindFirstDatabase(a1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
CE_API(CeFindNextDatabaseEx) { return ce_CeFindNextDatabase(a0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
/* Battery driver queries (Power applets): Glaze's battery reports bands,
 * so three main levels and one backup level; no change notifications. */
CE_API(BatteryDrvrGetLevels) { return 3 | 1u << 16; }
CE_API(BatteryDrvrSupportsChangeNotification) { return 0; }
CE_API(BatteryGetLifeTimeInfo)
{
    if (a0)
        memset(PTR(void, a0), 0, 16);
    if (a1)
        *PTR(CW, a1) = 0;
    if (a2)
        *PTR(CW, a2) = 0;
    return 1;
}
CE_API(EnumPnpIds)
{
    ce_error = 259;
    return 0;
}
/* KeybdGetDeviceInfo(index, out): no CE keyboard driver answers. */
CE_API(KeybdGetDeviceInfo)
{
    ce_error = 50;
    return 0;
}
CE_API(NotifyWinUserSystem) { return 0; }
/* The SIP has one input method, Glaze's keyboard, with no COM CLSID. */
CE_API(SipStatus) { return 1; /* SIP_STATUS_AVAILABLE */ }
CE_API(SipGetCurrentIM)
{
    if (!a0)
        return 0;
    memset(PTR(void, a0), 0, 16);
    return 1;
}
CE_API(SipSetCurrentIM) { return 1; }
CE_API(SipEnumIM) { return 0; }

/* ---- shortcuts and special folders -------------------------------------- */

/* A CE shortcut file is "<length>#<target>" in the ANSI code page. */
CE_API(SHCreateShortcut)
{
    const WCHAR *target = PTR(WCHAR, a1);
    if (!a0 || !target) {
        ce_error = 87;
        return 0;
    }
    char text[FS_PATH_MAX + 16];
    unsigned len = ce_wlen(target);
    int n = snprintf(text, sizeof(text), "%u#", len);
    for (unsigned i = 0; i < len && n + 1 < (int)sizeof(text); i++) {
        int c = ce_cp1252_encode(target[i]);
        text[n++] = (char)(c < 0 ? '?' : c);
    }
    CW h = ce_CreateFileW(a0, 0x40000000u, 0, 0, 2, 0x80, 0, 0, 0, 0, 0, 0); /* CREATE_ALWAYS */
    if (h == 0xffffffffu)
        return 0;
    CW wrote = 0;
    bool ok = ce_WriteFile(h, WORD(text), (CW)n, WORD(&wrote), 0, 0, 0, 0, 0, 0, 0, 0) && wrote == (CW)n;
    ce_CloseHandle(h, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    return ok;
}
CE_API(SHGetShortcutTarget)
{
    if (!a0 || !a1 || !a2) {
        ce_error = 87;
        return 0;
    }
    char text[FS_PATH_MAX + 16];
    CW h = ce_CreateFileW(a0, 0x80000000u, 1, 0, 3, 0x80, 0, 0, 0, 0, 0, 0); /* OPEN_EXISTING */
    if (h == 0xffffffffu)
        return 0;
    CW got = 0;
    bool ok = ce_ReadFile(h, WORD(text), sizeof(text) - 1, WORD(&got), 0, 0, 0, 0, 0, 0, 0, 0);
    ce_CloseHandle(h, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (!ok)
        return 0;
    text[got] = 0;
    char *hash = strchr(text, '#');
    if (!hash) {
        ce_error = 13;
        return 0;
    }
    WCHAR *out = PTR(WCHAR, a1);
    unsigned n = 0;
    for (const char *p = hash + 1; *p && *p != '\r' && *p != '\n' && n + 1 < a2; p++)
        out[n++] = ce_cp1252_decode((uint8_t)*p);
    out[n] = 0;
    return 1;
}
/* SHGetSpecialFolderPath(window, path, CSIDL, create): CE's folders. */
CE_API(SHGetSpecialFolderPath)
{
    static const struct { unsigned csidl; const char *path; } folders[] = {
        {0x00, "\\Windows\\Desktop"}, {0x02, "\\Windows\\Programs"}, {0x05, "\\My Documents"},
        {0x06, "\\Windows\\Favorites"}, {0x07, "\\Windows\\StartUp"}, {0x08, "\\Windows\\Recent"},
        {0x0b, "\\Windows\\Start Menu"}, {0x10, "\\Windows\\Desktop"}, {0x14, "\\Windows\\Fonts"},
        {0x1a, "\\Application Data"}, {0x26, "\\Program Files"}, {0x24, "\\Windows"}};
    for (unsigned i = 0; a1 && i < ARRAY_SIZE(folders); i++)
        if (folders[i].csidl == (a2 & 0xff)) {
            ce_widen(PTR(WCHAR, a1), 260, folders[i].path);
            return 1;
        }
    ce_error = 87;
    return 0;
}

/* ---- TOOLHELP.DLL: this CE process, its main thread and its modules ------ */

#define TH32CS_SNAPHEAPLIST 1
#define TH32CS_SNAPPROCESS 2
#define TH32CS_SNAPTHREAD 4
#define TH32CS_SNAPMODULE 8
static struct { CW handle, flags, process, thread, module; } snapshots[4];
static CW snapshot_serial;
CE_API(CreateToolhelp32Snapshot)
{
    for (unsigned i = 0; i < ARRAY_SIZE(snapshots); i++)
        if (!snapshots[i].handle) {
            snapshots[i].handle = 0x7f000000u | (++snapshot_serial & 0xffffff);
            snapshots[i].flags = a0;
            snapshots[i].process = snapshots[i].thread = snapshots[i].module = 0;
            return snapshots[i].handle;
        }
    ce_error = 8;
    return 0xffffffffu;
}
static unsigned snapshot(CW h)
{
    for (unsigned i = 0; i < ARRAY_SIZE(snapshots); i++)
        if (h && snapshots[i].handle == h)
            return i;
    return ARRAY_SIZE(snapshots);
}
CE_API(CloseToolhelp32Snapshot)
{
    unsigned i = snapshot(a0);
    if (i == ARRAY_SIZE(snapshots))
        return 0;
    snapshots[i].handle = 0;
    return 1;
}
/* The CE path of the running program, e.g. \Nand Disk\Program Files\x.exe. */
static void exe_name(WCHAR *out, unsigned capacity, bool leaf)
{
    const char *name = ce_source_name();
    if (!name)
        name = "\\Windows\\program.exe";
    const char *p = leaf && strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
    ce_widen(out, capacity, p);
}
/* PROCESSENTRY32: dwSize, cntUsage, th32ProcessID, th32DefaultHeapID,
 * th32ModuleID, cntThreads, th32ParentProcessID, pcPriClassBase, dwFlags,
 * szExeFile[260], th32MemoryBase, th32AccessKey. */
static CW process_entry(CW h, CW out, bool first)
{
    unsigned i = snapshot(h);
    uint8_t *p = PTR(uint8_t, out);
    if (i == ARRAY_SIZE(snapshots) || !p || !(snapshots[i].flags & TH32CS_SNAPPROCESS) || ce_u32(p) < 564) {
        ce_error = 87;
        return 0;
    }
    if (first)
        snapshots[i].process = 0;
    if (snapshots[i].process++) {
        ce_error = 18;
        return 0;
    }
    memset(p + 4, 0, 560);
    ce_put32(p + 4, 1);
    ce_put32(p + 8, 0x42);
    ce_put32(p + 20, 1);
    ce_put32(p + 28, 3);
    exe_name(PTR(WCHAR, out + 36), 260, true);
    return 1;
}
CE_API(Process32First) { return process_entry(a0, a1, true); }
CE_API(Process32Next) { return process_entry(a0, a1, false); }
/* THREADENTRY32: dwSize, cntUsage, th32ThreadID, th32OwnerProcessID,
 * tpBasePri, tpDeltaPri, dwFlags, th32AccessKey, th32CurrentProcessID. */
static CW thread_entry(CW h, CW out, bool first)
{
    unsigned i = snapshot(h);
    uint8_t *p = PTR(uint8_t, out);
    if (i == ARRAY_SIZE(snapshots) || !p || !(snapshots[i].flags & TH32CS_SNAPTHREAD) || ce_u32(p) < 36) {
        ce_error = 87;
        return 0;
    }
    if (first)
        snapshots[i].thread = 0;
    if (snapshots[i].thread++) {
        ce_error = 18;
        return 0;
    }
    memset(p + 4, 0, 32);
    ce_put32(p + 4, 1);
    ce_put32(p + 8, 1);
    ce_put32(p + 12, 0x42);
    ce_put32(p + 16, 3);
    ce_put32(p + 32, 0x42);
    return 1;
}
CE_API(Thread32First) { return thread_entry(a0, a1, true); }
CE_API(Thread32Next) { return thread_entry(a0, a1, false); }
/* MODULEENTRY32: dwSize, th32ModuleID, th32ProcessID, GlblcntUsage,
 * ProccntUsage, modBaseAddr, modBaseSize, hModule, szModule[256],
 * szExePath[260], dwFlags. */
static CW module_entry(CW h, CW out, bool first)
{
    unsigned i = snapshot(h);
    uint8_t *p = PTR(uint8_t, out);
    if (i == ARRAY_SIZE(snapshots) || !p || !(snapshots[i].flags & TH32CS_SNAPMODULE) || ce_u32(p) < 1064) {
        ce_error = 87;
        return 0;
    }
    if (first)
        snapshots[i].module = 0;
    CW base = 0;
    uint32_t size = 0;
    const char *name = NULL;
    if (!ce_module_at(snapshots[i].module++, &base, &size, &name)) {
        ce_error = 18;
        return 0;
    }
    memset(p + 4, 0, 1060);
    ce_put32(p + 4, base);
    ce_put32(p + 8, 0x42);
    ce_put32(p + 12, 1);
    ce_put32(p + 16, 1);
    ce_put32(p + 20, base);
    ce_put32(p + 24, size);
    ce_put32(p + 28, base);
    if (name) {
        ce_widen(PTR(WCHAR, out + 32), 256, name);
        char path[FS_PATH_MAX];
        const char *where = ce_source_module_name(name);
        snprintf(path, sizeof(path), "%s", where ? where : name);
        ce_widen(PTR(WCHAR, out + 32 + 512), 260, path);
    } else {
        exe_name(PTR(WCHAR, out + 32), 256, true);
        exe_name(PTR(WCHAR, out + 32 + 512), 260, false);
    }
    return 1;
}
CE_API(Module32First) { return module_entry(a0, a1, true); }
CE_API(Module32Next) { return module_entry(a0, a1, false); }
/* Heaps aren't walked: CE apps' heaps are Glaze's malloc. */
CE_API(Heap32ListFirst)
{
    ce_error = 18;
    return 0;
}

static const struct ce_export exports[] = {
    CE_EXPORT(wcstol), CE_EXPORT(wcstoul), CE_EXPORT(wcscspn), CE_EXPORT(towlower), CE_EXPORT(_isctype),
    CE_EXPORT(_ultow), CE_EXPORT(_stricmp), CE_EXPORT(memchr), CE_EXPORT(CharNextW), CE_EXPORT(CharPrevW),
    CE_EXPORT(IsDBCSLeadByte), CE_EXPORT(sprintf), CE_EXPORT(vprintf), CE_EXPORT(sscanf),
    {"modf", (void *)modf_}, {"div", (void *)div_}, {"__dpcmp", (void *)dpcmp}, {"__fpcmp", (void *)fpcmp},
    CE_EXPORT(Random), CE_EXPORT(CompareFileTime), CE_EXPORT(GetNumberFormatW), CE_EXPORT(FormatMessageW),
    CE_EXPORT(VirtualProtect), CE_EXPORT(VirtualQuery), CE_EXPORT(FlushInstructionCache),
    CE_EXPORT(CompactAllHeaps), CE_EXPORT(GetCallerProcess), CE_EXPORT(MapPtrToProcess),
    CE_EXPORT(SetProcPermissions), CE_EXPORT(SetObjectOwner), CE_EXPORT(GetStoreInformation),
    CE_EXPORT(LoadLibraryExW), CE_EXPORT(CreateFileForMappingW), CE_EXPORT(GetSystemDefaultLangID),
    {"GetUserDefaultLangID", (void *)ce_GetSystemDefaultLangID}, {"GetUserDefaultLCID", (void *)ce_GetSystemDefaultLangID},
    {"GetSystemDefaultLCID", (void *)ce_GetSystemDefaultLangID}, CE_EXPORT(IsValidLocale),
    CE_EXPORT(EnumSystemCodePagesW), CE_EXPORT(GetCommState), CE_EXPORT(SetCommState),
    CE_EXPORT(GetCommTimeouts), CE_EXPORT(SetCommTimeouts), CE_EXPORT(EscapeCommFunction),
    CE_EXPORT(GetCommModemStatus), CE_EXPORT(SystemIdleTimerReset), CE_EXPORT(SetCleanRebootFlag),
    CE_EXPORT(RegRestoreFile), CE_EXPORT(GwesPowerOffSystem), CE_EXPORT(EnableEUDC), CE_EXPORT(AddFontResourceW),
    CE_EXPORT(RemoveFontResourceW), CE_EXPORT(RegisterTaskBar), CE_EXPORT(SignalStarted), CE_EXPORT(SystemStarted),
    CE_EXPORT(CreateAPISet), CE_EXPORT(RegisterAPISet), CE_EXPORT(GetForegroundInfo), CE_EXPORT(CeRunAppAtEvent),
    CE_EXPORT(mouse_event), CE_EXPORT(PostKeybdMessage), CE_EXPORT(GetDoubleClickTime), CE_EXPORT(SipStatus),
    CE_EXPORT(SipGetCurrentIM), CE_EXPORT(SipSetCurrentIM), CE_EXPORT(SipEnumIM), CE_EXPORT(SHCreateShortcut),
    CE_EXPORT(SHGetShortcutTarget), CE_EXPORT(SHGetSpecialFolderPath), CE_EXPORT(BatteryDrvrGetLevels),
    CE_EXPORT(BatteryDrvrSupportsChangeNotification), CE_EXPORT(BatteryGetLifeTimeInfo), CE_EXPORT(EnumPnpIds),
    CE_EXPORT(KeybdGetDeviceInfo), CE_EXPORT(NotifyWinUserSystem), CE_EXPORT(GetCPInfo), CE_EXPORT(_vsnprintf),
    CE_EXPORT(strpbrk), CE_EXPORT(GetStringTypeExW), CE_EXPORT(GetStringTypeW), CE_EXPORT(MapVirtualKeyW),
    CE_EXPORT(QueryPerformanceCounter), CE_EXPORT(QueryPerformanceFrequency), CE_EXPORT(CeGetRandomSeed),
    CE_EXPORT(IsEncryptionPermitted), CE_EXPORT(FlushViewOfFile), CE_EXPORT(DisableThreadLibraryCalls),
    CE_EXPORT(GetMouseMovePoints), CE_EXPORT(CeEventHasOccurred), CE_EXPORT(ImmEscapeW),
    {"IsDBCSLeadByteEx", (void *)ce_IsDBCSLeadByte}, CE_EXPORT(GetCurrentPermissions), CE_EXPORT(CeSetThreadPriority),
    {"CeGetThreadPriority", (void *)ce_GetCurrentPermissions}, CE_EXPORT(SetCommMask), CE_EXPORT(WaitCommEvent),
    CE_EXPORT(CeMountDBVol), CE_EXPORT(CeUnmountDBVol), CE_EXPORT(CeFreeNotification),
    CE_EXPORT(CeGetReplChangeBitsEx), CE_EXPORT(CeCreateDatabaseEx), CE_EXPORT(CeOpenDatabaseEx),
    CE_EXPORT(CeDeleteDatabaseEx), CE_EXPORT(CeOidGetInfoEx), CE_EXPORT(CeSetDatabaseInfoEx),
    CE_EXPORT(CeFindFirstDatabaseEx), CE_EXPORT(CeFindNextDatabaseEx),
};
static const struct ce_export toolhelp[] = {
    CE_EXPORT(CreateToolhelp32Snapshot), CE_EXPORT(CloseToolhelp32Snapshot), CE_EXPORT(Process32First),
    CE_EXPORT(Process32Next), CE_EXPORT(Thread32First), CE_EXPORT(Thread32Next), CE_EXPORT(Module32First),
    CE_EXPORT(Module32Next), CE_EXPORT(Heap32ListFirst), {"Heap32ListNext", (void *)ce_Heap32ListFirst},
    {"Heap32First", (void *)ce_Heap32ListFirst}, {"Heap32Next", (void *)ce_Heap32ListFirst},
};
void *ce_resolve_extras(const char *dll, const char *name)
{
    const struct ce_export *table = exports;
    unsigned count = ARRAY_SIZE(exports);
    if (!strcasecmp(dll, "toolhelp.dll")) {
        table = toolhelp;
        count = ARRAY_SIZE(toolhelp);
    } else if (strcasecmp(dll, "coredll.dll")) {
        return NULL;
    }
    for (unsigned i = 0; i < count; i++)
        if (!strcmp(name, table[i].name))
            return table[i].address;
    return NULL;
}
