/* Process-private CE synchronization and explicit TLS, backed by Glaze threads.
 * The VR4131 has no LL/SC; interlocked operations must use a kernel mutex. */
#include "runtime.h"
#include <glaze/thread.h>
#define TLS_SLOTS 64
#define SYNC_OBJECTS 64
struct ce_thread_local {
    CW error, values[TLS_SLOTS], generations[TLS_SLOTS], object;
};
static struct gz_mutex lock = GZ_MUTEX_INIT;
static uint64_t tls_used;
static CW tls_generation[TLS_SLOTS];
static CW serial;
enum { SYNC_MUTEX = 1, SYNC_EVENT, SYNC_THREAD };
static struct sync_object {
    CW handle, kind, refs, owner, depth, code, proc, arg;
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
CE_API(WaitForSingleObject)
{
    CW start = gz_time_ms(), tid = gz_thread_id();
    gz_mutex_lock(&lock);
    struct sync_object *o = lookup(a0);
    if (!o || !o->open) {
        gz_mutex_unlock(&lock);
        ce_error = 6;
        return (CW)-1;
    }
    o->refs++;
    for (;;) {
        bool ready = o->kind == SYNC_MUTEX ? (!o->owner || o->owner == tid) : o->signaled;
        if (ready) {
            if (o->kind == SYNC_MUTEX) {
                o->owner = tid;
                o->depth++;
            }
            if (o->kind == SYNC_EVENT && !o->manual)
                o->signaled = false;
            release(o);
            gz_mutex_unlock(&lock);
            return 0;
        }
        if (a1 != 0xffffffffu && gz_time_ms() - start >= a1) {
            release(o);
            gz_mutex_unlock(&lock);
            return 258;
        }
        gz_mutex_unlock(&lock);
        gz_sleep_ms(1);
        gz_mutex_lock(&lock);
    }
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
    if (a5)
        *PTR(CW, a5) = tid;
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
CE_API(GetCurrentThreadId) { return gz_thread_id(); }
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
                                           CE_EXPORT(InitializeCriticalSection),
                                           CE_EXPORT(DeleteCriticalSection),
                                           CE_EXPORT(EnterCriticalSection),
                                           CE_EXPORT(LeaveCriticalSection),
                                           CE_EXPORT(InterlockedIncrement),
                                           CE_EXPORT(InterlockedDecrement),
                                           CE_EXPORT(TlsAlloc),
                                           CE_EXPORT(TlsFree),
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
