/* Automation strings have a byte-length prefix and a UTF-16 terminator. */
#include "runtime.h"
CE_API(SysAllocStringLen)
{
    if (a1 > 2u * 1024u * 1024u) {
        ce_error = 8;
        return 0;
    }
    uint8_t *p = calloc(1, 4 + a1 * 2 + 2);
    if (!p) {
        ce_error = 8;
        return 0;
    }
    ce_put32(p, a1 * 2);
    if (a0)
        memcpy(p + 4, PTR(void, a0), a1 * 2);
    return WORD(p + 4);
}
CE_API(SysAllocString)
{
    return a0 ? ce_SysAllocStringLen(a0, ce_wlen(PTR(WCHAR, a0)), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) : 0;
}
CE_API(SysFreeString)
{
    if (a0)
        free(PTR(uint8_t, a0) - 4);
    return 0;
}
CE_API(SysStringLen) { return a0 ? ce_u32(PTR(uint8_t, a0) - 4) / 2 : 0; }
/* SysAllocStringByteLen(bytes, length): a BSTR of `length` bytes. */
CE_API(SysAllocStringByteLen)
{
    if (a1 > 4u * 1024u * 1024u) {
        ce_error = 8;
        return 0;
    }
    uint8_t *p = calloc(1, 4 + a1 + 2);
    if (!p)
        return 0;
    ce_put32(p, a1);
    if (a0)
        memcpy(p + 4, PTR(void, a0), a1);
    return WORD(p + 4);
}
CE_API(SysStringByteLen) { return a0 ? ce_u32(PTR(uint8_t, a0) - 4) : 0; }
/* VARIANT: vt (WORD), three reserved WORDs, then the 8-byte value. */
CE_API(VariantInit)
{
    if (a0)
        memset(PTR(void, a0), 0, 16);
    return 0;
}
CE_API(VariantClear)
{
    if (!a0)
        return 0x80070057u;
    uint8_t *v = PTR(uint8_t, a0);
    if (ce_u16(v) == 8) /* VT_BSTR */
        ce_SysFreeString(ce_u32(v + 8), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    memset(v, 0, 16);
    return 0;
}
/* VariantChangeType(dest, source, flags, vt) among VT_I2 2, VT_I4 3,
 * VT_BSTR 8, VT_BOOL 11, VT_UI2 18, VT_UI4 19. */
CE_API(VariantChangeType)
{
    if (!a0 || !a1)
        return 0x80070057u;
    const uint8_t *src = PTR(uint8_t, a1);
    unsigned from = ce_u16(src), to = a3 & 0xfff;
    int64_t value = 0;
    switch (from) {
    case 2: value = (int16_t)ce_u16(src + 8); break;
    case 18: value = ce_u16(src + 8); break;
    case 3: value = (int32_t)ce_u32(src + 8); break;
    case 19: value = ce_u32(src + 8); break;
    case 11: value = ce_u16(src + 8) ? -1 : 0; break;
    case 8: {
        const WCHAR *t = PTR(WCHAR, ce_u32(src + 8));
        bool negative = t && *t == '-';
        if (t && (*t == '-' || *t == '+'))
            t++;
        if (!t || *t < '0' || *t > '9')
            return 0x80020005u; /* DISP_E_TYPEMISMATCH */
        while (*t >= '0' && *t <= '9')
            value = value * 10 + (*t++ - '0');
        if (*t)
            return 0x80020005u;
        if (negative)
            value = -value;
        break;
    }
    default:
        return 0x80020005u;
    }
    uint8_t out[16] = {0};
    ce_put16(out, (uint16_t)to);
    switch (to) {
    case 2: case 18: ce_put16(out + 8, (uint16_t)value); break;
    case 3: case 19: ce_put32(out + 8, (uint32_t)value); break;
    case 11: ce_put16(out + 8, value ? 0xffff : 0); break;
    case 8: {
        char text[24];
        WCHAR wide[24];
        snprintf(text, sizeof(text), "%lld", (long long)value);
        ce_widen(wide, ARRAY_SIZE(wide), text);
        ce_put32(out + 8, ce_SysAllocString(WORD(wide), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
        break;
    }
    default:
        return 0x80020005u;
    }
    if (a0 == a1)
        ce_VariantClear(a0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    memcpy(PTR(void, a0), out, 16);
    return 0;
}
CE_API(VarUI4FromStr)
{
    const WCHAR *t = PTR(WCHAR, a0);
    uint64_t value = 0;
    if (!t || !a3 || *t < '0' || *t > '9')
        return 0x80020005u;
    while (*t >= '0' && *t <= '9')
        value = value * 10 + (*t++ - '0');
    if (*t || value > 0xffffffffull)
        return *t ? 0x80020005u : 0x8002000au; /* DISP_E_OVERFLOW */
    *PTR(CW, a3) = (CW)value;
    return 0;
}
/* No type libraries or rich error objects: Automation servers that need
 * them report it as on a device without them. */
CE_API(DispGetParam) { return 0x80020004u; }      /* DISP_E_PARAMNOTFOUND */
CE_API(LoadTypeLib)
{
    if (a1)
        *PTR(CW, a1) = 0;
    return 0x80029c4au; /* TYPE_E_CANTLOADLIBRARY */
}
CE_API(LoadRegTypeLib)
{
    if (a4)
        *PTR(CW, a4) = 0;
    return 0x80029c4au;
}
CE_API(RegisterTypeLib) { return 0x80029c4au; }
CE_API(CreateTypeLib2)
{
    if (a2)
        *PTR(CW, a2) = 0;
    return 0x80004001u; /* E_NOTIMPL */
}
CE_API(SetErrorInfo) { return 0; }
CE_API(CreateErrorInfo)
{
    if (a0)
        *PTR(CW, a0) = 0;
    return 0x80004001u;
}
static const struct ce_export exports[] = {CE_EXPORT(SysAllocString), CE_EXPORT(SysAllocStringLen),
                                           CE_EXPORT(SysFreeString), CE_EXPORT(SysStringLen),
                                           CE_EXPORT(SysAllocStringByteLen), CE_EXPORT(SysStringByteLen),
                                           CE_EXPORT(VariantInit), CE_EXPORT(VariantClear),
                                           CE_EXPORT(VariantChangeType), CE_EXPORT(VarUI4FromStr),
                                           CE_EXPORT(DispGetParam), CE_EXPORT(LoadTypeLib),
                                           CE_EXPORT(LoadRegTypeLib), CE_EXPORT(RegisterTypeLib),
                                           CE_EXPORT(CreateTypeLib2), CE_EXPORT(SetErrorInfo),
                                           CE_EXPORT(CreateErrorInfo)};
/* COM in-process servers, as OLE32 activates them: the class's
 * HKCR\CLSID\{clsid}\InprocServer32 names a DLL (the Hygglo character
 * recognizer's is \Program Disk\hygglo.dll), whose DllGetClassObject gives
 * an IClassFactory that creates the object. OLE32 itself (in NK) is not
 * loaded; other classes are unregistered.
 * https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-cocreateinstance */
CE_API(CoInitializeEx)
{
    return 0; /* S_OK */
}
CE_API(CoUninitialize)
{
    return 0;
}
static void guid_text(char out[39], const uint8_t *g)
{
    snprintf(out, 39, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned)ce_u32(g), ce_u16(g + 4),
             ce_u16(g + 6), g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}
/* CoCreateInstance(clsid, outer, context, iid, out). */
CE_API(CoCreateInstance)
{
    static const uint8_t iid_class_factory[16] = {1, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
    if (!a0 || !a3 || !a4)
        return 0x80070057u; /* E_INVALIDARG */
    *PTR(CW, a4) = 0;
    char clsid[39], key[80], server[260];
    WCHAR path[260];
    guid_text(clsid, PTR(uint8_t, a0));
    snprintf(key, sizeof(key), "HKCR\\CLSID\\%s\\InprocServer32", clsid);
    if (!ce_registry_string(key, "", path, ARRAY_SIZE(path)))
        return 0x80040154u; /* REGDB_E_CLASSNOTREG */
    ce_narrow(server, sizeof(server), path);
    const char *leaf = strrchr(server, '\\');
    CW module = ce_load_library(leaf ? leaf + 1 : server);
    CW get = module ? ce_find_symbol(module, "DllGetClassObject") : 0;
    if (!get) {
        printf("wince: COM server %s for %s did not load\n", server, clsid);
        return 0x8007007eu; /* HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND) */
    }
    CW factory = 0;
    CW hr = ce_call(get, a0, WORD(iid_class_factory), WORD(&factory), 0);
    if (hr & 0x80000000u || !factory)
        return hr ? hr : 0x80004005u;
    const CW *vtable = PTR(CW, *PTR(CW, factory));
    CW create = vtable[3], release = vtable[2];
    hr = ce_call(create, factory, a1, a3, a4); /* IClassFactory::CreateInstance */
    ce_call(release, factory, 0, 0, 0);
    return hr;
}
/* The COM task allocator is Glaze's heap. */
CE_API(CoTaskMemAlloc) { return WORD(malloc(a0 ? a0 : 1)); }
CE_API(CoTaskMemRealloc) { return WORD(realloc(PTR(void, a0), a1 ? a1 : 1)); }
CE_API(CoTaskMemFree)
{
    free(PTR(void, a0));
    return 0;
}
/* StringFromGUID2(guid, buffer, capacity): "{...}" and its length with the
 * terminator, or 0 if it doesn't fit. */
CE_API(StringFromGUID2)
{
    char text[39];
    if (!a0 || !a1 || a2 < 39)
        return 0;
    guid_text(text, PTR(uint8_t, a0));
    ce_widen(PTR(WCHAR, a1), 39, text);
    return 39;
}
CE_API(StringFromCLSID)
{
    if (!a0 || !a1)
        return 0x80070057u;
    CW out = ce_CoTaskMemAlloc(78, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (!out)
        return 0x8007000eu; /* E_OUTOFMEMORY */
    ce_StringFromGUID2(a0, out, 39, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    *PTR(CW, a1) = out;
    return 0;
}
static bool parse_guid(const WCHAR *t, uint8_t g[16])
{
    static const int8_t at[16] = {7, 5, 3, 1, 12, 10, 17, 15, 20, 22, 25, 27, 29, 31, 33, 35};
    if (!t || ce_wlen(t) != 38 || t[0] != '{' || t[37] != '}' || t[9] != '-' || t[14] != '-' || t[19] != '-' ||
        t[24] != '-')
        return false;
    for (unsigned i = 0; i < 16; i++) {
        unsigned v = 0;
        for (unsigned k = 0; k < 2; k++) {
            unsigned c = t[at[i] + k], d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : 16;
            if (d > 15)
                return false;
            v = v * 16 + d;
        }
        g[i] = (uint8_t)v;
    }
    return true;
}
CE_API(CLSIDFromString)
{
    if (!a1 || !parse_guid(PTR(WCHAR, a0), PTR(uint8_t, a1)))
        return 0x800401f3u; /* CO_E_CLASSSTRING */
    return 0;
}
/* CLSIDFromProgID: HKCR\<progid>\CLSID. */
CE_API(CLSIDFromProgID)
{
    char progid[128], key[160];
    WCHAR clsid[64];
    if (!a0 || !a1)
        return 0x80070057u;
    ce_narrow(progid, sizeof(progid), PTR(WCHAR, a0));
    snprintf(key, sizeof(key), "HKCR\\%s\\CLSID", progid);
    if (!ce_registry_string(key, "", clsid, ARRAY_SIZE(clsid)) || !parse_guid(clsid, PTR(uint8_t, a1)))
        return 0x800401f3u;
    return 0;
}
/* Structured storage (compound files) and OLE advise holders aren't
 * provided: MFC's document code reports the error to the app. */
CE_API(StgCreateDocfile)
{
    if (a3)
        *PTR(CW, a3) = 0;
    return 0x80004001u;
}
CE_API(StgOpenStorage)
{
    if (a5)
        *PTR(CW, a5) = 0;
    return 0x80030002u; /* STG_E_FILENOTFOUND */
}
CE_API(ReadClassStg) { return 0x80004001u; }
CE_API(CreateOleAdviseHolder)
{
    if (a0)
        *PTR(CW, a0) = 0;
    return 0x8007000eu;
}
static const struct ce_export com[] = {CE_EXPORT(CoInitializeEx), CE_EXPORT(CoUninitialize),
                                       CE_EXPORT(CoCreateInstance), CE_EXPORT(CoTaskMemAlloc),
                                       CE_EXPORT(CoTaskMemRealloc), CE_EXPORT(CoTaskMemFree),
                                       CE_EXPORT(StringFromGUID2), CE_EXPORT(StringFromCLSID),
                                       CE_EXPORT(CLSIDFromString), CE_EXPORT(CLSIDFromProgID),
                                       CE_EXPORT(StgCreateDocfile), CE_EXPORT(StgOpenStorage),
                                       CE_EXPORT(ReadClassStg), {"WriteClassStg", (void *)ce_ReadClassStg},
                                       {"ReadClassStm", (void *)ce_ReadClassStg},
                                       {"WriteClassStm", (void *)ce_ReadClassStg}, CE_EXPORT(CreateOleAdviseHolder)};
void *ce_resolve_ole(const char *dll, const char *name)
{
    bool automation = !strcasecmp(dll, "oleaut32.dll");
    if (!automation && strcasecmp(dll, "ole32.dll"))
        return NULL;
    const struct ce_export *table = automation ? exports : com;
    unsigned count = automation ? ARRAY_SIZE(exports) : ARRAY_SIZE(com);
    for (unsigned i = 0; i < count; i++)
        if (!strcmp(name, table[i].name))
            return table[i].address;
    return NULL;
}
