/* Production lifecycle policy with deterministic processes, time and hardware. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/proc.h"
#include <glaze/service.h>
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t flags) {}
#define SERVICE_TEST
#include "../kernel/service.c"
static struct proc processes[64];
static unsigned spawned, exited;
static uint64_t clock_us;
static bool connected, fail_spawn;
static uint32_t card_generation;
static char control_name[16];
static struct gz_service_snapshot observed;
static struct gz_service_manifest catalog = {
    GZ_SERVICES_MAGIC, GZ_SERVICES_ABI, 3,
    { {"fs", "bin/fs", PROC_FLASH, GZ_SERVICE_REQUIRED, 0},
      {"net", "bin/net", PROC_NET, GZ_SERVICE_NETWORK, 1},
      {"audio", "bin/audio", PROC_AUDIO, 0, 1} }
};
static struct romfs_entry file = { .size = 12 + 3 * sizeof(struct gz_service_spec) };
const struct romfs_entry *romfs_lookup(const char *path) { return &file; }
int romfs_read(const struct romfs_entry *f, uint32_t off, void *p, uint32_t n)
{ memcpy(p, (uint8_t *)&catalog + off, n); return 0; }
void kprintf(const char *fmt, ...) {}
void panic(const char *fmt, ...) { abort(); }
uint64_t time_us(void) { return clock_us; }
bool net_up(void) { return connected; }
uint32_t net_driver_kind(void) { return connected ? GZ_NET_NE2000 : 0; }
uint32_t net_driver_pages(void) { return connected ? 6 : 0; }
uint32_t net_generation(void) { return card_generation; }
void pcmcia_poll(void) {}
int sys_audio_stop(struct proc *p, uint32_t flush) { return 0; }
struct proc *proc_by_pid(int pid)
{
    for (unsigned i = 0; i < spawned; i++)
        if (processes[i].pid == pid && !processes[i].dying) return &processes[i];
    return NULL;
}
struct proc *proc_spawn(const char *path, int parent, uint32_t flags)
{
    if (fail_spawn) return NULL;
    assert(spawned < 64);
    struct proc *p = &processes[spawned++];
    p->pid = spawned + 32; p->flags = flags;
    return p;
}
void proc_exit(struct proc *p, int code) { p->dying = true; exited++; }
struct thread *thread_create_kernel(void (*entry)(void), int prio)
{ static struct thread worker; return &worker; }
void thread_sleep_until(uint64_t time) {}
int copy_to_as(struct addrspace *as, uint32_t out, const void *in, uint32_t size)
{ assert(size == sizeof(observed)); memcpy(&observed, in, size); return 0; }
int copy_str_from_as(struct addrspace *as, char *out, uint32_t in, uint32_t size)
{ strcpy(out, control_name); return (int)strlen(out); }
int main(void)
{
    struct proc admin = { .flags = PROC_PRIVILEGED }, user = {0};
    services_init();
    assert(states[0].pid && !states[1].pid && states[2].pid && spawned == 2);
    assert(proc_by_pid(states[0].pid)->flags == PROC_FLASH);
    assert(proc_by_pid(states[2].pid)->flags == PROC_AUDIO);
    assert(!sys_services(&user, 1, sizeof(observed)));
    assert(!observed.network_kind && !observed.driver_pages && observed.entries[1].state == GZ_SERVICE_WAITING);
    strcpy(control_name, "net");
    assert(sys_service_set(&user, 0, 0) == GZ_EPERM);
    strcpy(control_name, "fs"); assert(sys_service_set(&admin, 0, 0) == GZ_EPERM);
    strcpy(control_name, "unknown"); assert(sys_service_set(&admin, 0, 1) == GZ_ENOENT);
    strcpy(control_name, "net");
    connected = true; card_generation++; reconcile();
    int first = states[1].pid; assert(first && spawned == 3);
    assert(proc_by_pid(first)->flags == PROC_NET);
    assert(proc_by_pid(first)->net_generation == card_generation);
    assert(!sys_service_set(&admin, 0, 0)); reconcile();
    assert(!states[1].pid && !proc_by_pid(first) && exited == 1);
    assert(!sys_service_set(&admin, 0, 1)); reconcile();
    assert(states[1].pid && states[1].pid != first);
    first = states[1].pid;
    proc_exit(proc_by_pid(first), -1); reconcile();
    assert(!states[1].pid); /* crash backoff */
    clock_us += 1000000; reconcile(); assert(states[1].pid && states[1].pid != first);
    first = states[1].pid;
    card_generation++; reconcile(); assert(!proc_by_pid(first));
    clock_us += 1000000; reconcile(); assert(states[1].pid);
    first = states[1].pid;
    connected = false; card_generation++; reconcile();
    assert(!states[1].pid && !proc_by_pid(first) && states[0].pid && states[2].pid);
    int old_fs = states[0].pid, old_audio = states[2].pid;
    proc_exit(proc_by_pid(old_fs), -1); clock_us += 1000000; reconcile();
    assert(states[0].pid != old_fs && states[2].pid != old_audio && !proc_by_pid(old_audio));
    connected = true; card_generation++; fail_spawn = true; reconcile();
    assert(!states[1].pid && states[1].error == GZ_ENOMEM);
    unsigned count = spawned; reconcile(); assert(count == spawned);
    fail_spawn = false; clock_us += 1000000; reconcile(); assert(states[1].pid);
    puts("services: capabilities, absent hardware, start/stop, restart, backoff and dependency teardown PASS");
    return 0;
}
