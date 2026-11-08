#include "../../apps/wince/pe.h"
#include "../../apps/wince/registry.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    struct ce_registry *r = calloc(1, sizeof(*r)), *copy = calloc(1, sizeof(*copy));
    assert(r && copy);
    assert(ce_reg_exists(r, "hkcu"));
    assert(!ce_reg_create(r, "HKCU\\Software\\Glaze\\Empty"));
    assert(!strcmp(ce_reg_child(r, "HKCU", 0), "Software"));
    assert(!strcmp(ce_reg_child(r, "hkcu\\software", 0), "Glaze"));
    assert(!ce_reg_child(r, "HKCU", 1));
    assert(ce_reg_create(r, "HKCU\\\\Broken") == 87);
    assert(ce_reg_create(r, "Other\\Broken") == 87);
    assert(ce_reg_delete(r, "HKCU\\Software") == 5);
    const unsigned char binary[] = {0, 255, 1, 0, 4};
    assert(!ce_reg_set(r, "HKCU\\Software\\Glaze", "Blob", 3, binary, sizeof(binary)));
    assert(!ce_reg_set(r, "hkcu\\software\\glaze", "", 1, "test", 5));
    assert(!ce_reg_set(r, "HKCU", "Root", 4, binary, 4));
    assert(r->value_count == 3);
    assert(ce_reg_set(r, "HKCU\\Missing", "v", 1, NULL, 0) == 2);
    uint32_t size;
    uint8_t *data = ce_reg_encode(r, &size);
    assert(data && size < CE_REG_FILE_MAX);
    assert(ce_reg_decode(copy, data, size));
    assert(ce_reg_exists(copy, "HKCU\\Software\\Glaze\\Empty"));
    struct ce_reg_value *v = ce_reg_find(copy, "HKCU\\Software\\Glaze", "bLOB");
    assert(v && v->type == 3 && v->size == sizeof(binary) && !memcmp(v->bytes, binary, sizeof(binary)));
    unsigned old_count = ce_u32(data + 4);
    uint32_t suffix = 8;
    for (unsigned i = 0; i < old_count; i++)
        suffix += 16 + ce_u32(data + suffix) + ce_u32(data + suffix + 4) + ce_u32(data + suffix + 12);
    /* Corruption/truncation must leave the previous live state intact. */
    for (uint32_t n = 0; n < size; n++) {
        if (n == suffix)
            continue;
        assert(!ce_reg_decode(copy, data, n));
        assert(ce_reg_exists(copy, "HKCU\\Software\\Glaze\\Empty"));
    }
    data[0] = 'X';
    assert(!ce_reg_decode(copy, data, size));
    data[0] = 'G';
    ce_put32(data + 4, 0xffffffffu);
    assert(!ce_reg_decode(copy, data, size));
    ce_put32(data + 4, old_count);
    /* Legacy records infer their parent keys; no key suffix is required. */
    assert(ce_reg_decode(copy, data, suffix));
    assert(ce_reg_exists(copy, "HKCU\\Software"));
    assert(!ce_reg_exists(copy, "HKCU\\Software\\Glaze\\Empty"));
    assert(!ce_reg_remove_value(copy, "HKCU\\Software\\Glaze", "blob"));
    assert(!ce_reg_remove_value(copy, "HKCU\\Software\\Glaze", ""));
    assert(!ce_reg_delete(copy, "HKCU\\Software\\Glaze"));
    assert(ce_reg_delete(copy, "HKCU") == 5);
    assert(!ce_reg_delete(copy, "HKCU\\Software"));
    ce_reg_clear(copy);
    char key[256];
    for (unsigned i = 0; i < CE_REG_KEYS; i++) {
        snprintf(key, sizeof(key), "HKLM\\Key%u", i);
        assert(!ce_reg_create(copy, key));
    }
    assert(ce_reg_create(copy, "HKLM\\Overflow\\Child") == 8);
    assert(!ce_reg_exists(copy, "HKLM\\Overflow"));
    assert(!ce_reg_delete(copy, key)); /* Last slot at the capacity boundary. */
    ce_reg_clear(copy);
    unsigned char *large = calloc(1, 65536);
    assert(large);
    assert(!ce_reg_set(copy, "HKCU", "A", 3, large, 65536));
    assert(!ce_reg_set(copy, "HKCU", "B", 3, large, 65536));
    assert(!ce_reg_set(copy, "HKCU", "C", 3, large, 65536));
    assert(ce_reg_set(copy, "HKCU", "D", 3, large, 65536) == 8);
    assert(copy->value_count == 3 && !ce_reg_find(copy, "HKCU", "D"));
    assert(!ce_reg_set(copy, "HKCU", "A", 4, binary, 4));
    free(large);
    ce_reg_clear(copy);
    assert(!ce_reg_create(copy, "HKCU\\App\\A\\Deep"));
    assert(!ce_reg_create(copy, "HKCU\\App\\B"));
    assert(!ce_reg_create(copy, "HKCU\\Apple"));
    assert(!ce_reg_set(copy, "HKCU\\App\\A\\Deep", "v", 4, binary, 4));
    assert(!ce_reg_set(copy, "HKCU\\App", "w", 4, binary, 4));
    assert(!ce_reg_set(copy, "HKCU\\Apple", "x", 4, binary, 4));
    assert(ce_reg_delete_tree(copy, "HKCU") == 5);
    assert(ce_reg_delete_tree(copy, "HKCU\\Missing") == 2);
    assert(!ce_reg_delete_tree(copy, "hkcu\\app"));
    assert(!ce_reg_exists(copy, "HKCU\\App") && !ce_reg_exists(copy, "HKCU\\App\\A\\Deep"));
    assert(ce_reg_exists(copy, "HKCU\\Apple") && copy->value_count == 1 && copy->key_count == 1);
    ce_reg_clear(r);
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        assert(f);
        assert(!fseek(f, 0, SEEK_END));
        long sz = ftell(f);
        assert(sz > 0 && sz <= CE_REG_FILE_MAX);
        rewind(f);
        uint8_t *buf = malloc((size_t)sz);
        assert(buf);
        assert(fread(buf, 1, (size_t)sz, f) == (size_t)sz);
        fclose(f);
        assert(ce_reg_decode(r, buf, (uint32_t)sz));
        free(buf);
        ce_reg_clear(r);
    }
    /* A synthetic Casio System.reg: HKCU\ControlPanel\Menu\Volume with a
     * default value and a DWORD, and an HKLM\Drivers key that is skipped. */
    {
        uint8_t backup[512] = {0};
        size_t at = 8;
        const char *keys[] = {"ControlPanel\\Menu\\Volume", "Drivers\\BuiltIn"};
        const unsigned roots[] = {1, 2};
        for (unsigned k = 0; k < 2; k++) {
            unsigned units = (unsigned)strlen(keys[k]) + 1;
            backup[at] = (uint8_t)(6 + units * 2);
            backup[at + 2] = 1;
            backup[at + 4] = (uint8_t)roots[k];
            backup[at + 6] = (uint8_t)units;
            for (unsigned i = 0; i < units; i++)
                backup[at + 10 + i * 2] = (uint8_t)keys[k][i];
            at += 10 + units * 2;
            const char *names[] = {"Default", "ListNO"};
            for (unsigned v = 0; v < 2; v++) {
                unsigned nunits = (unsigned)strlen(names[v]) + 1, bytes = 4; /* "Hi" or DWORD 1 */
                backup[at] = (uint8_t)(6 + nunits * 2 + bytes);
                backup[at + 2] = 2;
                backup[at + 4] = v ? 4 : 1;
                backup[at + 6] = (uint8_t)nunits;
                backup[at + 8] = (uint8_t)bytes;
                for (unsigned i = 0; i < nunits; i++)
                    backup[at + 10 + i * 2] = (uint8_t)names[v][i];
                uint8_t *payload = backup + at + 10 + nunits * 2;
                if (v)
                    payload[0] = 1;
                else
                    payload[0] = 'H', payload[2] = 'i';
                at += 10 + nunits * 2 + bytes;
            }
        }
        backup[0] = 0xb2, backup[1] = 0x74, backup[2] = 0x83, backup[3] = 0x1d;
        backup[4] = (uint8_t)at, backup[5] = (uint8_t)(at >> 8);
        ce_reg_clear(r);
        assert(ce_reg_import_casio(r, backup, at));
        struct ce_reg_value *v = ce_reg_find(r, "HKCU\\ControlPanel\\Menu\\Volume", "");
        assert(v && v->type == 1 && v->size == 4 && v->bytes[0] == 'H');
        v = ce_reg_find(r, "HKCU\\ControlPanel\\Menu\\Volume", "ListNO");
        assert(v && v->type == 4 && v->bytes[0] == 1);
        assert(!ce_reg_exists(r, "HKLM\\Drivers") && !ce_reg_exists(r, "HKLM\\Drivers\\BuiltIn"));
        assert(!ce_reg_import_casio(r, backup, at - 1)); /* size word must match */
        ce_reg_clear(r);
        const char *path = getenv("WINCE_SYSTEM_REG");
        if (path) { /* a locally supplied \Backup\System.reg */
            FILE *f = fopen(path, "rb");
            assert(f);
            static uint8_t file[CE_REG_FILE_MAX];
            size_t n = fread(file, 1, sizeof(file), f);
            fclose(f);
            assert(ce_reg_import_casio(r, file, n));
            assert(ce_reg_find(r, "HKCU\\ControlPanel\\Menu\\Memory", "AppPath"));
            uint32_t size;
            uint8_t *encoded = ce_reg_encode(r, &size);
            assert(encoded && ce_reg_decode(copy, encoded, size));
            printf("System.reg: %u keys, %u values, %u bytes encoded\n", r->key_count, r->value_count, size);
            free(encoded);
            ce_reg_clear(r);
        }
    }
    ce_reg_clear(copy);
    free(r);
    free(copy);
    free(data);
    puts("WinCE registry persistence, enumeration and corruption checks passed");
}
