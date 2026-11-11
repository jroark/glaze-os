/* servers/fs/ftl.c's table-driven BCH remainder against Dhara's bit-serial
 * one, which wrote the codes on existing volumes: same code bytes, and the
 * same verdict on matching, damaged and unrelated codes. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int ftl_bch_selftest(const uint8_t *data, size_t len, const uint8_t *other_ecc);
int flash_raw_is_bad(uint32_t block) { (void)block; return 0; }
void ftl_log(const char *msg) { (void)msg; }
int flash_raw_read(uint32_t page, uint8_t *buf) { (void)page; (void)buf; return -1; }
int flash_raw_prog(uint32_t page, const uint8_t *buf) { (void)page; (void)buf; return -1; }
int flash_raw_erase(uint32_t block) { (void)block; return -1; }

static uint32_t state = 0x12345678u;

static uint32_t next(void)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

int main(void)
{
    static const size_t lens[] = { 1, 7, 8, 480, 511, 512 };
    uint8_t data[512], other[8];

    for (int round = 0; round < 20000; round++) {
        size_t len = lens[round % (sizeof(lens) / sizeof(lens[0]))];
        int fill = round % 5;

        for (size_t i = 0; i < sizeof(data); i++)
            data[i] = fill == 0 ? 0xFF : fill == 1 ? 0x00 : (uint8_t)next();
        for (size_t i = 0; i < sizeof(other); i++)
            other[i] = (uint8_t)next();
        int r = ftl_bch_selftest(data, len, other);
        if (r) {
            printf("ftl bch: FAIL %d at round %d, length %zu\n", r, round, len);
            return 1;
        }
    }
    puts("ftl bch: table-driven codes match Dhara's PASS");
    return 0;
}
