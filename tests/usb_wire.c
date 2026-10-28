#include <glaze/usb_wire.h>
#include <glaze/usb_boot.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    assert(~gu_crc(~0u, "123456789", 9) == 0xcbf43926u);
    uint8_t data[GU_PAYLOAD], f[GU_FRAME];
    for (unsigned i = 0; i < sizeof(data); i++) data[i] = (uint8_t)i;
    struct gu_parser s = {0};
    unsigned sizes[] = {0,1,40,63,64,65,128,4096};
    for (unsigned j = 0; j < sizeof(sizes) / sizeof(sizes[0]); j++) {
        unsigned n = sizes[j]; size_t size = gu_encode(f, GU_PUT_DATA, 123, -5, data, n);
        assert(size == n + GU_HEADER);
        for (unsigned repeat = 0; repeat < 3; repeat++) {
            assert(!gu_feed(&s, 0xff)); assert(!gu_feed(&s, 0));
            for (size_t i = 0; i < size; i++) assert(gu_feed(&s, f[i]) == (i == size - 1 ? 1 : 0));
            assert(gu_get32(s.bytes + 8) == 123 && (int32_t)gu_get32(s.bytes + 16) == -5);
            assert(!memcmp(s.bytes + GU_HEADER, data, n));
        }
        f[20] ^= 1;
        for (size_t i = 0; i < size; i++) assert(gu_feed(&s, f[i]) == (i == size - 1 ? -1 : 0));
    }
    assert(!gu_encode(f, 1, 1, 0, data, GU_PAYLOAD + 1));
    gu_encode(f, 1, 1, 0, NULL, 0); gu_put32(f + 12, ~0u);
    for (unsigned i = 0; i < GU_HEADER; i++) assert(gu_feed(&s, f[i]) == (i == GU_HEADER - 1 ? -1 : 0));
    uint8_t h[32] = {0}; struct gu_layout l;
    gu_put32(h, 0x424b5a47); gu_put32(h + 4, 0x80010000); gu_put32(h + 8, 0x80010000);
    gu_put32(h + 12, 65536); gu_put32(h + 16, 0x80030000);
    assert(!gu_boot_layout(h, 65536 + 4096, 4 * 1024 * 1024, 2048, &l));
    assert(l.romfs == 0xc00000 && l.ramfs == 0xa00000);
    assert(gu_boot_layout(h, 65536 + 4095, 4096, 2048, &l));
    gu_put32(h + 12, ~0u); assert(gu_boot_layout(h, 65536 + 4096, 4096, 2048, &l));
    gu_put32(h + 12, 65536); gu_put32(h + 8, 0x80030000);
    assert(gu_boot_layout(h, 65536 + 4096, 4096, 2048, &l));
    gu_put32(h + 8, 0x80010000);
    gu_put32(h + 16, 0x80060000);
    assert(gu_boot_layout(h, 65536 + 4096, 0x4c0000, 11264, &l));
    puts("usb-wire: ALL PASS"); return 0;
}
