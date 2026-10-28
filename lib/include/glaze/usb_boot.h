/* Pure validation shared by the CLI, reload path and native tests. */
#ifndef GLAZE_USB_BOOT_H
#define GLAZE_USB_BOOT_H
#include <glaze/usb_wire.h>
#define GU_RAM_BYTES 0x1000000u
struct gu_layout { uint32_t load, entry, size, end, romfs, ramfs, ramfs_len; };
static inline int gu_boot_layout(const uint8_t *h, uint32_t kernel_bytes,
                                 uint32_t romfs_bytes, uint32_t ramfs_kb,
                                 struct gu_layout *l)
{
    if (kernel_bytes < 4096 || kernel_bytes > GU_RAM_BYTES || romfs_bytes < 8 ||
        romfs_bytes > 0x4c0000 || ramfs_kb < 128 || ramfs_kb > 11264 ||
        gu_get32(h) != 0x424b5a47u) return -1;
    uint32_t load = gu_get32(h + 4), entry = gu_get32(h + 8), end = gu_get32(h + 16);
    l->size = gu_get32(h + 12);
    if (load < 0x80010000u || load >= 0x81000000u || load % 4096 ||
        !l->size || l->size != kernel_bytes - 4096 || l->size > 0x81000000u - load ||
        end < load + l->size || end > 0x81000000u ||
        entry < load || entry >= load + l->size || entry % 4) return -1;
    l->load = load & 0x1fffffffu; l->entry = entry;
    l->end = ((end & 0x1fffffffu) + 4095) & ~4095u;
    l->romfs = (GU_RAM_BYTES - ((romfs_bytes + 4095) & ~4095u)) & ~8191u;
    l->ramfs_len = (ramfs_kb * 1024 + 16383) & ~16383u;
    if (l->romfs < l->ramfs_len || l->romfs - l->ramfs_len < l->end) return -1;
    l->ramfs = l->romfs - l->ramfs_len;
    return 0;
}
#endif
