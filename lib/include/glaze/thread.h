/*
 * Threads and locks (libc). Threads of a process share its address space
 * and run preemptively; each has a 64 KiB stack. There are no atomic
 * instructions on the VR4131, so every lock is a kernel semaphore.
 *
 * Until a process creates its first thread, locking is skipped, so
 * single-threaded programs pay nothing for malloc's lock.
 */
#ifndef GLAZE_THREAD_H
#define GLAZE_THREAD_H

#include <glaze/types.h>

#define GZ_THREAD_STACK     (64u * 1024u)

/* At the base of every thread's stack (stacks are 64 KiB aligned). */
struct gz_tcb {
    int tid;                    /* 0 until first asked for */
    int err;                    /* errno */
    void (*fn)(void *);
    void *arg;
    char dl_error[160];          /* dlerror: independent of compiler TLS */
    bool dl_pending;
    void *tls[2];               /* thread-local slots for libraries (lwIP) */
};

static inline struct gz_tcb *gz_self(void)
{
    return (struct gz_tcb *)((uintptr_t)__builtin_frame_address(0) & ~(uintptr_t)(GZ_THREAD_STACK - 1));
}

extern bool __gz_threaded;

/* Starts fn(arg) in a new thread; returns its tid or an error. */
int gz_thread_create(void (*fn)(void *), void *arg);
int gz_thread_id(void);

/* Recursive mutex. GZ_MUTEX_INIT works for statics: the semaphore is
 * made on first contended use. */
struct gz_mutex {
    int sem;
    int owner;
    int depth;
};
#define GZ_MUTEX_INIT   { 0, 0, 0 }

int gz_mutex_init(struct gz_mutex *m);
void gz_mutex_lock(struct gz_mutex *m);
void gz_mutex_unlock(struct gz_mutex *m);
void gz_mutex_destroy(struct gz_mutex *m);

#endif
