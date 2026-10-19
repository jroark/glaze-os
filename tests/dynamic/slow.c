#include <glaze/sys.h>
static int ready;
int slow_value(void) { return ready; }
__attribute__((constructor)) static void init(void) { gz_sleep_ms(20); ready = 123; }
__attribute__((destructor)) static void fini(void) { gz_sleep_ms(5); }
