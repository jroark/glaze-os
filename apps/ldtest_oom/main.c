#include <glaze/sys.h>
#include <dlfcn.h>
int main(void)
{
    /* Warm loader metadata so the failure happens part-way through mapping. */
    void *h = dlopen("libld-bss.so.1", RTLD_NOW);
    if (!h || dlclose(h)) return 2;
    struct gz_sysinfo before, after;
    for (;;) {
        gz_sysinfo(&before);
        if (before.free_pages <= 8 || !gz_mmap_anon(4096)) break;
    }
    gz_sysinfo(&before);
    h = dlopen("libld-bss.so.1", RTLD_NOW);
    gz_sysinfo(&after);
    struct gz_msg m = {{0}}, r;
    m.w[0] = !h && before.free_pages == after.free_pages;
    m.w[1] = before.free_pages; m.w[2] = after.free_pages;
    gz_send(gz_port_lookup("ldtest"), &m, &r);
    return m.w[0] ? 0 : 1;
}
