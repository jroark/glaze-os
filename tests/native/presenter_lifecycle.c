#include "../../kernel/presenter.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned loaded, unloaded, started, polled, stopped;
static int load_error, init_error;
static bool executable = true;
static int start(uint32_t cor, unsigned index)
{ assert(cor == 0x200 && index == 1); started++; return init_error; }
static void poll_card(void) { polled++; }
static void stop_card(bool present) { stopped++; }
static struct presenter_driver descriptor = { PRESENTER_DRIVER_MAGIC, PRESENTER_DRIVER_ABI,
    sizeof(struct presenter_driver), start, poll_card, stop_card };
int module_load(const char *path, const char *symbol, struct module *m)
{
    loaded++; assert(!strcmp(path,"drivers/presenter.ko"));
    if (load_error) return load_error;
    m->entry = &descriptor; return 0;
}
void module_unload(struct module *m) { if (m->entry) unloaded++; memset(m,0,sizeof(*m)); }
bool module_contains(const struct module *m,const void *p,uint32_t size) { return p == &descriptor; }
bool module_executable(const struct module *m,const void *p) { return executable && p; }
void kputs(const char *s) {}
void kprintf(const char *s,...) {}
int main(void)
{
    presenter_poll(); assert(!loaded);
    presenter_insert(0,1); presenter_poll(); assert(!loaded && !presenter_active());
    presenter_insert(0x200,1); load_error=GZ_EINPROGRESS;
    presenter_poll(); assert(loaded==1 && presenter_active() && !started);
    load_error=0; presenter_poll(); assert(started==1 && polled==1);
    presenter_insert(0x200,1); presenter_poll(); assert(started==1 && polled==2);
    presenter_removed(); presenter_poll(); assert(stopped==1 && unloaded==1 && !presenter_active());
    executable=false; presenter_insert(0x200,1); presenter_poll();
    assert(started==1 && unloaded==2 && !presenter_active());
    executable=true; init_error=GZ_EIO; presenter_insert(0x200,1); presenter_poll();
    assert(started==2 && stopped==2 && unloaded==3 && !presenter_active());
    puts("presenter: deferred loading, descriptor validation, failure cleanup and removal PASS");
}
