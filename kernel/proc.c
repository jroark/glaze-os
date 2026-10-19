/* Process lifecycle: spawn from romfs, exit, and reaping. */

#include "kernel.h"
#include "mips.h"
#include "proc.h"

static struct proc procs[MAX_PROCS];
static int pid_gen = 1;

struct proc *proc_by_pid(int pid)
{
    if (pid <= 0)
        return NULL;
    struct proc *p = &procs[pid & (MAX_PROCS - 1)];
    return (p->used && p->pid == pid && !p->dying) ? p : NULL;
}

int proc_count(void)
{
    int n = 0;

    for (int i = 0; i < MAX_PROCS; i++)
        n += procs[i].used && !procs[i].dying;
    return n;
}

struct proc *proc_spawn(const char *path, int parent, uint32_t flags)
{
    uint32_t size, entry;
    const void *image = romfs_find(path, &size);
    struct proc *p = NULL;
    int slot;

    if (!image)
        return NULL;
    for (slot = 0; slot < MAX_PROCS; slot++) {
        if (!procs[slot].used) {
            p = &procs[slot];
            break;
        }
    }
    if (!p)
        return NULL;

    memset(p, 0, sizeof(*p));
    /* ASID = slot + 1; ASID 0 is the kernel/idle context. The TLB is
     * flushed when a slot is reaped, so ASIDs are never stale. */
    if (as_create(&p->as, (uint8_t)(slot + 1)))
        return NULL;
    p->used = true;
    p->pid = (pid_gen++ << 5) | slot;
    p->parent = parent;
    p->flags = flags;
    const char *base = path;
    for (const char *c = path; *c; c++) {
        if (*c == '/')
            base = c + 1;
    }
    strncpy(p->name, base, sizeof(p->name) - 1);

    if (elf_load(&p->as, image, size, &entry) ||
        as_alloc_region(&p->as, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE, REGION_OWNED))
        goto fail;

    int prio = (flags & PROC_PRIVILEGED) ? PRIO_HIGH : PRIO_NORMAL;
    p->main = thread_create_user(p, entry, USER_STACK_TOP - 16, (uint32_t)p->pid, prio);
    if (!p->main)
        goto fail;
    thread_make_runnable(p->main);
    kprintf("proc: spawned %s pid %d\n", p->name, p->pid);
    return p;

fail:
    as_destroy(&p->as);
    p->used = false;
    return NULL;
}

void proc_exit(struct proc *p, int code)
{
    if (p->dying)
        return;
    kprintf("proc: %s pid %d exited (%d)\n", p->name, p->pid, code);
    p->dying = true;
    p->exit_code = code;
    ipc_proc_cleanup(p);
    sched_kill_proc_threads(p);
    proc_reap();
}

void proc_reap(void)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &procs[i];
        if (!p->used || !p->dying)
            continue;
        if (sched_reap_threads(p))
            continue;
        as_destroy(&p->as);
        tlb_flush_all();
        p->used = false;
    }
}
