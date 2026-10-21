/* Host validation of the PE boundary: malformed inputs never reach guest code. */
#include "../apps/wince/pe.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t file[2048];
static void put16(uint32_t p, uint16_t v)
{
    file[p] = v;
    file[p + 1] = v >> 8;
}
static void put32(uint32_t p, uint32_t v)
{
    ce_put32(file + p, v);
}
static void fixture(void)
{
    memset(file, 0, sizeof(file));
    file[0] = 'M';
    file[1] = 'Z';
    put32(60, 128);
    put32(128, 0x4550);
    put16(132, 0x166);
    put16(134, 2);
    put16(148, 224);
    put16(150, 2);
    put16(152, 0x10b);
    put32(168, 4096);
    put32(180, 0x10000);
    put32(184, 4096);
    put32(188, 512);
    put32(208, 12288);
    put32(212, 512);
    put16(220, 9);
    put32(224, 65536);
    put32(244, 16);
    put32(384, 512);
    put32(388, 4096);
    put32(392, 512);
    put32(396, 512);
    put32(412, 0x60000020);
    put32(424, 1024);
    put32(428, 8192);
    put32(432, 1024);
    put32(436, 1024);
    put32(452, 0xc0000040);
    /* Import descriptor and its terminating descriptor; two thunk imports. */
    put32(256, 8192);
    put32(260, 40);
    put32(1024, 8256);
    put32(1036, 8288);
    put32(1040, 8272);
    put32(1088, 8320);
    put32(1092, 0x80000021);
    memcpy(file + 1120, "COREDLL.dll", 12);
    memcpy(file + 1154, "LocalAlloc", 11);
}

static unsigned resolved;
static uint32_t resolve(const char *dll, const char *name, uint16_t ordinal, void *arg)
{
    assert(!strcmp(dll, "COREDLL.dll"));
    assert(arg == &resolved);
    if (name)
        assert(!strcmp(name, "LocalAlloc") && !ordinal);
    else
        assert(ordinal == 33);
    return 0x12340000u + ++resolved * 4u;
}

int main(int argc, char **argv)
{
    struct ce_pe p;
    fixture();
    assert(!ce_pe_parse(&p, file, sizeof(file)));
    void *image = malloc(p.size);
    assert(image);
    assert(!ce_pe_copy(&p, image));
    assert(!ce_pe_imports(&p, image, resolve, &resolved) && resolved == 2);
    assert(ce_u32((uint8_t *)image + 8272) == 0x12340004);
    assert(ce_u32((uint8_t *)image + 8276) == 0x12340008);
    assert(!ce_pe_at(&p, image, 4095, 2));
    assert(!ce_pe_at(&p, image, UINT32_MAX, 2));
    assert(!ce_pe_string(&p, image, UINT32_MAX));
    assert(!ce_pe_relocate(&p, image, p.base));
    assert(ce_pe_relocate(&p, image, p.base + 4096));
    free(image);
    /* Three resource-directory levels, a valid payload, and hostile links. */
    fixture();
    put32(264, 8704); put32(268, 256);
    put16(1550, 1); put32(1552, 2); put32(1556, 0x80000020u);
    put16(1582, 1); put32(1584, 7); put32(1588, 0x80000040u);
    put16(1614, 1); put32(1616, 0x409); put32(1620, 96);
    put32(1632, 8832); put32(1636, 4); memcpy(file + 1664, "bmp!", 4);
    assert(!ce_pe_parse(&p, file, sizeof(file)));
    image = malloc(p.size); assert(image); ce_pe_copy(&p, image);
    uint32_t resource_size;
    const void *resource = ce_pe_resource(&p, image, 2, 7, &resource_size);
    assert(resource && resource_size == 4 && !memcmp(resource, "bmp!", 4));
    ce_put32((uint8_t *)image + 8788, 0x80000040u);
    assert(!ce_pe_resource(&p, image, 2, 7, &resource_size));
    ce_put32((uint8_t *)image + 8788, 0x7fffffffu);
    assert(!ce_pe_resource(&p, image, 2, 7, &resource_size));
    free(image);
    /* Truncation, wrong CPU, section overlap, privileged VA, arithmetic wrap. */
    for (unsigned n = 0; n < 512; n++)
        assert(ce_pe_parse(&p, file, n));
    put16(132, 0x1c0);
    assert(ce_pe_parse(&p, file, sizeof(file)));
    fixture();
    put32(428, 4096);
    assert(ce_pe_parse(&p, file, sizeof(file)));
    fixture();
    put32(180, 0x80010000);
    assert(ce_pe_parse(&p, file, sizeof(file)));
    fixture();
    put32(424, UINT32_MAX);
    assert(ce_pe_parse(&p, file, sizeof(file)));
    fixture();
    put32(412, 0xe0000020);
    assert(ce_pe_parse(&p, file, sizeof(file)));
    fixture();
    put32(320, 8192);
    put32(324, 4);
    assert(ce_pe_parse(&p, file, sizeof(file)));
    /* A relocation must update only a validated target within a section. */
    fixture();
    put32(288, 8448);
    put32(292, 12);
    put32(1280, 4096);
    put32(1284, 12);
    put16(1288, 0x3000);
    put32(512, 0x10000);
    assert(!ce_pe_parse(&p, file, sizeof(file)));
    image = malloc(p.size);
    assert(image);
    ce_pe_copy(&p, image);
    assert(!ce_pe_relocate(&p, image, p.base + 4096));
    assert(ce_u32((uint8_t *)image + 4096) == 0x11000);
    ce_put32((uint8_t *)image + 8448, 0xfffff000);
    assert(ce_pe_relocate(&p, image, p.base + 4096));
    free(image);
    /* Parse real local assets when supplied, without committing their bytes. */
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        assert(f);
        assert(!fseek(f, 0, SEEK_END));
        long size = ftell(f);
        assert(size > 0 && size <= CE_PE_MAX_SIZE);
        rewind(f);
        uint8_t *b = malloc((size_t)size);
        assert(b);
        assert(fread(b, 1, (size_t)size, f) == (size_t)size);
        fclose(f);
        const char *err = ce_pe_parse(&p, b, (uint32_t)size);
        if (err)
            fprintf(stderr, "%s: %s\n", argv[i], err);
        assert(!err);
        free(b);
    }
    puts("wince-pe: ALL PASS");
    return 0;
}
