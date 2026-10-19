/* Threads, recursive mutexes and per-thread errno (see glaze/thread.h). */

#include <errno.h>
#include <glaze/sys.h>
#include <glaze/thread.h>
#include <stdlib.h>

bool __gz_threaded;
static int init_sem;            /* guards lazy mutex creation */

int *__gz_errno(void)
{
    return &gz_self()->err;
}

int gz_thread_id(void)
{
    struct gz_tcb *t = gz_self();

    if (!t->tid)
        t->tid = gz_gettid();
    return t->tid;
}

struct start {
    void (*fn)(void *);
    void *arg;
};

static void thread_start(void *p)
{
    struct start st = *(struct start *)p;
    struct gz_tcb *t = gz_self();

    free(p);
    t->tid = gz_gettid();
    t->err = 0;
    t->fn = st.fn;
    t->arg = st.arg;
    t->tls[0] = t->tls[1] = NULL;   /* the stack slot may be reused */
    st.fn(st.arg);
    gz_thread_exit();
}

int gz_thread_create(void (*fn)(void *), void *arg)
{
    if (!__gz_threaded) {
        /* Still single-threaded: nothing can race this. */
        if (!init_sem && (init_sem = gz_sem_create(1)) < 0) {
            init_sem = 0;
            return GZ_ENOMEM;
        }
        gz_thread_id();
        __gz_threaded = true;
    }
    struct start *st = malloc(sizeof(*st));
    if (!st)
        return GZ_ENOMEM;
    st->fn = fn;
    st->arg = arg;
    int tid = gz_thread_spawn(thread_start, st);
    if (tid < 0)
        free(st);
    return tid;
}

int gz_mutex_init(struct gz_mutex *m)
{
    m->owner = m->depth = 0;
    m->sem = gz_sem_create(1);
    return m->sem < 0 ? m->sem : 0;
}

void gz_mutex_lock(struct gz_mutex *m)
{
    if (!__gz_threaded)
        return;
    int self = gz_thread_id();
    if (m->owner == self) {
        m->depth++;
        return;
    }
    if (m->sem <= 0) {
        gz_sem_wait(init_sem, -1);
        if (m->sem <= 0)
            m->sem = gz_sem_create(1);
        gz_sem_post(init_sem);
    }
    gz_sem_wait(m->sem, -1);
    m->owner = self;
    m->depth = 1;
}

void gz_mutex_unlock(struct gz_mutex *m)
{
    /* Locks skipped while single-threaded have no owner. */
    if (!__gz_threaded || m->owner != gz_thread_id())
        return;
    if (--m->depth == 0) {
        m->owner = 0;
        gz_sem_post(m->sem);
    }
}

void gz_mutex_destroy(struct gz_mutex *m)
{
    if (m->sem > 0)
        gz_sem_destroy(m->sem);
    m->sem = 0;
}
