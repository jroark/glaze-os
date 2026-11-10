/* An executable's application icon as a GZI, read through a ce_asset_reader
 * so a CPK needs only its headers and resource blocks decoded. Layouts:
 * Microsoft PE/COFF specification (optional header data directory 2, the
 * resource directory tables and data entries), and the RT_GROUP_ICON /
 * RT_ICON resources: a GRPICONDIR whose 14-byte entries name RT_ICON IDs,
 * each a BITMAPINFOHEADER DIB of twice the icon's height (XOR rows, then
 * the 1 bpp AND mask), bottom-up. The first group is the program's icon,
 * as Explorer and ExtractIcon take it. */
#include "exeicon.h"
#include "pe.h"
#include <stdlib.h>
#include <string.h>

#define GROUP_MAX 1024u
#define ICON_MAX (64u * 1024u)
#define DIR_ENTRIES_MAX 512u

struct exe {
    ce_asset_reader read;
    void *arg;
    uint32_t bytes, rsrc_rva, rsrc_raw, rsrc_size;
};
static bool exe_read(const struct exe *x, uint32_t offset, void *out, uint32_t size)
{
    return ce_range(offset, size, x->bytes) && x->read(x->arg, offset, out, size) == (int)size;
}
/* Resource RVAs inside the section holding directory 2. */
static bool rsrc_read(const struct exe *x, uint32_t rva, void *out, uint32_t size)
{
    if (rva < x->rsrc_rva || !ce_range(rva - x->rsrc_rva, size, x->rsrc_size))
        return false;
    return exe_read(x, x->rsrc_raw + rva - x->rsrc_rva, out, size);
}
/* Entry `pick` of a directory table at section offset `table` (-1: the one
 * with this ID; else that index, named entries first as stored). Returns
 * the entry's OffsetToData word, or 0. */
static uint32_t dir_entry(const struct exe *x, uint32_t table, int id, unsigned pick)
{
    uint8_t head[16], entry[8];
    if (!rsrc_read(x, x->rsrc_rva + table, head, 16))
        return 0;
    unsigned count = MIN((unsigned)ce_u16(head + 12) + ce_u16(head + 14), DIR_ENTRIES_MAX);
    for (unsigned i = 0; i < count; i++) {
        if (!rsrc_read(x, x->rsrc_rva + table + 16 + i * 8, entry, 8))
            return 0;
        uint32_t name = ce_u32(entry);
        if (id >= 0 ? !(name & 0x80000000u) && name == (uint32_t)id : i == pick)
            return ce_u32(entry + 4);
    }
    return 0;
}
/* Type → name → first language → data, read into a new buffer. */
static uint8_t *resource(const struct exe *x, unsigned type, int id, uint32_t max, uint32_t *size)
{
    uint32_t t = dir_entry(x, 0, (int)type, 0);
    if (!(t & 0x80000000u))
        return NULL;
    uint32_t n = dir_entry(x, t & 0x7fffffffu, id, 0);
    if (!(n & 0x80000000u))
        return NULL;
    uint32_t l = dir_entry(x, n & 0x7fffffffu, -1, 0);
    uint8_t data[16];
    if (!l || (l & 0x80000000u) || !rsrc_read(x, x->rsrc_rva + l, data, 16))
        return NULL;
    uint32_t rva = ce_u32(data), bytes = ce_u32(data + 4);
    uint8_t *out = bytes && bytes <= max ? malloc(bytes) : NULL;
    if (out && !rsrc_read(x, rva, out, bytes)) {
        free(out);
        out = NULL;
    }
    *size = bytes;
    return out;
}
static const char *open_exe(struct exe *x)
{
    uint8_t dos[64], pe[24 + 96 + 16 * 8], sec[40];
    if (!exe_read(x, 0, dos, 64) || dos[0] != 'M' || dos[1] != 'Z')
        return "Not an executable";
    uint32_t at = ce_u32(dos + 60);
    if (!exe_read(x, at, pe, 24) || memcmp(pe, "PE\0\0", 4))
        return "Not a PE executable";
    unsigned sections = ce_u16(pe + 6), optional = ce_u16(pe + 20);
    if (optional < 96 + 3 * 8 || optional > 96 + 16 * 8 || !exe_read(x, at + 24, pe + 24, optional) ||
        ce_u16(pe + 24) != 0x10b || ce_u32(pe + 24 + 92) < 3)
        return "Unsupported PE optional header";
    uint32_t rva = ce_u32(pe + 24 + 96 + 16), size = ce_u32(pe + 24 + 96 + 20);
    if (!rva || !size)
        return "No resources";
    uint32_t table = at + 24 + optional;
    for (unsigned i = 0; i < MIN(sections, 96u); i++) {
        if (!exe_read(x, table + i * 40, sec, 40))
            return "Cannot read the section table";
        uint32_t va = ce_u32(sec + 12), vsize = ce_u32(sec + 8), raw = ce_u32(sec + 20), rawsize = ce_u32(sec + 16);
        uint32_t span = vsize ? MIN(vsize, rawsize) : rawsize;
        if (rva >= va && rva - va < span) {
            x->rsrc_rva = rva;
            x->rsrc_raw = raw + (rva - va);
            x->rsrc_size = span - (rva - va);
            return NULL;
        }
    }
    return "Resources outside the file";
}
/* The DIB's pixel (x, y from the top) as 0xAARRGGBB. */
static uint32_t dib_pixel(const uint8_t *dib, uint32_t palette, uint32_t colors, const uint8_t *xor_rows,
                          uint32_t xor_stride, const uint8_t *and_rows, uint32_t and_stride, int w, int h, int bpp,
                          bool alpha, int x, int y)
{
    const uint8_t *row = xor_rows + (uint32_t)(h - 1 - y) * xor_stride;
    uint32_t c;
    if (bpp <= 8) {
        unsigned bit = (unsigned)x * (unsigned)bpp, index = (row[bit / 8] >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1);
        const uint8_t *q = index < colors ? dib + palette + index * 4 : dib + palette;
        c = 0xff000000u | (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
    } else if (bpp == 16) {
        unsigned v = ce_u16(row + x * 2);
        c = 0xff000000u | (uint32_t)((v >> 10 & 31) * 255 / 31) << 16 | (uint32_t)((v >> 5 & 31) * 255 / 31) << 8 |
            (v & 31) * 255 / 31;
    } else {
        const uint8_t *q = row + x * (bpp / 8);
        c = (bpp == 32 && alpha ? (uint32_t)q[3] << 24 : 0xff000000u) | (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
    }
    (void)w;
    if (!alpha && and_rows && (and_rows[(uint32_t)(h - 1 - y) * and_stride + x / 8] & (0x80u >> (x & 7))))
        c = 0;
    return c;
}
static const char *icon_gzi(const uint8_t *dib, uint32_t bytes, unsigned out_size, uint8_t **gzi, uint32_t *gzi_bytes)
{
    if (bytes < 40 || ce_u32(dib) != 40)
        return "Unsupported icon image";
    int32_t w = (int32_t)ce_u32(dib + 4), h2 = (int32_t)ce_u32(dib + 8);
    int bpp = ce_u16(dib + 14);
    uint32_t compression = ce_u32(dib + 16), used = ce_u32(dib + 32);
    if (w <= 0 || w > 256 || h2 <= 0 || h2 > 512 || (h2 & 1) || compression ||
        (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32))
        return "Unsupported icon image";
    int h = h2 / 2;
    uint32_t colors = bpp <= 8 ? (used && used <= (1u << bpp) ? used : 1u << bpp) : 0;
    uint32_t xor_stride = ALIGN_UP((uint32_t)w * (uint32_t)bpp, 32u) / 8, and_stride = ALIGN_UP((uint32_t)w, 32u) / 8;
    uint32_t pixels = 40 + colors * 4, mask = pixels + xor_stride * (uint32_t)h;
    if (bpp <= 8 && !colors)
        return "Unsupported icon image";
    if (!ce_range(pixels, xor_stride * (uint32_t)h, bytes))
        return "Truncated icon image";
    const uint8_t *and_rows = ce_range(mask, and_stride * (uint32_t)h, bytes) ? dib + mask : NULL;
    bool alpha = false;
    if (bpp == 32)
        for (int y = 0; y < h && !alpha; y++)
            for (int x = 0; x < w && !alpha; x++)
                alpha = dib[pixels + (uint32_t)y * xor_stride + (uint32_t)x * 4 + 3] != 0;
    uint32_t n = out_size * out_size;
    *gzi_bytes = 12 + n * 3;
    uint8_t *out = *gzi = malloc(*gzi_bytes);
    if (!out)
        return "Out of memory";
    memcpy(out, "GZI1", 4);
    ce_put16(out + 4, (uint16_t)out_size);
    ce_put16(out + 6, (uint16_t)out_size);
    ce_put16(out + 8, 1); /* GZI_ALPHA */
    ce_put16(out + 10, 0);
    /* Area-average each output pixel over the source cells it covers, with
     * premultiplied colour so transparent pixels don't darken edges. */
    for (unsigned oy = 0; oy < out_size; oy++) {
        for (unsigned ox = 0; ox < out_size; ox++) {
            uint32_t a = 0, r = 0, g = 0, b = 0, cells = 0;
            int x0 = (int)(ox * (unsigned)w / out_size), x1 = (int)(((ox + 1) * (unsigned)w + out_size - 1) / out_size);
            int y0 = (int)(oy * (unsigned)h / out_size), y1 = (int)(((oy + 1) * (unsigned)h + out_size - 1) / out_size);
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++, cells++) {
                    uint32_t c = dib_pixel(dib, 40, colors, dib + pixels, xor_stride, and_rows, and_stride, w, h, bpp,
                                           alpha, x, y);
                    uint32_t ca = c >> 24;
                    a += ca;
                    r += (c >> 16 & 255) * ca;
                    g += (c >> 8 & 255) * ca;
                    b += (c & 255) * ca;
                }
            unsigned i = oy * out_size + ox;
            unsigned R = a ? r / a : 0, G = a ? g / a : 0, B = a ? b / a : 0;
            ce_put16(out + 12 + i * 2, (uint16_t)((R >> 3) << 11 | (G >> 2) << 5 | B >> 3));
            out[12 + n * 2 + i] = (uint8_t)(cells ? a / cells : 0);
        }
    }
    return NULL;
}
const char *ce_exe_icon(ce_asset_reader read, void *arg, uint32_t bytes, unsigned size, uint8_t **gzi, uint32_t *gzi_bytes)
{
    struct exe x = {read, arg, bytes, 0, 0, 0};
    *gzi = NULL;
    *gzi_bytes = 0;
    if (size < 8 || size > 64)
        return "Unsupported icon size";
    const char *error = open_exe(&x);
    if (error)
        return error;
    /* The first RT_GROUP_ICON (14), whatever its name or ID. */
    uint32_t t = dir_entry(&x, 0, 14, 0), group_bytes = 0;
    if (!(t & 0x80000000u))
        return "No icon";
    uint32_t first = 0;
    uint8_t entry[8];
    if (!rsrc_read(&x, x.rsrc_rva + (t & 0x7fffffffu) + 16, entry, 8))
        return "No icon";
    first = ce_u32(entry);
    uint8_t *group = NULL;
    if (first & 0x80000000u) {
        /* A named group: walk it by index rather than by ID. */
        uint32_t n = ce_u32(entry + 4), l = (n & 0x80000000u) ? dir_entry(&x, n & 0x7fffffffu, -1, 0) : 0;
        uint8_t data[16];
        if (l && !(l & 0x80000000u) && rsrc_read(&x, x.rsrc_rva + l, data, 16) && ce_u32(data + 4) <= GROUP_MAX &&
            (group = malloc(MAX(ce_u32(data + 4), 1u))) && !rsrc_read(&x, ce_u32(data), group, ce_u32(data + 4))) {
            free(group);
            group = NULL;
        }
        group_bytes = group ? ce_u32(data + 4) : 0;
    } else {
        group = resource(&x, 14, (int)first, GROUP_MAX, &group_bytes);
    }
    if (!group)
        return "No icon";
    if (group_bytes < 20 || ce_u16(group + 2) != 1 || !ce_u16(group + 4)) {
        free(group);
        return "Invalid icon group";
    }
    /* The smallest entry at least `size` wide (scaled down), else the largest;
     * among equal widths, the most colours. */
    unsigned count = MIN(ce_u16(group + 4), (group_bytes - 6) / 14), best = 0;
    for (unsigned i = 1; i < count; i++) {
        const uint8_t *e = group + 6 + i * 14, *b = group + 6 + best * 14;
        unsigned ew = e[0] ? e[0] : 256, bw = b[0] ? b[0] : 256;
        bool e_fits = ew >= size, b_fits = bw >= size;
        if (e_fits != b_fits ? e_fits
                             : ew != bw ? (e_fits ? ew < bw : ew > bw) : ce_u16(e + 6) > ce_u16(b + 6))
            best = i;
    }
    unsigned id = ce_u16(group + 6 + best * 14 + 12);
    free(group);
    uint32_t dib_bytes = 0;
    uint8_t *dib = resource(&x, 3, (int)id, ICON_MAX, &dib_bytes);
    if (!dib)
        return "Missing icon image";
    error = icon_gzi(dib, dib_bytes, size, gzi, gzi_bytes);
    free(dib);
    return error;
}
