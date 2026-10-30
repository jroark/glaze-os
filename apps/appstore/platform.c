/* Device services for the shared HTTP/TLS client. DNS runs off the UI thread. */
#include "net.h"
#include "plat.h"
#include "web.h"
#include <glaze/fs.h>
#include <glaze/random.h>
#include <glaze/net.h>
#include <glaze/notify.h>
#include <glaze/sys.h>
#include <glaze/thread.h>
#include <string.h>
static struct gz_mutex lock = GZ_MUTEX_INIT;
static int wake;
static struct {
    int state, rv;
    bool cancelled;
    char host[128];
    uint32_t addr;
} dns[4];
static void resolver(void *arg)
{
    (void)arg;
    for (;;) {
        gz_sem_wait(wake, -1);
        for (int i = 0; i < 4; i++) {
            char host[128];
            gz_mutex_lock(&lock);
            if (dns[i].state != 1) {
                gz_mutex_unlock(&lock);
                continue;
            }
            dns[i].state = 2;
            strlcpy(host, dns[i].host, sizeof(host));
            gz_mutex_unlock(&lock);
            uint32_t addr = 0;
            int rv = net_resolve(host, &addr, 15000);
            gz_mutex_lock(&lock);
            dns[i].addr = addr;
            dns[i].rv = rv < 0 ? (rv == GZ_ENETDOWN    ? WE_NETDOWN
                                  : rv == GZ_ETIMEDOUT ? WE_TIMEOUT
                                                       : WE_DNS)
                               : 1;
            dns[i].state = dns[i].cancelled ? 0 : 3;
            gz_mutex_unlock(&lock);
        }
    }
}
int plat_resolve_start(const char *host)
{
    if (!wake) {
        wake = gz_sem_create(0);
        if (wake < 0) {
            wake = 0;
            return WE_NOMEM;
        }
        if (gz_thread_create(resolver, NULL) < 0) {
            gz_sem_destroy(wake);
            wake = 0;
            return WE_NOMEM;
        }
    }
    gz_mutex_lock(&lock);
    for (int i = 0; i < 4; i++)
        if (!dns[i].state) {
            strlcpy(dns[i].host, host, sizeof(dns[i].host));
            dns[i].cancelled = false;
            dns[i].state = 1;
            gz_mutex_unlock(&lock);
            gz_sem_post(wake);
            return i;
        }
    gz_mutex_unlock(&lock);
    return WE_NOMEM;
}
int plat_resolve_poll(int id, uint32_t *addr)
{
    gz_mutex_lock(&lock);
    int rv = 0;
    if (dns[id].state == 3) {
        *addr = dns[id].addr;
        rv = dns[id].rv;
        dns[id].state = 0;
    }
    gz_mutex_unlock(&lock);
    return rv;
}
void plat_resolve_cancel(int id)
{
    gz_mutex_lock(&lock);
    if (dns[id].state == 2)
        dns[id].cancelled = true;
    else
        dns[id].state = 0;
    gz_mutex_unlock(&lock);
}
uint32_t plat_ms(void)
{
    return gz_time_ms();
}
const void *plat_roots(uint32_t *n)
{
    return gz_romfs_map("ssl/roots.der", n);
}
long long plat_time(void)
{
    struct gz_datetime dt;
    if (gz_rtc_get(&dt) < 0)
        return 0;
    return (long long)gz_dt_to_secs(&dt) + 946684800;
}
void plat_noise(uint8_t *buf, int n)
{
    for (int i = 0; i < n; i++) {
        gz_yield();
        uint64_t t = gz_time_us();
        buf[i] = (uint8_t)(t ^ (t >> 8) ^ (t >> 16));
    }
}
int plat_load_seed(uint8_t *seed)
{
    return gz_entropy_load_seed("/Library/Caches/AppStore/seed", seed);
}
void plat_save_seed(const uint8_t *seed)
{
    fs_write_file("/Library/Caches/AppStore/seed", seed, PLAT_SEED_LEN);
}
