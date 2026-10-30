/* ROM 9FC01758..9FC017C4 votes on logical IDs in the first five spare tags.
 * Unlike the writable filesystem, boot code must never use a physical page
 * number as a fallback when the tagged logical block cannot be found. */
#include "nand_boot.h"
#include "nand_ecc.h"

static int checked(struct nand_boot_map *m, nand_boot_raw_fn read, void *ctx,
                   uint32_t page, bool allow_no_ecc)
{
    if (read(ctx, page, m->raw)) return -1;
    bool parity = false;
    for (unsigned i = 520; i < 528; i++) parity |= m->raw[i] != 0xFF;
    if (!parity) return allow_no_ecc ? 0 : -1;
    m->protected = true;
    int n = nand_ecc_check(m->raw);
    if (n < 0) return -1;
    m->corrected += (uint32_t)n;
    return 0;
}

int nand_boot_scan(struct nand_boot_map *m, nand_boot_raw_fn read, void *ctx,
                   bool allow_no_ecc)
{
    m->corrected = 0;
    m->protected = false;
    for (unsigned i = 0; i < 128; i++) m->quarantine[i] = 0;
    for (unsigned i = 0; i < NAND_BOOT_BLOCKS; i++) {
        m->block[i] = NAND_BOOT_NONE;
        m->protected_block[i] = 0;
    }
    for (unsigned phys = 0; phys < 1024; phys++) {
        uint16_t ids[5];
        bool protected[5];
        unsigned n = 0, erased = 0;
        bool failed = false, good = false;
        for (unsigned p = 0; p < 5; p++) {
            if (checked(m, read, ctx, phys * 32 + p, allow_no_ecc)) { failed = true; continue; }
            const uint8_t *tag = m->raw + 512;
            uint32_t id = (uint32_t)tag[4] | (uint32_t)tag[5] << 8 |
                          (uint32_t)tag[6] << 16 | (uint32_t)tag[7] << 24;
            if (tag[0] == 0xAA && tag[1] == 0x55 && tag[2] == 0x0F &&
                id < 1024u) {
                ids[n] = (uint16_t)id;
                protected[n] = false;
                for (unsigned b = 520; b < 528; b++) protected[n] |= m->raw[b] != 0xFF;
                n++;
            } else if (tag[0] == 0xAA && tag[1] == 0x55 && tag[2] == 0xFF) {
                bool parity = false;
                for (unsigned b = 520; b < 528; b++) parity |= m->raw[b] != 0xFF;
                erased += parity;
            }
        }
        for (unsigned a = 0; a < n; a++) {
            unsigned votes = 0, protected_votes = 0;
            for (unsigned b = 0; b < n; b++) {
                votes += ids[a] == ids[b];
                protected_votes += ids[a] == ids[b] && protected[b];
            }
            if (votes < 3) continue;
            good = protected_votes >= 3;
            if (ids[a] >= NAND_BOOT_BLOCKS) break;
            if (m->block[ids[a]] != NAND_BOOT_NONE) return -1;
            m->block[ids[a]] = (uint16_t)phys;
            m->protected_block[ids[a]] = protected_votes >= 3;
            break;
        }
        if (failed || !(good || erased >= 3)) m->quarantine[phys/8] |= 1u << (phys%8);
    }
    if (m->protected)
        for (unsigned i = 0; i < NAND_BOOT_BLOCKS; i++)
            if (!m->protected_block[i]) m->block[i] = NAND_BOOT_NONE;
    return 0;
}

int nand_boot_read(struct nand_boot_map *m, nand_boot_raw_fn read, void *ctx,
                   uint32_t logical_page, void *data, bool allow_no_ecc)
{
    if (logical_page >= NAND_BOOT_BLOCKS * 32 ||
        m->block[logical_page / 32] == NAND_BOOT_NONE) return -1;
    uint32_t page = (uint32_t)m->block[logical_page / 32] * 32 + logical_page % 32;
    if (checked(m, read, ctx, page, allow_no_ecc && !m->protected)) return -1;
    if (m->protected && !m->protected_block[logical_page / 32]) return -1;
    uint8_t *dest = data;
    for (unsigned i = 0; i < 512; i++) dest[i] = m->raw[i];
    return 0;
}
