/* C runtime and small system queries exported by COREDLL beyond string.c. */
#include "runtime.h"

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

static const struct ce_export exports[] = {
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
