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
/* COM: the runtime registers no COM servers, and the original OS's (OLE32
 * in NK) are not loaded, so every class is unregistered. Initialization
 * itself succeeds, as it does on CE. */
CE_API(CoInitializeEx)
{
    return 0; /* S_OK */
}
CE_API(CoUninitialize)
{
    return 0;
}
CE_API(CoCreateInstance)
{
    if (a4)
        *PTR(CW, a4) = 0;
    return 0x80040154u; /* REGDB_E_CLASSNOTREG */
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
