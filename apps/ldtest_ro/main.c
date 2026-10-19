#include <glaze/sys.h>
#include <dlfcn.h>
int main(void)
{
    void *h = dlopen("libld-simple.so.1", RTLD_NOW);
    void *f = h ? dlsym(h, "simple_value") : 0;
    if (!f) return 2;
    *(volatile uint32_t *)f = 0;
    return 3;
}
