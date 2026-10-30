/* Platform hooks for the existing verified TLS transport. Only the radio
 * network worker uses TLS; no resolver thread or browser engine is linked. */
#include "plat.h"
#include <glaze/fs.h>
#include <glaze/random.h>
#include <glaze/notify.h>
#include <glaze/sys.h>

const void *plat_roots(uint32_t *size) { return gz_romfs_map("ssl/roots.der", size); }
long long plat_time(void)
{
    struct gz_datetime dt;
    return gz_rtc_get(&dt) < 0 ? 0 : (long long)gz_dt_to_secs(&dt) + 946684800;
}
void plat_noise(uint8_t *buf, int n)
{
    for (int i = 0; i < n; i++) {
        gz_yield();
        uint64_t t = gz_time_us();
        buf[i] = (uint8_t)(t ^ (t >> 8) ^ (t >> 16));
    }
}
int plat_load_seed(uint8_t *seed) { return gz_entropy_load_seed("/Library/Caches/radio.seed", seed); }
void plat_save_seed(const uint8_t *seed) { fs_write_file("/Library/Caches/radio.seed", seed, PLAT_SEED_LEN); }
