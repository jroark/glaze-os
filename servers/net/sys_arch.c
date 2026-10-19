/*
 * lwIP OS port (see arch/sys_arch.h).
 *
 * lwIP's timeouts are milliseconds with 0 meaning "forever"; the kernel's
 * are milliseconds with 0 meaning "don't wait" and -1 forever. Mailboxes
 * are rings guarded by a binary semaphore, with counting semaphores for
 * queued messages and free slots. SYS_ARCH_PROTECT is one recursive lock.
 */

#include <glaze/sys.h>
#include <glaze/thread.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/opt.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "server.h"

struct sys_mbox {
    int lock, items, space;
    int size, head, count;
    void *msgs[];
};

static struct gz_mutex protect_lock;

static int kernel_timeout(u32_t timeout)
{
    return timeout ? (int)timeout : -1;
}

void sys_init(void)
{
    gz_mutex_init(&protect_lock);
}

u32_t sys_now(void)
{
    return gz_time_ms();
}

sys_prot_t sys_arch_protect(void)
{
    gz_mutex_lock(&protect_lock);
    return 0;
}

void sys_arch_unprotect(sys_prot_t pval)
{
    (void)pval;
    gz_mutex_unlock(&protect_lock);
}

/* ---- semaphores and mutexes ------------------------------------------------ */

err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
    sem->id = gz_sem_create(count);
    if (sem->id <= 0) {
        sem->id = 0;
        return ERR_MEM;
    }
    return ERR_OK;
}

void sys_sem_signal(sys_sem_t *sem)
{
    gz_sem_post(sem->id);
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
    u32_t start = gz_time_ms();

    if (gz_sem_wait(sem->id, kernel_timeout(timeout)) == GZ_ETIMEDOUT)
        return SYS_ARCH_TIMEOUT;
    u32_t waited = gz_time_ms() - start;
    return waited == SYS_ARCH_TIMEOUT ? waited - 1 : waited;
}

void sys_sem_free(sys_sem_t *sem)
{
    if (sem->id > 0)
        gz_sem_destroy(sem->id);
    sem->id = 0;
}

err_t sys_mutex_new(sys_mutex_t *mutex)
{
    mutex->id = gz_sem_create(1);
    if (mutex->id <= 0) {
        mutex->id = 0;
        return ERR_MEM;
    }
    return ERR_OK;
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
    gz_sem_wait(mutex->id, -1);
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
    gz_sem_post(mutex->id);
}

void sys_mutex_free(sys_mutex_t *mutex)
{
    if (mutex->id > 0)
        gz_sem_destroy(mutex->id);
    mutex->id = 0;
}

/* ---- mailboxes ---------------------------------------------------------------- */

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    if (size <= 0)
        size = 16;
    struct sys_mbox *mb = malloc(sizeof(*mb) + sizeof(void *) * (size_t)size);
    if (!mb)
        return ERR_MEM;
    memset(mb, 0, sizeof(*mb));
    mb->size = size;
    mb->lock = gz_sem_create(1);
    mb->items = gz_sem_create(0);
    mb->space = gz_sem_create(size);
    if (mb->lock <= 0 || mb->items <= 0 || mb->space <= 0) {
        if (mb->lock > 0)
            gz_sem_destroy(mb->lock);
        if (mb->items > 0)
            gz_sem_destroy(mb->items);
        if (mb->space > 0)
            gz_sem_destroy(mb->space);
        free(mb);
        return ERR_MEM;
    }
    mbox->mb = mb;
    return ERR_OK;
}

static void put(struct sys_mbox *mb, void *msg)
{
    gz_sem_wait(mb->lock, -1);
    mb->msgs[(mb->head + mb->count) % mb->size] = msg;
    mb->count++;
    gz_sem_post(mb->lock);
    gz_sem_post(mb->items);
}

static void *take(struct sys_mbox *mb)
{
    gz_sem_wait(mb->lock, -1);
    void *msg = mb->msgs[mb->head];
    mb->head = (mb->head + 1) % mb->size;
    mb->count--;
    gz_sem_post(mb->lock);
    gz_sem_post(mb->space);
    return msg;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
    gz_sem_wait(mbox->mb->space, -1);
    put(mbox->mb, msg);
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
    if (gz_sem_wait(mbox->mb->space, 0) != 0)
        return ERR_MEM;
    put(mbox->mb, msg);
    return ERR_OK;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
    return sys_mbox_trypost(mbox, msg);
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
    u32_t start = gz_time_ms();

    if (gz_sem_wait(mbox->mb->items, kernel_timeout(timeout)) != 0) {
        if (msg)
            *msg = NULL;
        return SYS_ARCH_TIMEOUT;
    }
    void *m = take(mbox->mb);
    if (msg)
        *msg = m;
    u32_t waited = gz_time_ms() - start;
    return waited == SYS_ARCH_TIMEOUT ? waited - 1 : waited;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
    if (gz_sem_wait(mbox->mb->items, 0) != 0)
        return SYS_MBOX_EMPTY;
    void *m = take(mbox->mb);
    if (msg)
        *msg = m;
    return 0;
}

void sys_mbox_free(sys_mbox_t *mbox)
{
    struct sys_mbox *mb = mbox->mb;

    if (!mb)
        return;
    if (mb->count)
        net_log("net: freeing a mailbox with %d messages\n", mb->count);
    gz_sem_destroy(mb->lock);
    gz_sem_destroy(mb->items);
    gz_sem_destroy(mb->space);
    free(mb);
    mbox->mb = NULL;
}

/* ---- threads -------------------------------------------------------------------- */

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread, void *arg, int stacksize, int prio)
{
    (void)stacksize;            /* every thread gets GZ_THREAD_STACK */
    (void)prio;
    int tid = gz_thread_create(thread, arg);
    if (tid < 0)
        net_log("net: can't start thread %s (%d)\n", name, tid);
    return tid;
}

sys_sem_t *sys_arch_netconn_sem_get(void)
{
    struct gz_tcb *t = gz_self();

    if (!t->tls[0]) {
        sys_sem_t *sem = malloc(sizeof(*sem));
        if (!sem || sys_sem_new(sem, 0) != ERR_OK) {
            free(sem);
            return NULL;
        }
        t->tls[0] = sem;
    }
    return t->tls[0];
}

void sys_arch_netconn_sem_free(void)
{
    struct gz_tcb *t = gz_self();

    if (t->tls[0]) {
        sys_sem_free(t->tls[0]);
        free(t->tls[0]);
        t->tls[0] = NULL;
    }
}
