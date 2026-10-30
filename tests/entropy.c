/* Production seed wrapper: persistence fast path, provider errors and wiping. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <glaze/fs.h>
#include <glaze/random.h>
#include <glaze/thread.h>
#include "../third_party/jitterentropy/jitterentropy.h"

static int file_size = GZ_ENOENT, init_error, alloc_error, read_error;
static int calls, locked;
static struct rand_data collector;
uint64_t entropy_test_time(void) { return 0; }
int fs_read_file(const char *path, void *out, uint32_t size)
{
    assert(!strcmp(path, "/seed"));
    if (file_size < 0) return file_size;
    int n = file_size < (int)size ? file_size : (int)size;
    memset(out, 0x37, (size_t)n);
    return n;
}
void gz_mutex_lock(struct gz_mutex *m) { (void)m; assert(!locked); locked = 1; }
void gz_mutex_unlock(struct gz_mutex *m) { (void)m; assert(locked); locked = 0; }
int jent_entropy_init_ex(unsigned osr, unsigned flags)
{
    assert(locked && osr == 3 && (flags & JENT_FORCE_FIPS));
    calls++; return init_error;
}
struct rand_data *jent_entropy_collector_alloc(unsigned osr, unsigned flags)
{
    (void)osr; (void)flags; assert(locked);
    return alloc_error ? NULL : &collector;
}
void jent_entropy_collector_free(struct rand_data *p) { assert(p == &collector && locked); }
ssize_t jent_read_entropy(struct rand_data *p, char *out, size_t size)
{
    assert(p == &collector && locked);
    memset(out, 0xa5, size); /* Even partial/error outputs must be wiped. */
    return read_error ? read_error : (int)size;
}
static void filled(const uint8_t *p, uint8_t value)
{
    for (unsigned i = 0; i < 32; i++) assert(p[i] == value);
}
int main(void)
{
    uint8_t out[32];
    assert(gz_entropy_seed(NULL, 32) == GZ_EINVAL);
    assert(gz_entropy_seed(out, 0) == GZ_EINVAL);
    assert(gz_entropy_seed(out, 257) == GZ_EINVAL && !calls);
    assert(!gz_entropy_seed(out, 32)); filled(out, 0xa5);
    file_size = 32; calls = 0;
    assert(!gz_entropy_load_seed("/seed", out) && !calls); filled(out, 0x37);
    const int invalid[] = {GZ_ENOENT, 0, 1, 31, 33, 40};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        file_size = invalid[i]; calls = 0;
        assert(!gz_entropy_load_seed("/seed", out) && calls == 1); filled(out, 0xa5);
    }
    init_error = ERCT;
    assert(gz_entropy_seed(out, 32) == GZ_EIO && !locked); filled(out, 0);
    init_error = 0; alloc_error = 1;
    assert(gz_entropy_seed(out, 32) == GZ_ENOMEM && !locked); filled(out, 0);
    alloc_error = 0; read_error = -3;
    assert(gz_entropy_seed(out, 32) == GZ_EIO && !locked); filled(out, 0);
    read_error = 31;
    assert(gz_entropy_seed(out, 32) == GZ_EIO && !locked); filled(out, 0);
    puts("entropy: saved seeds, missing/malformed files, failures and output wiping PASS");
}
