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
static const struct ce_export exports[] = {CE_EXPORT(SysAllocString), CE_EXPORT(SysAllocStringLen),
                                           CE_EXPORT(SysFreeString), CE_EXPORT(SysStringLen)};
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
static const struct ce_export com[] = {CE_EXPORT(CoInitializeEx), CE_EXPORT(CoUninitialize),
                                       CE_EXPORT(CoCreateInstance)};
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
