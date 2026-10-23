/* Host adapter: preserve the 32-bit CE ABI while translating test pointers.
 * Production runtime headers and code remain identical to the target build. */
#define _DEFAULT_SOURCE /* glibc: recursive mutexes and timeval under -std=c11 */
#define GLAZE_SYS_H
#define GLAZE_THREAD_H
#define GZ_THREAD_STACK (64u * 1024u)
#include "../apps/wince/runtime.h"
#include <assert.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
struct gz_mutex {
    int sem, owner, depth;
};
#define GZ_MUTEX_INIT                                                                                        \
    {                                                                                                        \
        0, 0, 0                                                                                              \
    }
struct gz_tcb {
    void *tls[2];
};
static _Thread_local struct gz_tcb host_tcb;
static struct gz_tcb *gz_self(void) { return &host_tcb; }
static CW host_word(const void *);
static void *host_ptr(CW);
#undef WORD
#undef PTR
#define WORD(p) host_word((const void *)(p))
#define PTR(t, w) ((t *)host_ptr(w))
static int gz_thread_id(void);
static int gz_getpid(void) { return 1; }
static void gz_sleep_ms(unsigned ms) { usleep(ms * 1000); }
static CW gz_time_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (CW)((uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
static int gz_mutex_init(struct gz_mutex *);
static void gz_mutex_destroy(struct gz_mutex *);
static void gz_mutex_lock(struct gz_mutex *);
static void gz_mutex_unlock(struct gz_mutex *);
static int gz_thread_create(void (*)(void *), void *);
static void gz_thread_exit(void) { pthread_exit(NULL); }
static void gz_exit(int code) { exit(code); }
