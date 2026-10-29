/* Trusted romfs service catalog and a small deferred lifecycle worker.
 * Loading, process creation and teardown never happen in a device IRQ.
 */
#include "kernel.h"
#ifndef SERVICE_TEST
#include "mips.h"
#endif
#include "drivers.h"
#include "service.h"
#include <glaze/service.h>

static struct gz_service_manifest manifest;
static struct {
    int pid, error;
    uint32_t starts, generation;
    uint64_t retry;
    bool enabled;
} states[GZ_SERVICE_MAX];
static uint32_t order[GZ_SERVICE_MAX];

static bool alive(unsigned i) { return proc_by_pid(states[i].pid) != NULL; }

static void reconcile(void)
{
    usbnet_collect();
    uint32_t wanted = 0, healthy = 0, generation = net_generation();
    for (unsigned n = 0; n < manifest.count; n++) {
        unsigned i = order[n];
        const struct gz_service_spec *s = &manifest.entries[i];
        if (states[i].enabled && (!(s->policy & GZ_SERVICE_NETWORK) || net_up()) &&
            (s->after & wanted) == s->after)
            wanted |= 1u << i;
        if ((wanted & (1u << i)) && alive(i) && (s->after & healthy) == s->after &&
            (!(s->policy & GZ_SERVICE_NETWORK) || states[i].generation == generation))
            healthy |= 1u << i;
    }
    /* Stop dependents first, including a process for the previous card. */
    for (unsigned n = manifest.count; n; n--) {
        unsigned i = order[n - 1];
        struct gz_service_spec *s = &manifest.entries[i];
        struct proc *p = proc_by_pid(states[i].pid);
        if ((p && !(healthy & (1u << i))) || (!p && states[i].pid)) {
            if (p && (s->privileges & PROC_AUDIO)) sys_audio_stop(p, 1);
            if (s->privileges & PROC_USB) { usb_input_cancel(); usbnet_stop(); }
            if (p) proc_exit(p, 0);
            kprintf("service: stopped %s\n", s->name);
        }
        if (!alive(i)) states[i].pid = 0;
    }
    uint32_t running = 0;
    for (unsigned n = 0; n < manifest.count; n++) {
        unsigned i = order[n];
        struct gz_service_spec *s = &manifest.entries[i];
        if ((wanted & (1u << i)) && !states[i].pid && time_us() >= states[i].retry &&
            (s->after & running) == s->after) {
            struct proc *p = proc_spawn(s->path, 0, s->privileges);
            if (p) {
                p->net_generation = generation;
                states[i].pid = p->pid;
                states[i].starts++;
                states[i].retry = time_us() + 1000000u;
                states[i].error = 0;
                states[i].generation = generation;
                kprintf("service: started %s pid %d\n", s->name, p->pid);
            } else {
                states[i].error = GZ_ENOMEM;
                states[i].retry = time_us() + 1000000u;
                kprintf("service: cannot start %s; retrying\n", s->name);
            }
        }
        if (states[i].pid) running |= 1u << i;
    }
}

static void service_worker(void)
{
    for (;;) {
        uint32_t flags = irq_save();
        pcmcia_poll();
        reconcile();
        thread_sleep_until(time_us() + 100000u);
        irq_restore(flags);
    }
}

void services_init(void)
{
    const struct romfs_entry *file = romfs_lookup("etc/services");
    if (!file || file->size < 12 || file->size > sizeof(manifest) ||
        romfs_read(file, 0, &manifest, file->size) || manifest.magic != GZ_SERVICES_MAGIC ||
        manifest.abi != GZ_SERVICES_ABI || !manifest.count || manifest.count > GZ_SERVICE_MAX ||
        file->size != 12 + manifest.count * sizeof(manifest.entries[0]))
        panic("invalid service catalog");
    uint32_t done = 0, count = 0, all = (1u << manifest.count) - 1;
    for (unsigned i = 0; i < manifest.count; i++) {
        struct gz_service_spec *s = &manifest.entries[i];
        if (!s->name[0] || !memchr(s->name, 0, sizeof(s->name)) ||
            !memchr(s->path, 0, sizeof(s->path)) || strncmp(s->path, "bin/", 4) ||
            (s->privileges & ~(PROC_PRIVILEGED | PROC_FLASH | PROC_NET | PROC_AUDIO | PROC_USB)) ||
            (s->policy & ~(GZ_SERVICE_NETWORK | GZ_SERVICE_REQUIRED)) || (s->after & ~all) ||
            ((s->privileges & PROC_NET) && !(s->policy & GZ_SERVICE_NETWORK)))
            panic("invalid service definition");
        for (unsigned j = 0; j < i; j++)
            if (!strcmp(manifest.entries[j].name, s->name)) panic("duplicate service");
        states[i].enabled = true;
    }
    for (unsigned pass = 0; pass < manifest.count; pass++)
        for (unsigned i = 0; i < manifest.count; i++)
            if (!(done & (1u << i)) && (manifest.entries[i].after & done) == manifest.entries[i].after) {
                order[count++] = i; done |= 1u << i;
            }
    if (done != all) panic("cyclic service dependencies");
    reconcile();
    for (unsigned i = 0; i < manifest.count; i++)
        if ((manifest.entries[i].policy & GZ_SERVICE_REQUIRED) && !states[i].pid)
            panic("required service %s unavailable", manifest.entries[i].name);
    if (!thread_create_kernel(service_worker, PRIO_NORMAL)) panic("no service worker");
}

int sys_services(struct proc *p, uint32_t output, uint32_t size)
{
    struct gz_service_snapshot snapshot = {0};
    if (size != sizeof(snapshot)) return GZ_EINVAL;
    snapshot.count = manifest.count;
    snapshot.network_kind = net_driver_kind();
    snapshot.driver_pages = net_driver_pages();
    snapshot.generation = net_generation();
    for (unsigned i = 0; i < manifest.count; i++) {
        struct gz_service_info *s = &snapshot.entries[i];
        memcpy(s->name, manifest.entries[i].name, sizeof(s->name));
        s->pid = alive(i) ? states[i].pid : 0;
        s->starts = states[i].starts;
        s->error = states[i].error;
        s->state = s->pid ? GZ_SERVICE_RUNNING : !states[i].enabled ? GZ_SERVICE_DISABLED :
                   s->error ? GZ_SERVICE_FAILED : GZ_SERVICE_WAITING;
    }
    return copy_to_as(&p->as, output, &snapshot, sizeof(snapshot));
}

int sys_service_set(struct proc *p, uint32_t uname, int enabled)
{
    char name[GZ_SERVICE_NAME];
    if (!(p->flags & PROC_PRIVILEGED)) return GZ_EPERM;
    if ((enabled != 0 && enabled != 1) || copy_str_from_as(&p->as, name, uname, sizeof(name)) < 0) return GZ_EINVAL;
    for (unsigned i = 0; i < manifest.count; i++) {
        if (strcmp(name, manifest.entries[i].name)) continue;
        if (!enabled && (manifest.entries[i].policy & GZ_SERVICE_REQUIRED)) return GZ_EPERM;
        states[i].enabled = enabled;
        states[i].retry = 0;
        states[i].error = 0;
        return 0; /* deferred: never tear down the calling process's stack */
    }
    return GZ_ENOENT;
}
