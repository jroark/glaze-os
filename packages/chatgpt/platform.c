/* HTTP is used only by the network worker, so a bounded synchronous DNS
 * lookup does not block the UI. No shared resolver state is needed. */
#include "plat.h"
#include "web.h"
#include <glaze/fs.h>
#include <glaze/net.h>
#include <glaze/notify.h>
#include <glaze/res.h>
#include <glaze/sys.h>
#include <stdio.h>
static int resolved;
static uint32_t address;
int plat_resolve_start(const char *host)
{
    int rv = net_resolve(host, &address, 2000);
    resolved = rv < 0 ? (rv == GZ_ENETDOWN ? WE_NETDOWN : rv == GZ_ETIMEDOUT ? WE_TIMEOUT : WE_DNS) : 1;
    return 0;
}
int plat_resolve_poll(int id, uint32_t *out)
{
    (void)id; *out = address; return resolved;
}
void plat_resolve_cancel(int id) { (void)id; }
uint32_t plat_ms(void) { return gz_time_ms(); }
const void *plat_roots(uint32_t *size) { return gz_romfs_map("ssl/roots.der", size); }
long long plat_time(void)
{
    struct gz_datetime dt;
    return gz_rtc_get(&dt) < 0 ? 0 : (long long)gz_dt_to_secs(&dt) + 946684800;
}
void plat_noise(uint8_t *buf, int n)
{
    for (int i = 0; i < n; i++) {
        gz_yield(); uint64_t t = gz_time_us();
        buf[i] = (uint8_t)(t ^ (t >> 8) ^ (t >> 16));
    }
}
static void seed_path(char *path) { snprintf(path, FS_PATH_MAX, "%s/data/seed", res_bundle()); }
void plat_load_seed(uint8_t *seed)
{
    char path[FS_PATH_MAX]; seed_path(path); fs_read_file(path, seed, PLAT_SEED_LEN);
}
void plat_save_seed(const uint8_t *seed)
{
    char path[FS_PATH_MAX]; seed_path(path); fs_write_file(path, seed, PLAT_SEED_LEN);
}
