/* Process lifecycle: spawn from romfs, exit, and reaping. */

#include "kernel.h"
#include "hw.h"
#include "mips.h"
#include "proc.h"

static struct proc procs[MAX_PROCS];
_Static_assert(MAX_PROCS == GZ_TASK_MAX, "task snapshot must hold every process");
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

/* A managed app and nothing more: a privileged app may not be ended or
 * demoted by other apps, and goes back to PRIO_HIGH, not PRIO_NORMAL. */
static bool proc_is_app(const struct proc *p)
{
    return (p->flags & ~PROC_BACKGROUND) == PROC_APP;
}

int proc_snapshot(struct addrspace *as, uint32_t out, uint32_t size)
{
    struct gz_task_snapshot s;
    if (size != sizeof(s)) return GZ_EINVAL;
    memset(&s, 0, sizeof(s));
    s.time_us = time_us();
    s.busy_us = sched_account(s.time_us);
    s.total_pages = SDRAM_SIZE / PAGE_SIZE;
    s.free_pages = mm_free_pages();
    s.page_size = PAGE_SIZE;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &procs[i];
        if (!p->used || p->dying) continue;
        struct gz_task_info *t = &s.tasks[s.count++];
        t->pid = (uint32_t)p->pid;
        t->parent = (uint32_t)p->parent;
        t->flags = (proc_is_app(p) ? GZ_TASK_APP : 0) |
                   ((p->flags & PROC_BACKGROUND) ? GZ_TASK_BG : 0) |
                   (p->frozen ? GZ_TASK_SUSPENDED : 0) |
                   (p == cur->proc ? GZ_TASK_SELF : 0);
        t->cpu_us = p->cpu_us;
        strncpy(t->name, p->name, sizeof(t->name) - 1);
        sched_task_info(p, t);
        for (struct region_page *rp = p->as.regions; rp; rp = rp->next)
            for (unsigned j = 0; j < ARRAY_SIZE(rp->entry); j++) {
                struct region *r = &rp->entry[j];
                if (r->kind != REGION_OWNED && r->kind != REGION_STACK && r->kind != REGION_IMAGE &&
                    r->kind != REGION_SHM) continue;
                for (uint32_t n = 0; n < r->npages; n++)
                    t->mapped_pages += (as_lookup(&p->as, r->va + n * PAGE_SIZE) & PTE_V) != 0;
            }
    }
    return copy_to_as(as, out, &s, sizeof(s));
}

int proc_task_stop(int pid)
{
    struct proc *p = proc_by_pid(pid);
    if (!p) return GZ_ENOENT;
    if (p == cur->proc || !proc_is_app(p)) return GZ_EPERM;
    proc_exit(p, -9);
    return 0;
}

int proc_set_bg(int pid, int state)
{
    struct proc *p = proc_by_pid(pid);
    if (!p) return GZ_ENOENT;
    if (!proc_is_app(p)) return GZ_EPERM;
    if (state < GZ_PROC_FG || state > GZ_PROC_SUSPENDED) return GZ_EINVAL;
    if (state != GZ_PROC_FG)
        p->flags |= PROC_BACKGROUND;
    else
        p->flags &= ~PROC_BACKGROUND;
    sched_set_proc_prio(p, state != GZ_PROC_FG ? PRIO_LOW : PRIO_NORMAL);
    sched_freeze_proc(p, state == GZ_PROC_SUSPENDED);
    if (p == cur->proc && p->frozen)
        schedule();                     /* returns once thawed */
    return 0;
}

int proc_child_notify(int port)
{
    if (port && !ipc_port_owned(port, cur->proc)) return GZ_EPERM;
    cur->proc->child_port = port;
    return 0;
}

/* The program is romfs data (rom), or size bytes at src in src_as. */
static struct proc *spawn(const char *name, struct addrspace *src_as, const struct romfs_entry *rom, uint32_t src,
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
    if (as_stack_region(&p->as, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE))
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
    const struct romfs_entry *image = romfs_lookup(path);
    const char *base = path;

    if (!image)
        return NULL;
    for (const char *c = path; *c; c++) {
        if (*c == '/')
            base = c + 1;
    }
    return spawn(base, NULL, image, 0, image->size, parent, flags);
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
    struct proc *parent = proc_by_pid(p->parent);
    if (parent && parent->child_port && ipc_port_owned(parent->child_port, parent))
        ipc_notify(parent->child_port, GZ_NOTIFY_CHILD);
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
 * libc relies on to find its per-thread block. Pages are faulted in as the
 * stack grows. Nothing is ever unmapped, so a slot's pages are reused by the
 * next thread that gets it.
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
    if (as_stack_region(&p->as, base, USER_STACK_SIZE))
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
