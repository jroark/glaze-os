/*
 * Counting semaphores for the threads of one process.
 *
 * The VR4131 has no LL/SC, so userspace can't build atomic locks; libc's
 * mutexes and lwIP's sys_arch port use these instead. The kernel runs
 * with interrupts disabled, which makes each operation atomic. Waiters
 * queue FIFO and may time out.
 */

#include "kernel.h"
#include "mips.h"
#include "proc.h"

struct ksem {
    bool used;
    struct proc *owner;
    int count;
    struct thread *head, *tail;     /* waiters, linked through t->next */
};

static struct ksem sems[MAX_SEMS];

static struct ksem *sem_get(int id)
{
    if (id < 1 || id > MAX_SEMS || !sems[id - 1].used || sems[id - 1].owner != cur->proc)
        return NULL;
    return &sems[id - 1];
}

int sem_create(struct proc *p, int count)
{
    if (count < 0)
        return GZ_EINVAL;
    for (int i = 0; i < MAX_SEMS; i++) {
        if (!sems[i].used) {
            memset(&sems[i], 0, sizeof(sems[i]));
            sems[i].used = true;
            sems[i].owner = p;
            sems[i].count = count;
            return i + 1;
        }
    }
    return GZ_EFULL;
}

int sem_wait(int id, int timeout_ms)
{
    struct ksem *s = sem_get(id);

    if (!s)
        return GZ_EINVAL;
    if (s->count > 0) {
        s->count--;
        return 0;
    }
    if (timeout_ms == 0)
        return GZ_ETIMEDOUT;
    cur->next = NULL;
    cur->sem_id = id;
    if (s->tail)
        s->tail->next = cur;
    else
        s->head = cur;
    s->tail = cur;
    if (timeout_ms > 0)
        thread_block_timeout(T_SEM, time_us() + (uint64_t)timeout_ms * 1000u);
    else
        thread_block(T_SEM);
    return SYSRET_BLOCKED;
}

static struct thread *pop(struct ksem *s)
{
    struct thread *t = s->head;

    if (t) {
        s->head = t->next;
        if (!s->head)
            s->tail = NULL;
        t->next = NULL;
    }
    return t;
}

int sem_post(int id)
{
    struct ksem *s = sem_get(id);

    if (!s)
        return GZ_EINVAL;
    struct thread *t = pop(s);
    if (t) {
        t->tf->regs[R_V0] = 0;
        thread_make_runnable(t);
    } else if (s->count < 0x7FFFFFFF) {
        s->count++;
    }
    return 0;
}

int sem_destroy(int id)
{
    struct ksem *s = sem_get(id);
    struct thread *t;

    if (!s)
        return GZ_EINVAL;
    while ((t = pop(s))) {
        t->tf->regs[R_V0] = (uint32_t)GZ_EDEAD;
        thread_make_runnable(t);
    }
    s->used = false;
    return 0;
}

/* From the timer tick: t's wait ran out. */
void sem_timeout(struct thread *t)
{
    int id = t->sem_id;

    if (id >= 1 && id <= MAX_SEMS) {
        struct ksem *s = &sems[id - 1];
        struct thread **link = &s->head, *prev = NULL;
        while (*link && *link != t) {
            prev = *link;
            link = &(*link)->next;
        }
        if (*link) {
            *link = t->next;
            if (s->tail == t)
                s->tail = prev;
            t->next = NULL;
        }
    }
    t->tf->regs[R_V0] = (uint32_t)GZ_ETIMEDOUT;
    thread_make_runnable(t);
}

/* An exiting process's semaphores go; its waiters are being killed. */
void sem_proc_cleanup(struct proc *p)
{
    for (int i = 0; i < MAX_SEMS; i++)
        if (sems[i].used && sems[i].owner == p)
            memset(&sems[i], 0, sizeof(sems[i]));
}
