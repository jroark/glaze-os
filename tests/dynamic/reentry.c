#include <dlfcn.h>
extern void ldtest_event(int);
static void *dep;
int reentry_ok(void) { return dep != 0; }
__attribute__((constructor)) static void init(void) {
    dep = dlopen("libld-dep.so.1", RTLD_NOW);
    ldtest_event(3);
}
__attribute__((destructor)) static void fini(void) {
    dlclose(dep); ldtest_event(-3);
}
