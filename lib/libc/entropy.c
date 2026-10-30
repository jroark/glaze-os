#include <glaze/random.h>
#include <glaze/thread.h>
#include <glaze/fs.h>
#include <string.h>
#include "../../third_party/jitterentropy/jitterentropy.h"

static struct gz_mutex entropy_lock = GZ_MUTEX_INIT;

int gz_entropy_seed(void *out, uint32_t size)
{
    if (!out || !size || size > 256) return GZ_EINVAL;
    const unsigned flags = JENT_FORCE_FIPS | JENT_MAX_MEMSIZE_32kB;
    int result = GZ_EIO;
    gz_mutex_lock(&entropy_lock);
    if (!jent_entropy_init_ex(3, flags)) {
        struct rand_data *collector = jent_entropy_collector_alloc(3, flags);
        if (collector) {
            if (jent_read_entropy(collector, out, size) == (int)size) result = 0;
            jent_entropy_collector_free(collector);
        } else result = GZ_ENOMEM;
    }
    if (result) {
        volatile uint8_t *bytes = out;
        for (uint32_t i = 0; i < size; i++) bytes[i] = 0;
    }
    gz_mutex_unlock(&entropy_lock);
    return result;
}

int gz_entropy_load_seed(const char *path, uint8_t seed[32])
{
    if (!path || !seed) return GZ_EINVAL;
    uint8_t saved[33];
    int size = fs_read_file(path, saved, sizeof(saved));
    if (size == 32) memcpy(seed, saved, 32);
    volatile uint8_t *wipe = saved;
    for (unsigned i = 0; i < sizeof(saved); i++) wipe[i] = 0;
    return size == 32 ? 0 : gz_entropy_seed(seed, 32);
}
