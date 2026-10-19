#include <glaze/sys.h>
#include <dlfcn.h>
int main(void)
{
    void *h = dlopen("libld-simple.so.1", RTLD_NOW);
    if (!h) return 2;
    int (*get)(void) = dlsym(h, "simple_value");
    void (*set)(int) = dlsym(h, "simple_set");
    struct gz_image_page page;
    if (!get || !set || gz_image_query(get, &page)) return 3;
    int port = gz_port_lookup("ldtest");
    struct gz_msg m = {{0}}, r;
    m.w[0] = (uint32_t)get(); m.w[1] = page.frame; m.w[2] = page.users; m.w[3] = page.writable;
    if (gz_send(port, &m, &r)) return 4;
    if (r.w[0] == 1) for (;;) gz_sleep_ms(1000);
    set(22); m.w[0] = (uint32_t)get();
    if (gz_send(port, &m, &r)) return 5;
    dlclose(h); return 0;
}
