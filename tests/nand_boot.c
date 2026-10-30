/* Independent encoder from the ECC test supplies physical tagged pages. */
#define main ecc_reference_main
#include "nand_ecc.c"
#undef main
#include <assert.h>
#include "../kernel/drivers/nand_boot.h"
static int logical[1024];
static unsigned faults;
static int read_raw(void *ctx, uint32_t page, uint8_t raw[528])
{
    (void)ctx;
    unsigned block = page / 32, within = page % 32;
    assert(block < 1024);
    memset(raw, 0xFF, 528);
    if (logical[block] < 0) return 0;
    for (unsigned i = 0; i < 512; i++) raw[i] = (uint8_t)(logical[block] + within + i);
    raw[512] = 0xAA; raw[513] = 0x55; raw[514] = 0x0F;
    unsigned id = (unsigned)logical[block];
    raw[516] = (uint8_t)id; raw[517] = (uint8_t)(id >> 8); raw[518] = raw[519] = 0;
    if ((faults & 1) && within < 2) raw[514] = 0; /* only three valid tags */
    if ((faults & 2) && within < 3) raw[514] = 0; /* no majority */
    encode(raw);
    if ((faults & 4) || ((faults & 128) && logical[block] == 8)) memset(raw + 520, 0xFF, 8); /* data-only emulator */
    if (faults & 8) raw[15] ^= 0x83;
    if (faults & 16) raw[518] ^= 0x41; /* ECC must repair a block ID */
    if (faults & 32) { raw[15] ^= 0x51; raw[32] ^= 0x63; raw[90] ^= 0x75; }
    if ((faults & 64) && within == 31) return -1;
    return 0;
}
int main(void)
{
    static struct nand_boot_map map;
    uint8_t data[512];
    for (unsigned i = 0; i < 1024; i++) logical[i] = -1;
    logical[2] = 0; logical[17] = 8; logical[1005] = 16;
    assert(!nand_boot_scan(&map, read_raw, NULL, false));
    assert(map.block[0] == 2 && map.block[8] == 17 && map.block[16] == 1005);
    for (unsigned i = 0; i < 3; i++) {
        unsigned block = i == 0 ? 0 : i == 1 ? 8 : 16;
        for (unsigned page = 0; page < 32; page += 31) {
            assert(!nand_boot_read(&map, read_raw, NULL, block * 32 + page, data, false));
            for (unsigned b = 0; b < 512; b++) assert(data[b] == (uint8_t)(block + page + b));
        }
    }
    assert(nand_boot_read(&map, read_raw, NULL, 32, data, false) < 0);
    assert(nand_boot_read(&map, read_raw, NULL, NAND_BOOT_BLOCKS * 32, data, false) < 0);
    faults = 1 | 8 | 16;
    assert(!nand_boot_scan(&map, read_raw, NULL, false));
    assert(map.block[8] == 17 && map.corrected == 30);
    assert(!nand_boot_read(&map, read_raw, NULL, 8*32+31, data, false));
    assert(data[15] == (uint8_t)(8+31+15));
    faults = 2;
    assert(!nand_boot_scan(&map, read_raw, NULL, false));
    assert(map.block[8] == NAND_BOOT_NONE);
    faults = 4;
    assert(!nand_boot_scan(&map, read_raw, NULL, false));
    assert(map.block[8] == NAND_BOOT_NONE);
    assert(!nand_boot_scan(&map, read_raw, NULL, true));
    assert(map.block[8] == 17);
    assert(nand_boot_read(&map, read_raw, NULL, 8*32, data, false) < 0);
    faults = 128;
    assert(!nand_boot_scan(&map, read_raw, NULL, true));
    assert(map.protected && map.block[0] == 2 && map.block[8] == NAND_BOOT_NONE);
    faults = 0; logical[18] = 8;
    assert(nand_boot_scan(&map, read_raw, NULL, false) < 0);
    logical[18] = -1;
    assert(!nand_boot_scan(&map, read_raw, NULL, false));
    memset(data, 0xA5, sizeof(data)); faults = 32;
    assert(nand_boot_read(&map, read_raw, NULL, 8*32, data, false) < 0);
    for (unsigned i = 0; i < 512; i++) assert(data[i] == 0xA5);
    faults = 4;
    assert(nand_boot_read(&map, read_raw, NULL, 8*32, data, true) < 0);
    faults = 64;
    assert(nand_boot_read(&map, read_raw, NULL, 8*32+31, data, false) < 0);
    puts("nand-boot: relocation, tag quorum, ECC repair/rejection, missing/duplicate maps and I/O failure PASS");
}
