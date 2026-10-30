/* Real upstream startup checks must reject frozen and predictable clocks. */
#include <assert.h>
#include <stdio.h>
#include "../third_party/jitterentropy/jitterentropy.h"
static uint64_t clock_count;
static int increment;
uint64_t entropy_test_time(void) { return increment ? ++clock_count : 1; }
int main(void)
{
    unsigned flags = JENT_FORCE_FIPS | JENT_MAX_MEMSIZE_32kB;
    assert(jent_entropy_init_ex(3, flags) != 0);
    increment = 1;
    assert(jent_entropy_init_ex(3, flags) != 0);
    puts("entropy: upstream rejects frozen and deterministic clocks PASS");
}
