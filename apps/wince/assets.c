/* Casio B000FF/LZSS and CCFS formats, matching tools/wince_assets.py.
 * Sizes and references are validated; record checksums are audited. Only export
 * names and the CPK dictionary survive the temporary NK recovery buffer. */
#include "assets.h"
#include "pe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define NAND_BYTES (16u * 1024u * 1024u)
#define NK_MAX (8u * 1024u * 1024u)
struct input {
    ce_asset_reader read;
    void *arg;
    uint32_t pos, end, at, have;
    uint8_t data[4096], ring[4096];
    unsigned cursor, flags, ref, left;
};
static int input_byte(struct input *in)
{
    if (in->at == in->have) {
        uint32_t size = MIN((uint32_t)sizeof(in->data), in->end - in->pos);
        if (!size || in->read(in->arg, in->pos, in->data, size) != (int)size) return -1;
        in->pos += size;
        in->at = 0;
        in->have = size;
    }
    return in->data[in->at++];
}
static int lz_byte(struct input *in)
{
    int value;
    if (in->left) {
        value = in->ring[in->ref++ & 4095u];
        in->left--;
    } else {
        in->flags >>= 1;
        if (!(in->flags & 0x100)) {
            int flags = input_byte(in);
            if (flags < 0) return -1;
            in->flags = (unsigned)flags | 0xff00;
        }
        if (in->flags & 1) value = input_byte(in);
        else {
            int lo = input_byte(in), hi = input_byte(in);
            if (lo < 0 || hi < 0) return -1;
            in->ref = (unsigned)lo | ((unsigned)hi & 0xf0) << 4;
            in->left = ((unsigned)hi & 15) + 2;
            value = in->ring[in->ref++ & 4095u];
        }
    }
    if (value < 0) return -1;
    in->ring[in->cursor++ & 4095u] = (uint8_t)value;
    return value;
}
static bool lz_read(struct input *in, void *out, uint32_t size)
{
    uint8_t *p = out;
    while (size--) {
        int c = lz_byte(in);
        if (c < 0) return false;
        *p++ = (uint8_t)c;
    }
    return true;
}
struct nk {
    uint8_t *data;
    uint32_t base, size;
};
static const uint8_t *nk_at(const struct nk *nk, uint32_t address, uint32_t bytes)
{
    uint32_t off = address - nk->base;
    return address >= nk->base && off <= nk->size && bytes <= nk->size - off ? nk->data + off : NULL;
}
struct module {
    const struct nk *nk;
    const uint8_t *sections;
    unsigned count;
};
static bool module_find(const struct nk *nk, const char *name, struct module *m)
{
    if (nk->size < 0x48 || ce_u32(nk->data + 0x40) != 0x43454345) return false;
    uint32_t toc = ce_u32(nk->data + 0x44);
    const uint8_t *header = nk_at(nk, toc, 80);
    if (!header) return false;
    uint32_t count = ce_u32(header + 16);
    if (!count || count > 256) return false;
    const uint8_t *entries = nk_at(nk, toc + 80, count * 32);
    if (!entries) return false;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *entry = entries + i * 32;
        const char *candidate = (const char *)nk_at(nk, ce_u32(entry + 20), (uint32_t)strlen(name) + 1);
        if (!candidate || strncasecmp(candidate, name, strlen(name) + 1)) continue;
        const uint8_t *e32 = nk_at(nk, ce_u32(entry + 24), 24);
        if (!e32 || !ce_u16(e32) || ce_u16(e32) > 32 || ce_u32(e32 + 20) > CE_ASSET_MAX) return false;
        m->nk = nk;
        m->count = ce_u16(e32);
        m->sections = nk_at(nk, ce_u32(entry + 28), m->count * 24);
        return m->sections != NULL;
    }
    return false;
}
static const uint8_t *module_at(const struct module *m, uint32_t rva, uint32_t size)
{
    for (unsigned i = 0; i < m->count; i++) {
        const uint8_t *s = m->sections + 24 * i;
        uint32_t start = ce_u32(s + 4), bytes = ce_u32(s + 8), off = rva - start;
        if (!(ce_u32(s + 20) & 0x2000) && rva >= start && off <= bytes && size <= bytes - off) {
            const uint8_t *p = nk_at(m->nk, ce_u32(s + 12), bytes);
            return p ? p + off : NULL;
        }
    }
    return NULL;
}
static const char *module_string(const struct module *m, uint32_t rva)
{
    for (unsigned size = 1; size <= 256; size++) {
        const uint8_t *p = module_at(m, rva, size);
        if (!p) return NULL;
        if (!p[size - 1]) return (const char *)p;
        if (p[size - 1] < 33 || p[size - 1] > 126) return NULL;
    }
    return NULL;
}
static bool export_names(const struct nk *nk, const char *dll, struct ce_nk_assets *out)
{
    struct module m;
    if (!module_find(nk, dll, &m)) return false;
    for (unsigned section = 0; section < m.count; section++) {
        const uint8_t *s = m.sections + 24 * section;
        uint32_t bytes = ce_u32(s + 8), rva = ce_u32(s + 4);
        const uint8_t *p = module_at(&m, rva, bytes);
        if (!p) continue;
        for (uint32_t at = 0; at + 40 <= bytes; at += 4) {
            const uint8_t *d = p + at;
            const char *name = module_string(&m, ce_u32(d + 12));
            if (!name || strcasecmp(name, dll)) continue;
            uint32_t base = ce_u32(d + 16), functions = ce_u32(d + 20), names = ce_u32(d + 24);
            if (!names || names > functions || functions > 65536 || base > 65535) continue;
            const uint8_t *ft = module_at(&m, ce_u32(d + 28), functions * 4);
            const uint8_t *nt = module_at(&m, ce_u32(d + 32), names * 4);
            const uint8_t *ot = module_at(&m, ce_u32(d + 36), names * 2);
            if (!ft || !nt || !ot) continue;
            for (uint32_t i = 0; i < names; i++) {
                unsigned ordinal = ce_u16(ot + i * 2);
                const char *symbol = module_string(&m, ce_u32(nt + i * 4));
                if (!symbol || ordinal >= functions || base + ordinal > 65535) return false;
                int n = snprintf(out->ordinals + out->ordinal_bytes, CE_ORDINAL_MAX - out->ordinal_bytes,
                                 "%s %u %s\n", dll, base + ordinal, symbol);
                if (n < 0 || (uint32_t)n >= CE_ORDINAL_MAX - out->ordinal_bytes) return false;
                out->ordinal_bytes += (uint32_t)n;
            }
            return true;
        }
    }
    return false;
}
static bool dictionary(const struct nk *nk, struct ce_nk_assets *out)
{
    struct module m;
    if (!module_find(nk, "ccfs.dll", &m)) return false;
    static const int32_t signature[] = {0, 0, 0, 0, 0, -2, -7};
    for (unsigned i = 0; i < m.count; i++) {
        const uint8_t *s = m.sections + i * 24;
        uint32_t bytes = ce_u32(s + 8), rva = ce_u32(s + 4);
        const uint8_t *p = module_at(&m, rva, bytes);
        if (!p) continue;
        for (uint32_t at = 4; at + sizeof(signature) <= bytes; at += 4) {
            unsigned j;
            for (j = 0; j < ARRAY_SIZE(signature); j++)
                if ((int32_t)ce_u32(p + at + j * 4) != signature[j]) break;
            if (j != ARRAY_SIZE(signature)) continue;
            uint32_t table = rva + at - 4;
            const uint8_t *head = module_at(&m, table, 0x104);
            if (!head || out->dictionary) return false;
            int64_t maximum = -1;
            for (unsigned width = 1; width <= 16; width++) {
                uint32_t threshold = ce_u32(head + 0x84 + width * 4);
                if (!threshold) continue;
                int64_t end = (int32_t)ce_u32(head + 4 + width * 4);
                end += MIN(threshold - 1, (1u << width) - 1);
                if (end > maximum) maximum = end;
            }
            if (maximum < 0 || maximum >= (CE_DICT_MAX - 0x104) / 2) return false;
            out->dictionary_bytes = 0x104 + ((uint32_t)maximum + 1) * 2;
            const uint8_t *data = module_at(&m, table, out->dictionary_bytes);
            if (!data || !(out->dictionary = malloc(out->dictionary_bytes))) return false;
            memcpy(out->dictionary, data, out->dictionary_bytes);
        }
    }
    return out->dictionary != NULL;
}
/* Casio's application libraries, which have no Glaze service adapter of their
 * own. System DLLs (coredll, commctrl, drivers) are never loaded from NK. */
static const char *const rom_libraries[] = {
    "pimdb.dll", "NFC.dll", "MailData.dll", "NotifyData.dll", "CS.dll", "CSO.dll", "coshellapi.dll",
    "GetDisk.dll", "clblib.dll", "buzzer.dll", "NANDAccess.dll", "commdlg.dll", "CAInst.dll", "cefobj.dll",
};
static bool toc_entry(const struct nk *nk, const char *name, const uint8_t **e32, const uint8_t **o32)
{
    if (nk->size < 0x48 || ce_u32(nk->data + 0x40) != 0x43454345) return false;
    uint32_t toc = ce_u32(nk->data + 0x44);
    const uint8_t *header = nk_at(nk, toc, 80);
    if (!header) return false;
    uint32_t count = ce_u32(header + 16);
    const uint8_t *entries = count && count <= 256 ? nk_at(nk, toc + 80, count * 32) : NULL;
    for (unsigned i = 0; entries && i < count; i++) {
        const uint8_t *entry = entries + i * 32;
        const char *candidate = (const char *)nk_at(nk, ce_u32(entry + 20), (uint32_t)strlen(name) + 1);
        if (!candidate || strncasecmp(candidate, name, strlen(name) + 1)) continue;
        *e32 = nk_at(nk, ce_u32(entry + 24), 106);
        *o32 = *e32 ? nk_at(nk, ce_u32(entry + 28), ce_u16(*e32) * 24u) : NULL;
        return *o32 && ce_u16(*e32) && ce_u16(*e32) <= CE_PE_SECTIONS;
    }
    return false;
}
/* Record: u32 bytes, name[32], vbase, vsize, entry, imageflags, count,
 * nine e32 units (rva, size), then count {rva, vsize, psize, flags} and the
 * section bytes as stored. Layout of e32_rom/o32_rom: Windows CE romldr.h. */
static bool rom_records(const struct nk *nk, struct ce_nk_assets *out)
{
    for (unsigned i = 0; i < ARRAY_SIZE(rom_libraries); i++) {
        const uint8_t *e32, *o32;
        if (!toc_entry(nk, rom_libraries[i], &e32, &o32)) continue;
        unsigned count = ce_u16(e32);
        uint32_t bytes = CE_ROM_RECORD_HEADER + count * 16u;
        for (unsigned s = 0; s < count; s++) {
            uint32_t psize = ce_u32(o32 + s * 24 + 8);
            if (psize > CE_PE_MAX_SIZE || !nk_at(nk, ce_u32(o32 + s * 24 + 12), psize)) return false;
            bytes += ALIGN_UP(psize, 4u);
        }
        if (bytes > CE_ROM_MODULES_MAX - out->module_bytes) return false;
        uint8_t *r = out->modules + out->module_bytes, *p = r + CE_ROM_RECORD_HEADER + count * 16u;
        memset(r, 0, bytes);
        ce_put32(r, bytes);
        strlcpy((char *)r + 4, rom_libraries[i], 32);
        ce_put32(r + 36, ce_u32(e32 + 8));
        ce_put32(r + 40, ce_u32(e32 + 20));
        ce_put32(r + 44, ce_u32(e32 + 4));
        ce_put32(r + 48, ce_u16(e32 + 2));
        ce_put32(r + 52, count);
        memcpy(r + 56, e32 + 32, 72);
        for (unsigned s = 0; s < count; s++) {
            const uint8_t *o = o32 + s * 24;
            uint8_t *d = r + CE_ROM_RECORD_HEADER + s * 16;
            uint32_t psize = ce_u32(o + 8);
            ce_put32(d, ce_u32(o + 4));
            ce_put32(d + 4, ce_u32(o));
            ce_put32(d + 8, psize);
            ce_put32(d + 12, ce_u32(o + 20));
            memcpy(p, nk_at(nk, ce_u32(o + 12), psize), psize);
            p += ALIGN_UP(psize, 4u);
        }
        out->module_bytes += bytes;
    }
    return true;
}
static uint32_t u24(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}
/* Each block's match offsets are absolute within its 4 KiB of output. A flag
 * byte selects literals (0) or matches (1), least significant bit first. The
 * match byte's low nibble 1 is a two-byte copy from 2..17 bytes back; 0 means
 * a 12-bit offset and an extra length byte (17..272); 2..15 give lengths 3..16.
 * Derived from nk.exe's BinaryDecompress, reached from COREDLL's export. */
static bool rom_block(const uint8_t *src, uint32_t n, uint8_t *out, uint32_t cap, uint32_t *produced)
{
    uint32_t i = 0, pos = 0;
    while (i < n && pos < cap) {
        unsigned flags = src[i++];
        for (unsigned bit = 1; bit < 0x100 && i < n && pos < cap; bit <<= 1) {
            unsigned v = src[i++];
            if (!(flags & bit)) {
                out[pos++] = (uint8_t)v;
                continue;
            }
            unsigned low = v & 15, length;
            uint32_t from;
            if (low == 1) {
                if (pos < (v >> 4) + 2u) return false;
                from = pos - (v >> 4) - 2;
                length = 2;
            } else {
                if (i >= n) return false;
                from = v >> 4 | (uint32_t)src[i++] << 4;
                if (!low) {
                    if (i >= n) return false;
                    length = src[i++] + 17u;
                } else {
                    length = low + 1;
                }
            }
            if (from >= pos) return false;
            for (unsigned k = 0; k < length && pos < cap; k++) out[pos++] = out[from + k];
        }
    }
    *produced = pos;
    return true;
}
const char *ce_rom_decompress(const uint8_t *src, uint32_t n, uint8_t *out, uint32_t capacity, uint32_t *produced)
{
    *produced = 0;
    if (n < 6) return "Truncated compressed ROM section";
    uint32_t total = u24(src), entries = (total >> 12) + 2, start = entries * 3, done = 0;
    if (total > capacity || start > n) return "Invalid compressed ROM section size";
    for (uint32_t k = 1; k < entries && done < total; k++) {
        uint32_t end = u24(src + 3 * k), made;
        if (end < start || end > n ||
            !rom_block(src + start, end - start, out + done, MIN(4096u, total - done), &made))
            return "Invalid compressed ROM block";
        done += made;
        start = end;
    }
    if (done != total) return "Truncated compressed ROM data";
    *produced = done;
    return NULL;
}
static const uint8_t *rom_record(const uint8_t *modules, uint32_t bytes, const char *name)
{
    for (uint32_t at = 0; bytes - at >= CE_ROM_RECORD_HEADER;) {
        const uint8_t *r = modules + at;
        uint32_t size = ce_u32(r);
        if (size < CE_ROM_RECORD_HEADER || size > bytes - at || (size & 3) || r[35]) return NULL;
        if (!strcasecmp((const char *)r + 4, name)) return r;
        at += size;
    }
    return NULL;
}
const char *ce_rom_module_pe(const uint8_t *modules, uint32_t bytes, const char *name, void **result,
                             uint32_t *result_size)
{
    *result = NULL; *result_size = 0;
    const uint8_t *r = modules ? rom_record(modules, bytes, name) : NULL;
    if (!r) return "ROM module not cached";
    uint32_t size = ce_u32(r), vbase = ce_u32(r + 36), vsize = ce_u32(r + 40), count = ce_u32(r + 52);
    if (!count || count > CE_PE_SECTIONS || CE_ROM_RECORD_HEADER + count * 16 > size || (vbase & 0xffff) ||
        !vsize || vsize > CE_PE_MAX_SIZE || (vsize & 4095) || !ce_range(vbase, vsize, 0x0f000000u))
        return "Invalid ROM module layout";
    uint32_t headers = ALIGN_UP(0x138u + count * 40u, 512u), file = headers;
    const uint8_t *p = r + CE_ROM_RECORD_HEADER + count * 16;
    for (uint32_t s = 0, left = size - CE_ROM_RECORD_HEADER - count * 16; s < count; s++) {
        const uint8_t *d = r + CE_ROM_RECORD_HEADER + s * 16;
        uint32_t vs = ce_u32(d + 4), psize = ce_u32(d + 8);
        if (psize > left || !ce_range(ce_u32(d), vs, vsize)) return "Invalid ROM module section";
        left -= MIN(left, ALIGN_UP(psize, 4u));
        file += ALIGN_UP(vs, 512u);
    }
    uint8_t *pe = calloc(1, file);
    if (!pe) return "Out of memory loading ROM module";
    pe[0] = 'M'; pe[1] = 'Z';
    ce_put32(pe + 60, 0x40);
    ce_put32(pe + 0x40, 0x4550);
    uint8_t *coff = pe + 0x44, *o = coff + 20;
    ce_put16(coff, 0x166);
    ce_put16(coff + 2, (uint16_t)count);
    ce_put16(coff + 16, 0xe0);
    ce_put16(coff + 18, 0x2102);
    ce_put16(o, 0x10b);
    ce_put32(o + 16, ce_u32(r + 44));
    ce_put32(o + 28, vbase);
    ce_put32(o + 32, 4096);
    ce_put32(o + 36, 512);
    ce_put32(o + 56, vsize);
    ce_put32(o + 60, headers);
    ce_put16(o + 68, 9);
    ce_put32(o + 92, 16);
    /* Exports, imports, resources, exceptions and fixups keep their PE
     * directory numbers; the security, debug and other units are unused. */
    static const unsigned units[] = {0, 1, 2, 3, 5};
    for (unsigned i = 0; i < ARRAY_SIZE(units); i++) {
        uint32_t rva = ce_u32(r + 56 + units[i] * 8), len = ce_u32(r + 60 + units[i] * 8);
        if (rva && len) {
            ce_put32(o + 96 + units[i] * 8, rva);
            ce_put32(o + 100 + units[i] * 8, len);
        }
    }
    const char *error = NULL;
    uint32_t raw = headers;
    for (uint32_t s = 0; s < count && !error; s++) {
        const uint8_t *d = r + CE_ROM_RECORD_HEADER + s * 16;
        uint32_t rva = ce_u32(d), vs = ce_u32(d + 4), psize = ce_u32(d + 8), flags = ce_u32(d + 12), made;
        uint8_t *section = pe + 0x138 + s * 40;
        if (flags & 0x2000) {
            error = ce_rom_decompress(p, psize, pe + raw, vs, &made);
        } else {
            made = MIN(vs, psize);
            memcpy(pe + raw, p, made);
        }
        ce_put32(section + 8, vs);
        ce_put32(section + 12, rva);
        ce_put32(section + 16, made);
        ce_put32(section + 20, made ? raw : 0);
        ce_put32(section + 36, flags & ~0x2000u);
        raw += ALIGN_UP(vs, 512u);
        p += ALIGN_UP(psize, 4u);
    }
    if (!error && ce_u32(r + 44) >= vsize) error = "Invalid ROM module entry";
    if (error) { free(pe); return error; }
    *result = pe; *result_size = file;
    return NULL;
}
void ce_nk_assets_free(struct ce_nk_assets *assets)
{
    free(assets->ordinals);
    free(assets->modules);
    free(assets->dictionary);
    memset(assets, 0, sizeof(*assets));
}
const char *ce_nk_recover(ce_asset_reader read, void *arg, ce_asset_progress progress,
                         struct ce_nk_assets *assets)
{
    uint8_t table[512], header[15], record[12];
    memset(assets, 0, sizeof(*assets));
    if (read(arg, 0, table, sizeof(table)) != sizeof(table)) return "Cannot read original NAND";
    uint32_t start = ce_u32(table + 40), pages = ce_u32(table + 44);
    if (start >= NAND_BYTES / 512 || !pages || pages > NAND_BYTES / 512 - start) return "Invalid CE NK partition";
    struct input *in = calloc(1, sizeof(*in));
    if (!in) return "Out of memory decoding CE ROM";
    in->read = read; in->arg = arg; in->pos = start * 512; in->end = (start + pages) * 512; in->cursor = 0xfee;
    struct nk nk = {0};
    const char *error = "Invalid compressed CE ROM";
    if (!lz_read(in, header, sizeof(header)) || memcmp(header, "B000FF\n", 7)) goto done;
    nk.base = ce_u32(header + 7); nk.size = ce_u32(header + 11);
    if (nk.base < 0x80000000u || nk.base >= 0x81000000u || !nk.size || nk.size > NK_MAX ||
        nk.size > 0x81000000u - nk.base) goto done;
    nk.data = calloc(1, nk.size);
    if (!nk.data) { error = "Not enough free RAM to recover CE ROM metadata"; goto done; }
    uint32_t total = 0;
    bool ended = false;
    for (unsigned count = 0; count < 256; count++) {
        if (!lz_read(in, record, sizeof(record))) goto done;
        uint32_t addr = ce_u32(record), bytes = ce_u32(record + 4), expected = ce_u32(record + 8);
        if (!addr) { ended = count != 0 && expected == 0; break; }
        if (!nk_at(&nk, addr, bytes) || !bytes || bytes > NK_MAX - total) goto done;
        uint8_t *dest = nk.data + addr - nk.base;
        uint32_t sum = 0;
        for (uint32_t j = 0; j < bytes; j++) {
            int c = lz_byte(in); if (c < 0) goto done;
            dest[j] = (uint8_t)c; sum += (uint8_t)c;
            if (progress && (j & 0x3ffffu) == 0) progress(arg, total + j, nk.size);
        }
        /* Some captured CE restore streams retain stale checksums after
         * metadata changes. Audit them; this reader consumes only bounded
         * export/dictionary tables and never executes NK code. */
        if (sum != expected) assets->checksum_mismatches++;
        total += bytes;
    }
    if (!ended) goto done;
    error = "Cannot recover matching CE export names or CCFS dictionary";
    assets->ordinals = malloc(CE_ORDINAL_MAX);
    assets->modules = malloc(CE_ROM_MODULES_MAX);
    if (!assets->ordinals || !assets->modules || !export_names(&nk, "coredll.dll", assets) ||
        !export_names(&nk, "commctrl.dll", assets) || !dictionary(&nk, assets)) goto done;
    error = "Invalid CE ROM library table";
    if (!rom_records(&nk, assets)) goto done;
    if (!assets->module_bytes) {
        free(assets->modules);
        assets->modules = NULL;
    } else {
        uint8_t *shrunk = realloc(assets->modules, assets->module_bytes);
        if (shrunk) assets->modules = shrunk;
    }
    error = NULL;
    if (progress) progress(arg, nk.size, nk.size);
done:
    free(nk.data); free(in);
    if (error) ce_nk_assets_free(assets);
    return error;
}

struct memory { const uint8_t *data; uint32_t size; };
static int memory_read(void *arg, uint32_t off, void *out, uint32_t size)
{
    struct memory *mem = arg;
    if (off > mem->size || size > mem->size - off) return -1;
    memcpy(out, mem->data + off, size); return (int)size;
}
static bool bits(const uint8_t *data, uint32_t bytes, uint32_t bit, unsigned width, bool msb, uint32_t *out)
{
    if (width > 16 || bit > bytes * 8u || width > bytes * 8u - bit) return false;
    uint32_t value = 0;
    for (unsigned i = 0; i < width; i++, bit++) {
        unsigned b = (data[bit / 8] >> (msb ? 7 - bit % 8 : bit % 8)) & 1;
        if (msb) value = value * 2 + b; else value |= b << i;
    }
    *out = value; return true;
}
static bool cpk_block(const uint8_t *src, uint32_t len, unsigned kind, uint8_t *out, uint32_t size,
                       const uint8_t *dict, uint32_t dict_bytes)
{
    if (!kind) { if (len < size) return false; memcpy(out, src, size); return true; }
    if (kind <= 8) {
        bool nibble = kind >= 4;
        unsigned threshold = nibble ? kind - 2 : kind + 1;
        uint32_t count = size * (nibble ? 2 : 1), at = 0, pos = 0, run = 0, prev = UINT32_MAX;
        memset(out, 0, size);
        while (at < count) {
            uint32_t value;
            if (!bits(src, len, pos, nibble ? 4 : 8, false, &value)) return false;
            pos += nibble ? 4 : 8;
            if (nibble) out[at / 2] |= (uint8_t)(value << ((at & 1) * 4)); else out[at] = (uint8_t)value;
            at++; run = prev == value ? run + 1 : 1; prev = value;
            if (run == threshold) {
                uint32_t repeat;
                if (!bits(src, len, pos, nibble ? 4 : 8, false, &repeat) || repeat > count - at) return false;
                pos += nibble ? 4 : 8;
                while (repeat--) { if (nibble) out[at / 2] |= (uint8_t)(value << ((at & 1) * 4)); else out[at] = (uint8_t)value; at++; }
                run = 0;
            }
        }
        return true;
    }
    if (kind <= 14) {
        bool nibble = kind >= 13;
        unsigned width = kind - (nibble ? 12 : 8), unit = nibble ? 4 : 8, mask = (1u << width) - 1;
        uint32_t count = size * (nibble ? 2 : 1), index = mask * unit;
        uint32_t escape = (1u << width) * unit + ((count * width + unit - 1) / unit) * unit - unit;
        memset(out, 0, size);
        for (uint32_t at = 0; at < count; at++) {
            uint32_t code, value;
            if (!bits(src, len, index, width, false, &code)) return false;
            index += width;
            uint32_t offset = code == mask ? escape : code * unit;
            if (!bits(src, len, offset, unit, false, &value)) return false;
            if (code == mask) escape += unit;
            if (nibble) out[at / 2] |= (uint8_t)(value << ((at & 1) * 4)); else out[at] = (uint8_t)value;
        }
        return true;
    }
    if (kind == 15) {
        if (!dict || dict_bytes < 0x104 || (size & 1)) return false;
        uint32_t pos = 0;
        for (uint32_t at = 0; at < size; at += 2) {
            uint32_t code = 0, bit = 0, value; unsigned width;
            for (width = 1; width <= 16; width++) {
                if (!bits(src, len, pos++, 1, true, &bit)) return false;
                code = code * 2 + bit;
                if (code < ce_u32(dict + 0x84 + width * 4)) break;
            }
            if (width == 17) return false;
            int64_t index = (int32_t)ce_u32(dict + 4 + width * 4); index += code;
            if (index < 0 || index >= (dict_bytes - 0x104) / 2) return false;
            value = ce_u16(dict + 0x104 + (uint32_t)index * 2);
            if (value == ce_u32(dict) && !bits(src, len, pos, 16, true, &value)) return false;
            if (ce_u16(dict + 0x104 + (uint32_t)index * 2) == ce_u32(dict)) pos += 16;
            ce_put16(out + at, (uint16_t)value);
        }
        return true;
    }
    if (kind == 16) {
        struct input *in = calloc(1, sizeof(*in)); if (!in) return false;
        struct memory mem = {src, len};
        in->read = memory_read; in->arg = &mem; in->end = len; in->cursor = 0xfee;
        bool ok = lz_read(in, out, size); free(in); return ok;
    }
    return false;
}
const char *ce_cpk_unpack(const void *data, uint32_t bytes, const void *dict, uint32_t dict_bytes,
                          void **result, uint32_t *result_size)
{
    const uint8_t *src = data;
    *result = NULL; *result_size = 0;
    if (bytes < 52 || bytes > CE_ASSET_MAX || memcmp(src, "CASIO.$&%-Z\1", 12)) return "Invalid CPK signature";
    uint32_t size = ce_u32(src + 44), word = ce_u32(src + 48), count = word & 0xffffff;
    uint32_t stride = MAX(word >> 24, 1u) * 512;
    if (!size || size > CE_ASSET_MAX || count != (size + stride - 1) / stride || count > (bytes - 52) / 4)
        return "Invalid CPK block table";
    uint32_t payload = 52 + count * 4;
    uint8_t *out = malloc(size), *block = malloc(stride);
    if (!out || !block) { free(out); free(block); return "Out of memory decoding CPK"; }
    const char *error = "Invalid compressed CPK block";
    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry = ce_u32(src + 52 + i * 4), lo = entry & 0xffffff;
        uint32_t hi = i + 1 < count ? ce_u32(src + 56 + i * 4) & 0xffffff : bytes - payload;
        if ((!i && lo) || lo > hi || hi > bytes - payload ||
            !cpk_block(src + payload + lo, hi - lo, entry >> 24, block, stride, dict, dict_bytes)) goto done;
        memcpy(out + i * stride, block, MIN(stride, size - i * stride));
    }
    *result = out; *result_size = size; out = NULL; error = NULL;
done:
    free(out); free(block); return error;
}
