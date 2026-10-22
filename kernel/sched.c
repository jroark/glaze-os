/*
 * Threads and the scheduler.
 *
 * The kernel is non-preemptive: it runs with interrupts disabled, and only
 * the idle thread runs kernel code with interrupts enabled. User threads
 * are preempted from the timer tick. Scheduling is strict priority with
 * round-robin inside each level.
 */

#include "kernel.h"
#include "hw.h"
#include "mips.h"
#include "proc.h"

struct thread *cur;
uint32_t cur_kstack_top;
bool need_resched;

static struct thread threads[MAX_THREADS];
static int next_tid_gen = 1;
static struct thread *runq_head[NPRIO], *runq_tail[NPRIO];
static struct thread *idle_thread;
static struct context sched_boot_ctx;
static uint64_t account_since, busy_us;

/* Also called when sampling, so the currently running slice is included.
 * Kernel execution is serialized with interrupts disabled. */
uint64_t sched_account(uint64_t now)
{
    uint64_t elapsed = now - account_since;
    if (cur && cur != idle_thread) {
        busy_us += elapsed;
        if (cur->proc) cur->proc->cpu_us += elapsed;
    }
    account_since = now;
    return busy_us;
}

void sched_task_info(struct proc *p, struct gz_task_info *info)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        struct thread *t = &threads[i];
        if (t->proc != p || t->state == T_UNUSED || t->state == T_ZOMBIE) continue;
        info->threads++;
        info->runnable += t->state == T_RUNNING || t->state == T_RUNNABLE;
    }
}

/* Threads waiting with a timeout, unsorted (the table is small). */
static struct thread *timed[MAX_THREADS];

static struct thread *thread_alloc(void)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        struct thread *t = &threads[i];
        if (t->state != T_UNUSED)
            continue;
        uint8_t *kstack = page_alloc_n(KSTACK_SIZE / PAGE_SIZE);
        if (!kstack)
            return NULL;
        memset(t, 0, sizeof(*t));
        t->kstack = kstack;
        /* tid encodes the slot plus a generation so stale tokens fail. */
        t->tid = (next_tid_gen++ << 7) | i;
        t->stack_slot = -1;
        if (next_tid_gen > 0xFFFFF)
            next_tid_gen = 1;
        return t;
    }
    return NULL;
}

void thread_free(struct thread *t)
{
    thread_cancel_timeout(t);
    page_free(t->kstack);
    page_free(t->kstack + PAGE_SIZE);
    t->state = T_UNUSED;
}

struct thread *thread_by_tid(int tid)
{
    if (tid <= 0)
        return NULL;
    struct thread *t = &threads[tid & (MAX_THREADS - 1)];
    return (t->tid == tid && t->state != T_UNUSED) ? t : NULL;
}

static void runq_push(struct thread *t)
{
    if (t->proc && t->proc->frozen)
        return;                         /* runnable, but queued only when thawed */
    t->next = NULL;
    if (runq_tail[t->prio])
        runq_tail[t->prio]->next = t;
    else
        runq_head[t->prio] = t;
    runq_tail[t->prio] = t;
}

static struct thread *runq_pop(void)
{
    for (int p = 0; p < NPRIO; p++) {
        struct thread *t = runq_head[p];
        if (t) {
            runq_head[p] = t->next;
            if (!runq_head[p])
                runq_tail[p] = NULL;
            t->next = NULL;
            return t;
        }
    }
    return NULL;
}

/* Build a thread whose first run eret's into `tf` (prepared by caller). */
static struct thread *thread_setup(struct proc *p, int prio, uint32_t status, uint32_t pc, uint32_t sp)
{
    struct thread *t = thread_alloc();

    if (!t)
        return NULL;
    t->proc = p;
    t->prio = prio;
    t->tf = (struct trapframe *)(t->kstack + KSTACK_SIZE - sizeof(struct trapframe));
    memset(t->tf, 0, sizeof(*t->tf));
    t->tf->epc = pc;
    t->tf->status = status;
    t->tf->regs[R_SP] = sp;
    t->ctx.sp = (uint32_t)t->tf;
    t->ctx.ra = (uint32_t)trap_return;
    return t;
}

struct thread *thread_create_user(struct proc *p, uint32_t entry, uint32_t sp, uint32_t arg, int prio)
{
    struct thread *t = thread_setup(p, prio, ST_IM2 | ST_IM7 | ST_KSU_USER | ST_EXL | ST_IE, entry, sp);

    if (t) {
        t->tf->regs[R_A0] = arg;
        t->tf->regs[25] = entry; /* MIPS O32 PIC prologues derive gp from t9. */
    }
    return t;
}

static void idle_loop(void)
{
    for (;;) {
        /* Interrupts are enabled here (see thread_setup in sched_init). */
        if (need_resched) {
            uint32_t s = irq_save();
            schedule();
            irq_restore(s);
        }
    }
}

void thread_make_runnable(struct thread *t)
{
    thread_cancel_timeout(t);
    t->state = T_RUNNABLE;
    if (t != idle_thread)
        runq_push(t);
    if (cur == idle_thread || (cur && t->prio < cur->prio))
        need_resched = true;
}

void thread_cancel_timeout(struct thread *t)
{
    if (!t->timed)
        return;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (timed[i] == t) {
            timed[i] = NULL;
            break;
        }
    }
    t->timed = false;
    t->wake_us = 0;
}

static void set_timeout(struct thread *t, uint64_t wake_us)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!timed[i]) {
            timed[i] = t;
            t->timed = true;
            t->wake_us = wake_us;
            return;
        }
    }
    panic("timeout table full");
}

/* Block the current thread in `st` and run something else. */
void thread_block(enum tstate st)
{
    cur->state = st;
    schedule();
}

void thread_sleep_until(uint64_t wake_us)
{
    set_timeout(cur, wake_us);
    thread_block(T_SLEEP);
}

/* Block with a timeout; the timeout handler runs from the tick. */
void thread_block_timeout(enum tstate st, uint64_t wake_us)
{
    set_timeout(cur, wake_us);
    thread_block(st);
}

void sched_timer_tick(uint64_t now_us)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        struct thread *t = timed[i];
        if (!t || t->wake_us > now_us)
            continue;
        timed[i] = NULL;
        t->timed = false;
        if (t->state == T_SLEEP) {
            t->tf->regs[R_V0] = 0;
            thread_make_runnable(t);
        } else if (t->state == T_RECV) {
            ipc_timeout(t);
        } else if (t->state == T_SEM) {
            sem_timeout(t);
        }
    }
    /* Round-robin among equal-priority user threads. */
    if (cur != idle_thread)
        need_resched = true;
}

/* Pick the next thread and switch to it. Called with interrupts off. */
void schedule(void)
{
    struct thread *prev = cur;
    struct thread *next;

    sched_account(time_us());
    need_resched = false;
    if (prev && prev->state == T_RUNNING) {
        prev->state = T_RUNNABLE;
        if (prev != idle_thread)
            runq_push(prev);
    }
    next = runq_pop();
    if (!next)
        next = idle_thread;
    next->state = T_RUNNING;
    if (next == prev)
        return;

    cur = next;
    cur_kstack_top = (uint32_t)next->kstack + KSTACK_SIZE;
    if (next->proc)
        as_activate(&next->proc->as);
    swtch(prev ? &prev->ctx : &sched_boot_ctx, &next->ctx);
    /* Back in `prev`: reap any zombies other threads left behind. */
    proc_reap();
}

void sched_init(void)
{
    /* The idle thread is a kernel thread that runs with IE set. */
    idle_thread = thread_setup(NULL, PRIO_IDLE, ST_IM2 | ST_IM7 | ST_EXL | ST_IE,
                               (uint32_t)idle_loop, 0);
    KASSERT(idle_thread);
    idle_thread->tf->regs[R_SP] = (uint32_t)idle_thread->tf - 64;
    idle_thread->state = T_RUNNABLE;
}

void sched_start(void)
{
    cur = NULL;
    schedule();
    panic("sched_start returned");
}

static void runq_remove(struct thread *t)
{
    struct thread **link = &runq_head[t->prio];
    struct thread *prev = NULL;

    while (*link) {
        if (*link == t) {
            *link = t->next;
            if (runq_tail[t->prio] == t)
                runq_tail[t->prio] = prev;
            t->next = NULL;
            return;
        }
        prev = *link;
        link = &(*link)->next;
    }
}

void ipc_fail_reply_waiters(struct proc *dead, struct thread *t);

void sched_set_proc_prio(struct proc *p, int prio)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        struct thread *t = &threads[i];
        if (t->state == T_UNUSED || t->state == T_ZOMBIE || t->proc != p)
            continue;
        if (t->prio == prio)
            continue;
        if (t->state == T_RUNNABLE) {
            runq_remove(t);
            t->prio = prio;
            runq_push(t);
        } else {
            t->prio = prio;
        }
    }
    need_resched = true;
}

/* A frozen process's runnable threads are off the run queue; wake-ups
 * leave them runnable there, and thawing queues them all. */
void sched_freeze_proc(struct proc *p, bool frozen)
{
    if (p->frozen == frozen)
        return;
    if (frozen) {
        for (int i = 0; i < MAX_THREADS; i++)
            if (threads[i].proc == p && threads[i].state == T_RUNNABLE)
                runq_remove(&threads[i]);
        p->frozen = true;
    } else {
        p->frozen = false;
        for (int i = 0; i < MAX_THREADS; i++)
            if (threads[i].proc == p && threads[i].state == T_RUNNABLE)
                runq_push(&threads[i]);
    }
    need_resched = true;
}

/* Make every thread of `p` a zombie and fail anyone waiting on `p`. */
void sched_kill_proc_threads(struct proc *p)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        struct thread *t = &threads[i];
        if (t->state == T_UNUSED || t->state == T_ZOMBIE)
            continue;
        if (t->proc == p) {
            if (t->state == T_RUNNABLE)
                runq_remove(t);
            thread_cancel_timeout(t);
            t->state = T_ZOMBIE;
        } else {
            ipc_fail_reply_waiters(p, t);
        }
    }
}

/* Threads of `p` that haven't exited. */
int sched_live_threads(struct proc *p)
{
    int live = 0;

    for (int i = 0; i < MAX_THREADS; i++) {
        struct thread *t = &threads[i];
        live += t->state != T_UNUSED && t->state != T_ZOMBIE && t->proc == p;
    }
    return live;
}

/* Free zombie threads other than the running one. Returns how many of
 * `p`'s threads are still allocated. */
int sched_reap_threads(struct proc *p)
{
    int live = 0;

    for (int i = 0; i < MAX_THREADS; i++) {
        struct thread *t = &threads[i];
        if (t->state == T_UNUSED || t->proc != p)
            continue;
        if (t->state == T_ZOMBIE && t != cur)
            thread_free(t);
        else
            live++;
    }
    return live;
}
