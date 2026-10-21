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
void *ce_resolve_ole(const char *dll, const char *name)
{
    if (strcasecmp(dll, "oleaut32.dll"))
        return NULL;
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return NULL;
}
