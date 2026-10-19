#include <stdlib.h>
extern int dep_value, dep_add(int), ldtest_callback(int);
extern void ldtest_event(int);
extern int absent_weak(void) __attribute__((weak));
int plugin_value = 7;
int *plugin_pointer = &plugin_value;
int (*plugin_function_pointer)(int) = dep_add;
int plugin_step(int n) { return plugin_function_pointer(*plugin_pointer + n) + ldtest_callback(dep_value); }
int plugin_weak(void) { return absent_weak == 0; }
void *plugin_alloc(int n) { return malloc((size_t)n); }
__attribute__((constructor)) static void init(void) { plugin_value = 17; ldtest_event(2); }
__attribute__((destructor)) static void fini(void) { ldtest_event(-2); }
