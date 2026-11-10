/* Process-private CE synchronization and explicit TLS, backed by Glaze threads.
 * The VR4131 has no LL/SC; interlocked operations must use a kernel mutex. */
#include "runtime.h"
#include <glaze/thread.h>
_Static_assert(GZ_THREAD_STACK == 65536u, "CE inline thread IDs require 64 KiB stacks");
#define TLS_SLOTS 64
#define SYNC_OBJECTS 64
struct ce_thread_local {
    CW error, values[TLS_SLOTS], generations[TLS_SLOTS], object;
    CW crt_word;
    void *tokens[2];
};
static struct gz_mutex lock = GZ_MUTEX_INIT;
static uint64_t tls_used;
static CW tls_generation[TLS_SLOTS];
static CW serial;
enum { SYNC_MUTEX = 1, SYNC_EVENT, SYNC_THREAD, SYNC_SEMAPHORE };
static struct sync_object {
    CW handle, kind, refs, owner, depth, code, proc, arg, tid;
    int32_t count, maximum; /* semaphores */
    bool signaled, manual, suspended, open;
    WCHAR name[128];
} objects[SYNC_OBJECTS];
static struct ce_thread_local *local(void)
{
    if (!gz_self()->tls[1]) {
        gz_self()->tls[1] = calloc(1, sizeof(struct ce_thread_local));
        if (!gz_self()->tls[1])
            ce_fatal("Out of memory creating WinCE thread state");
    }
    return gz_self()->tls[1];
}
CW *ce_error_slot(void) { return &local()->error; }
void **ce_token_context(unsigned wide) { return &local()->tokens[wide != 0]; }
static struct {
    CW handle;
    struct sync_object *object;
} handles[128];
static struct sync_object *lookup(CW h)
{
    for (unsigned i = 0; i < ARRAY_SIZE(handles); i++)
        if (handles[i].handle == h && h)
            return handles[i].object;
    return NULL;
}
static CW new_handle(struct sync_object *o)
{
    for (unsigned i = 0; i < ARRAY_SIZE(handles); i++)
        if (!handles[i].handle) {
            CW h = 0x7a000000u + (++serial & 0xffffffu);
            handles[i].handle = h;
            handles[i].object = o;
            o->refs++;
            return h;
        }
    return 0;
}
static void release(struct sync_object *o)
{
    if (!--o->refs)
        memset(o, 0, sizeof(*o));
}
static CW create(CW kind, CW name, bool initial, bool manual)
{
    const WCHAR *text = PTR(WCHAR, name);
    if (ce_wlen(text) >= 128) {
        ce_error = 87;
        return 0;
    }
    gz_mutex_lock(&lock);
    if (text && *text)
        for (unsigned i = 0; i < SYNC_OBJECTS; i++) {
            struct sync_object *o = &objects[i];
            if (o->kind && !ce_wcmp(o->name, text)) {
                if (o->kind != kind) {
                    gz_mutex_unlock(&lock);
                    ce_error = 6;
                    return 0;
                }
                CW h = new_handle(o);
                gz_mutex_unlock(&lock);
                ce_error = h ? 183 : 8;
                return h;
            }
        }
    for (unsigned i = 0; i < SYNC_OBJECTS; i++)
        if (!objects[i].kind) {
            struct sync_object *o = &objects[i];
            o->handle = new_handle(o);
            if (!o->handle) {
                gz_mutex_unlock(&lock);
                ce_error = 8;
                return 0;
            }
            o->kind = kind;

            o->open = true;
            o->manual = manual;
            o->signaled = kind == SYNC_EVENT && initial;
            if (kind == SYNC_MUTEX && initial) {
                o->owner = gz_thread_id();
                o->depth = 1;
            }
            if (text)
                memcpy(o->name, text, (ce_wlen(text) + 1) * 2);
            CW h = o->handle;
            gz_mutex_unlock(&lock);
            ce_error = 0;
            return h;
        }
    gz_mutex_unlock(&lock);
    ce_error = 8;
    return 0;
}
CE_API(CreateMutexW) { return create(SYNC_MUTEX, a2, a1 != 0, false); }
CE_API(CreateEventW) { return create(SYNC_EVENT, a3, a2 != 0, a1 != 0); }
CE_API(ReleaseMutex)
{
    gz_mutex_lock(&lock);
    struct sync_object *o = lookup(a0);
    bool ok = o && o->open && o->kind == SYNC_MUTEX && o->owner == (CW)gz_thread_id() && o->depth;
    if (ok && !--o->depth)
        o->owner = 0;
    gz_mutex_unlock(&lock);
    if (!ok)
        ce_error = 288;
    return ok;
}
CE_API(EventModify)
{
    gz_mutex_lock(&lock);
    struct sync_object *o = lookup(a0);
    bool ok = o && o->open && o->kind == SYNC_EVENT && (a1 == 2 || a1 == 3);
    if (ok)
        o->signaled = a1 == 3;
    gz_mutex_unlock(&lock);
    if (!ok)
        ce_error = 87;
    return ok;
}
CE_API(SetEvent) { return ce_EventModify(a0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
CE_API(ResetEvent) { return ce_EventModify(a0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
/* With `lock` held: whether a wait on `o` would succeed now, and taking it
 * (a mutex's ownership, an auto-reset event, a semaphore count) if `take`. */
static bool ready(struct sync_object *o, CW tid, bool take)
{
    bool now = o->kind == SYNC_MUTEX ? (!o->owner || o->owner == tid)
             : o->kind == SYNC_SEMAPHORE ? o->count > 0 : o->signaled;
    if (now && take) {
        if (o->kind == SYNC_MUTEX) {
            o->owner = tid;
            o->depth++;
        }
        if (o->kind == SYNC_EVENT && !o->manual)
            o->signaled = false;
        if (o->kind == SYNC_SEMAPHORE)
            o->count--;
    }
    return now;
}
/* WaitForMultipleObjects(count, handles, wait_all, timeout): WAIT_OBJECT_0 +
 * the first ready index (or 0 once all are), WAIT_TIMEOUT 258, or -1. */
CE_API(WaitForMultipleObjects)
{
    CW start = gz_time_ms(), tid = gz_thread_id();
    if (!a0 || a0 > 64 || !a1) {
        ce_error = 87;
        return (CW)-1;
    }
    struct sync_object *set[64];
    gz_mutex_lock(&lock);
    for (unsigned i = 0; i < a0; i++) {
        set[i] = lookup(PTR(CW, a1)[i]);
        if (!set[i] || !set[i]->open) {
            gz_mutex_unlock(&lock);
            ce_error = 6;
            return (CW)-1;
        }
    }
    for (unsigned i = 0; i < a0; i++)
        set[i]->refs++;
    CW result = 258;
    for (;;) {
        if (a2) {
            bool all = true;
            for (unsigned i = 0; i < a0 && all; i++)
                all = ready(set[i], tid, false);
            if (all) {
                for (unsigned i = 0; i < a0; i++)
                    ready(set[i], tid, true);
                result = 0;
                break;
            }
        } else {
            unsigned i = 0;
            while (i < a0 && !ready(set[i], tid, true))
                i++;
            if (i < a0) {
                result = i;
                break;
            }
        }
        if (a3 != 0xffffffffu && gz_time_ms() - start >= a3)
            break;
        gz_mutex_unlock(&lock);
        gz_sleep_ms(1);
        gz_mutex_lock(&lock);
    }
    for (unsigned i = 0; i < a0; i++)
        release(set[i]);
    gz_mutex_unlock(&lock);
    return result;
}
CE_API(WaitForSingleObject)
{
    CW handle = a0;
    return ce_WaitForMultipleObjects(1, WORD(&handle), 0, a1, 0, 0, 0, 0, 0, 0, 0, 0);
}
CE_API(CreateSemaphoreW)
{
    if ((int32_t)a2 <= 0 || (int32_t)a1 < 0 || (int32_t)a1 > (int32_t)a2) {
        ce_error = 87;
        return 0;
    }
    CW h = create(SYNC_SEMAPHORE, a3, false, false);
    if (h && ce_error != 183) {
        gz_mutex_lock(&lock);
        struct sync_object *o = lookup(h);
        o->count = (int32_t)a1;
        o->maximum = (int32_t)a2;
        gz_mutex_unlock(&lock);
    }
    return h;
}
CE_API(ReleaseSemaphore)
{
    gz_mutex_lock(&lock);
    struct sync_object *o = lookup(a0);
    bool ok = o && o->open && o->kind == SYNC_SEMAPHORE && (int32_t)a1 > 0 && (int32_t)a1 <= o->maximum - o->count;
    if (ok) {
        if (a2)
            *PTR(CW, a2) = (CW)o->count;
        o->count += (int32_t)a1;
    }
    gz_mutex_unlock(&lock);
    if (!ok)
        ce_error = o ? 298 : 6; /* ERROR_TOO_MANY_POSTS */
    return ok;
}
bool ce_sync_close(CW h, bool *handled)
{
    *handled = (h & 0xff000000u) == 0x7a000000u;
    gz_mutex_lock(&lock);
    for (unsigned i = 0; i < ARRAY_SIZE(handles); i++)
        if (h && handles[i].handle == h) {
            struct sync_object *o = handles[i].object;
            handles[i].handle = 0;
            handles[i].object = NULL;
            release(o);
            gz_mutex_unlock(&lock);
            return true;
        }
    gz_mutex_unlock(&lock);
    if (*handled)
        ce_error = 6;
    return false;
}
CE_API(InitializeCriticalSection)
{
    if (!a0) {
        ce_error = 87;
        return 0;
    }
    /* CE CRITICAL_SECTION is five 32-bit words, unlike desktop Win32. */
    memset(PTR(void, a0), 0, 20);
    if (gz_mutex_init(PTR(struct gz_mutex, a0)) < 0)
        ce_fatal("Cannot allocate WinCE critical section");
    return 0;
}
CE_API(DeleteCriticalSection)
{
    if (a0)
        gz_mutex_destroy(PTR(struct gz_mutex, a0));
    return 0;
}
CE_API(EnterCriticalSection)
{
    if (a0)
        gz_mutex_lock(PTR(struct gz_mutex, a0));
    return 0;
}
CE_API(LeaveCriticalSection)
{
    if (a0)
        gz_mutex_unlock(PTR(struct gz_mutex, a0));
    return 0;
}
CE_API(InterlockedIncrement)
{
    gz_mutex_lock(&lock);
    CW v = ++*PTR(CW, a0);
    gz_mutex_unlock(&lock);
    return v;
}
CE_API(InterlockedDecrement)
{
    gz_mutex_lock(&lock);
    CW v = --*PTR(CW, a0);
    gz_mutex_unlock(&lock);
    return v;
}
CE_API(InterlockedExchange)
{
    gz_mutex_lock(&lock);
    CW old = *PTR(CW, a0);
    *PTR(CW, a0) = a1;
    gz_mutex_unlock(&lock);
    return old;
}
CE_API(InterlockedCompareExchange)
{
    gz_mutex_lock(&lock);
    CW old = *PTR(CW, a0);
    if (old == a2)
        *PTR(CW, a0) = a1;
    gz_mutex_unlock(&lock);
    return old;
}
CE_API(InterlockedExchangeAdd)
{
    gz_mutex_lock(&lock);
    CW old = *PTR(CW, a0);
    *PTR(CW, a0) = old + a1;
    gz_mutex_unlock(&lock);
    return old;
}
CE_API(TlsAlloc)
{
    gz_mutex_lock(&lock);
    for (unsigned i = 0; i < TLS_SLOTS; i++)
        if (!(tls_used & ((uint64_t)1 << i))) {
            tls_used |= (uint64_t)1 << i;
            tls_generation[i]++;
            gz_mutex_unlock(&lock);
            return i;
        }
    gz_mutex_unlock(&lock);
    ce_error = 8;
    return (CW)-1;
}
CE_API(TlsFree)
{
    gz_mutex_lock(&lock);
    bool ok = a0 < TLS_SLOTS && (tls_used & ((uint64_t)1 << a0));
    if (ok)
        tls_used &= ~((uint64_t)1 << a0);
    gz_mutex_unlock(&lock);
    if (!ok)
        ce_error = 87;
    return ok;
}
CE_API(TlsGetValue)
{
    struct ce_thread_local *t = local();
    /* CE's supplied thunks use unchecked 32-bit pointer arithmetic. Some
     * statically linked CRTs leave __tlsindex at -1 and use TLS[-1]. Model
     * that extra word explicitly rather than exposing the native TCB. */
    if (a0 == UINT32_MAX) {
        ce_error = 0;
        return t->crt_word;
    }
    gz_mutex_lock(&lock);
    bool ok = a0 < TLS_SLOTS && (tls_used & ((uint64_t)1 << a0));
    CW value = ok && t->generations[a0] == tls_generation[a0] ? t->values[a0] : 0;
    gz_mutex_unlock(&lock);
    ce_error = ok ? 0 : 87;
    return value;
}
CE_API(TlsSetValue)
{
    struct ce_thread_local *t = local();
    if (a0 == UINT32_MAX) {
        t->crt_word = a1;
        return 1;
    }
    gz_mutex_lock(&lock);
    bool ok = a0 < TLS_SLOTS && (tls_used & ((uint64_t)1 << a0));
    if (ok) {
        t->values[a0] = a1;
        t->generations[a0] = tls_generation[a0];
    }
    gz_mutex_unlock(&lock);
    if (!ok)
        ce_error = 87;
    return ok;
}
CE_API(TlsCall)
{
    if (a0 == 0)
        return ce_TlsAlloc(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (a0 == 1)
        return ce_TlsFree(a1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    ce_error = 87;
    return 0;
}
void ce_thread_exit(CW code)
{
    struct ce_thread_local *t = local();
    if (!t->object) {
        ce_shutdown();
        gz_exit(code);
    }
    gz_mutex_lock(&lock);
    struct sync_object *o = NULL;
    for (unsigned i = 0; i < SYNC_OBJECTS; i++)
        if (objects[i].kind && objects[i].handle == t->object) {
            o = &objects[i];
            break;
        }
    if (o) {
        o->code = code;
        o->signaled = true;
        release(o);
    }
    gz_mutex_unlock(&lock);
    free(t);
    gz_self()->tls[1] = NULL;
    gz_thread_exit();
    __builtin_unreachable();
}
static void start_thread(void *arg)
{
    struct sync_object *o = arg;
    local()->object = o->handle;
    gz_mutex_lock(&lock);
    o->tid = WORD(gz_self()) >> 16;
    gz_mutex_unlock(&lock);
    for (;;) {
        gz_mutex_lock(&lock);
        bool run = !o->suspended;
        gz_mutex_unlock(&lock);
        if (run)
            break;
        gz_sleep_ms(1);
    }
    CW result = ce_call(o->proc, o->arg, 0, 0, 0);
    ce_thread_exit(result);
}
CE_API(CreateThread)
{
    if (!a2 || (a4 & ~4u) || a1 > GZ_THREAD_STACK) {
        ce_error = 87;
        return 0;
    }
    CW h = create(SYNC_THREAD, 0, false, false);
    if (!h)
        return 0;
    gz_mutex_lock(&lock);
    struct sync_object *o = lookup(h);
    o->proc = a2;
    o->arg = a3;
    o->suspended = (a4 & 4) != 0;
    o->refs++;
    gz_mutex_unlock(&lock);
    int tid = gz_thread_create(start_thread, o);
    if (tid < 0) {
        gz_mutex_lock(&lock);
        release(o);
        gz_mutex_unlock(&lock);
        bool handled;
        ce_sync_close(h, &handled);
        ce_error = 8;
        return 0;
    }
    /* The CE token must match inline KData reads translated by the loader. */
    for (;;) {
        gz_mutex_lock(&lock);
        CW token = o->tid;
        gz_mutex_unlock(&lock);
        if (token) {
            if (a5)
                *PTR(CW, a5) = token;
            break;
        }
        gz_sleep_ms(1);
    }
    return h;
}
CE_API(ResumeThread)
{
    gz_mutex_lock(&lock);
    struct sync_object *o = lookup(a0);
    CW result = (CW)-1;
    if (o && o->open && o->kind == SYNC_THREAD) {
        result = o->suspended;
        o->suspended = false;
    }
    gz_mutex_unlock(&lock);
    if (result == (CW)-1)
        ce_error = 6;
    return result;
}
/* TerminateThread(thread, code): Glaze can't stop a thread where it
 * stands. The thread object is signaled with the code, so waiters go on,
 * and a thread that hasn't started never will. */
CE_API(TerminateThread)
{
    gz_mutex_lock(&lock);
    struct sync_object *o = lookup(a0);
    bool ok = o && o->open && o->kind == SYNC_THREAD;
    if (ok && !o->signaled) {
        o->code = a1;
        o->signaled = true;
        o->suspended = true;
    }
    gz_mutex_unlock(&lock);
    if (!ok)
        ce_error = 6;
    return ok;
}
/* TryEnterCriticalSection: taken when free or already ours. The VR4131
 * has no LL/SC, so the check isn't atomic; a race only blocks briefly. */
CE_API(TryEnterCriticalSection)
{
    struct gz_mutex *m = PTR(struct gz_mutex, a0);
    if (!m || (m->owner && m->owner != gz_thread_id()))
        return 0;
    gz_mutex_lock(m);
    return 1;
}
/* Glaze can't stop another thread where it stands: a thread not yet
 * started stays suspended; a running one keeps running. */
CE_API(SuspendThread)
{
    gz_mutex_lock(&lock);
    struct sync_object *o = lookup(a0);
    CW result = (CW)-1;
    if (o && o->open && o->kind == SYNC_THREAD)
        result = o->suspended;
    gz_mutex_unlock(&lock);
    if (result == (CW)-1)
        ce_error = 6;
    return result;
}
/* GetThreadTimes(thread, creation, exit, kernel, user): not accounted. */
CE_API(GetThreadTimes)
{
    CW out[] = {a1, a2, a3, a4};
    for (unsigned i = 0; i < 4; i++)
        if (out[i])
            memset(PTR(void, out[i]), 0, 8);
    return 1;
}
CE_API(GetCurrentThreadId) { return WORD(gz_self()) >> 16; }
CE_API(GetCurrentProcessId) { return gz_getpid(); }
CE_API(GetCurrentProcess) { return 0x42; }
CE_API(GetExitCodeThread)
{
    gz_mutex_lock(&lock);
    struct sync_object *o = lookup(a0);
    bool ok = o && o->open && o->kind == SYNC_THREAD && a1;
    if (ok)
        *PTR(CW, a1) = o->signaled ? o->code : 259;
    gz_mutex_unlock(&lock);
    if (!ok)
        ce_error = 6;
    return ok;
}
static const struct ce_export exports[] = {CE_EXPORT(CreateMutexW),
                                           CE_EXPORT(ReleaseMutex),
                                           CE_EXPORT(CreateEventW),
                                           CE_EXPORT(EventModify),
                                           CE_EXPORT(SetEvent),
                                           CE_EXPORT(ResetEvent),
                                           CE_EXPORT(WaitForSingleObject),
                                           CE_EXPORT(WaitForMultipleObjects),
                                           CE_EXPORT(CreateSemaphoreW),
                                           CE_EXPORT(ReleaseSemaphore),
                                           CE_EXPORT(InterlockedExchange),
                                           CE_EXPORT(InterlockedCompareExchange),
                                           CE_EXPORT(InterlockedExchangeAdd),
                                           CE_EXPORT(SuspendThread),
                                           CE_EXPORT(TerminateThread),
                                           CE_EXPORT(TryEnterCriticalSection),
                                           CE_EXPORT(GetThreadTimes),
                                           CE_EXPORT(InitializeCriticalSection),
                                           CE_EXPORT(DeleteCriticalSection),
                                           CE_EXPORT(EnterCriticalSection),
                                           CE_EXPORT(LeaveCriticalSection),
                                           CE_EXPORT(InterlockedIncrement),
                                           CE_EXPORT(InterlockedDecrement),
                                           CE_EXPORT(TlsAlloc),
                                           CE_EXPORT(TlsFree),
                                           CE_EXPORT(TlsCall),
                                           CE_EXPORT(TlsGetValue),
                                           CE_EXPORT(TlsSetValue),
                                           CE_EXPORT(CreateThread),
                                           CE_EXPORT(ResumeThread),
                                           CE_EXPORT(GetCurrentThreadId),
                                           CE_EXPORT(GetCurrentProcessId),
                                           CE_EXPORT(GetCurrentProcess),
                                           CE_EXPORT(GetExitCodeThread)};
void *ce_resolve_thread(const char *name)
{
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(exports[i].name, name))
            return exports[i].address;
    return NULL;
}
