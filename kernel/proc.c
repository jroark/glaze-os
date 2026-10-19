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

/* The program is romfs data (rom), or size bytes at src in src_as. */
static struct proc *spawn(const char *name, struct addrspace *src_as, const void *rom, uint32_t src,
                          uint32_t size, int parent, uint32_t flags)
{
    uint32_t entry;
    struct gz_ld_start start;
    struct proc *p = NULL;
    int slot;

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
    strncpy(p->name, name, sizeof(p->name) - 1);

    int load_error = elf_load(&p->as, src_as, rom, src, size, &entry, &start);
    if (!load_error && !rom && !start.abi)
        load_error = GZ_EINVAL;     /* installed apps run on the dynamic linker */
    if (load_error) { kprintf("proc: cannot load %s (%d)\n", name, load_error); goto fail; }
    if (as_alloc_region(&p->as, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE, REGION_OWNED))
        goto fail;

    uint32_t sp = USER_STACK_TOP - 16, boot = 0;
    if (start.abi) {
        start.pid = (uint32_t)p->pid;
        boot = USER_STACK_TOP - ALIGN_UP(sizeof(start), 16u);
        if (copy_to_as(&p->as, boot, &start, sizeof(start))) goto fail;
        sp = boot - 32;
    }
    int prio = (flags & PROC_PRIVILEGED) ? PRIO_HIGH : PRIO_NORMAL;
    p->main = thread_create_user(p, entry, sp, (uint32_t)p->pid, prio);
    if (!p->main)
        goto fail;
    p->main->tf->regs[R_A1] = boot;
    thread_make_runnable(p->main);
    kprintf("proc: spawned %s pid %d\n", p->name, p->pid);
    return p;

fail:
    as_destroy(&p->as);
    p->used = false;
    return NULL;
}

struct proc *proc_spawn(const char *path, int parent, uint32_t flags)
{
    uint32_t size;
    const void *image = romfs_find(path, &size);
    const char *base = path;

    if (!image)
        return NULL;
    for (const char *c = path; *c; c++) {
        if (*c == '/')
            base = c + 1;
    }
    return spawn(base, NULL, image, 0, size, parent, flags);
}

/* An installed app: the ELF is read by the spawner (SpringBoard) and copied
 * from its address space into shared image frames. */
struct proc *proc_spawn_image(struct addrspace *src_as, uint32_t src, uint32_t size, const char *name,
                              int parent, uint32_t flags)
{
    return spawn(name, src_as, NULL, src, size, parent, flags);
}

void proc_exit(struct proc *p, int code)
{
    if (p->dying)
        return;
    kprintf("proc: %s pid %d exited (%d)\n", p->name, p->pid, code);
    p->dying = true;
    p->exit_code = code;
    ipc_proc_cleanup(p);
    sem_proc_cleanup(p);
    sched_kill_proc_threads(p);
    proc_reap();
}

void proc_reap(void)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &procs[i];
        if (!p->used)
            continue;
        /* Exited threads of live processes are freed here too. */
        if (sched_reap_threads(p) || !p->dying)
            continue;
        as_destroy(&p->as);
        tlb_flush_all();
        p->used = false;
    }
}

/*
 * Extra threads. Each gets a 64 KiB stack in a slot below the main stack,
 * with a 64 KiB unmapped guard above it; slots are 64 KiB aligned, which
 * libc relies on to find its per-thread block. Nothing is ever unmapped,
 * so a slot's pages are reused by the next thread that gets it.
 */
static uint32_t stack_base(int slot)
{
    return USER_STACK_TOP - USER_STACK_SIZE - (uint32_t)(slot + 1) * 2u * USER_STACK_SIZE;
}

int thread_spawn(uint32_t entry, uint32_t arg)
{
    struct proc *p = cur->proc;
    int slot = 0;

    while (slot < MAX_STACK_SLOTS && (p->stack_slots & (1u << slot)))
        slot++;
    if (slot == MAX_STACK_SLOTS)
        return GZ_EFULL;
    uint32_t base = stack_base(slot);
    if (as_alloc_region(&p->as, base, USER_STACK_SIZE, REGION_OWNED))
        return GZ_ENOMEM;
    struct thread *t = thread_create_user(p, entry, base + USER_STACK_SIZE - 16, arg, cur->prio);
    if (!t)
        return GZ_ENOMEM;
    t->stack_slot = slot;
    p->stack_slots |= 1u << slot;
    thread_make_runnable(t);
    return t->tid;
}

/* The current thread exits; the last one takes its process with it. */
int thread_exit(void)
{
    struct proc *p = cur->proc;

    if (sched_live_threads(p) <= 1) {
        proc_exit(p, 0);
        return SYSRET_BLOCKED;
    }
    if (cur->stack_slot >= 0)
        p->stack_slots &= ~(1u << cur->stack_slot);
    thread_cancel_timeout(cur);
    thread_block(T_ZOMBIE);     /* reaped by proc_reap after the switch */
    return SYSRET_BLOCKED;
}
