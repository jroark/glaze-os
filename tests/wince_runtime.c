/* Test real runtime control and synchronization code with host threads. */
#include "wince_runtime_host.h"
static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
static const void *pointers[8192];
static unsigned pointer_count;
static CW host_word(const void *p)
{
    if (!p)
        return 0;
    pthread_mutex_lock(&host_lock);
    for (unsigned i = 0; i < pointer_count; i++)
        if (pointers[i] == p) {
            pthread_mutex_unlock(&host_lock);
            return 0x10000000u + i * 65536;
        }
    assert(pointer_count < ARRAY_SIZE(pointers));
    unsigned i = pointer_count++;
    pointers[i] = p;
    pthread_mutex_unlock(&host_lock);
    return 0x10000000u + i * 65536;
}
static void *host_ptr(CW w)
{
    if (!w)
        return NULL;
    unsigned i = (w - 0x10000000u) / 65536, offset = w & 65535;
    assert(i < pointer_count);
    return (char *)pointers[i] + offset;
}
static pthread_mutex_t mutexes[256];
static unsigned mutex_count;
static int gz_mutex_init(struct gz_mutex *m)
{
    pthread_mutex_lock(&host_lock);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    assert(mutex_count < ARRAY_SIZE(mutexes));
    unsigned i = mutex_count++;
    pthread_mutex_init(&mutexes[i], &a);
    m->sem = i + 1;
    pthread_mutexattr_destroy(&a);
    pthread_mutex_unlock(&host_lock);
    return 0;
}
static void gz_mutex_destroy(struct gz_mutex *m)
{
    if (m->sem)
        pthread_mutex_destroy(&mutexes[m->sem - 1]);
    m->sem = 0;
}
static void gz_mutex_lock(struct gz_mutex *m)
{
    /* All test mutexes are initialized before launching a worker. */
    if (!m->sem)
        gz_mutex_init(m);
    pthread_mutex_lock(&mutexes[m->sem - 1]);
}
static void gz_mutex_unlock(struct gz_mutex *m) { pthread_mutex_unlock(&mutexes[m->sem - 1]); }
static _Thread_local int thread_id = 1;
static int gz_thread_id(void) { return thread_id; }
struct host_start {
    void (*fn)(void *);
    void *arg;
    int tid;
};
static void *host_start(void *p)
{
    struct host_start st = *(struct host_start *)p;
    free(p);
    thread_id = st.tid;
    st.fn(st.arg);
    return NULL;
}
static int gz_thread_create(void (*fn)(void *), void *arg)
{
    static int serial = 1;
    struct host_start *s = malloc(sizeof(*s));
    *s = (struct host_start){fn, arg, ++serial};
    int id = s->tid;
    pthread_t t;
    assert(!pthread_create(&t, NULL, host_start, s));
    pthread_detach(t);
    return id;
}
void ce_fatal(const char *s)
{
    fprintf(stderr, "fatal: %s\n", s);
    abort();
}
void ce_shutdown(void) {}
void ce_missing(const char *s) { ce_fatal(s); }
CW ce_call(CW proc, CW a, CW b, CW c, CW d) { return ((CW(*)(CW, CW, CW, CW))host_ptr(proc))(a, b, c, d); }
unsigned ce_wlen(const WCHAR *s)
{
    unsigned n = 0;
    if (s)
        while (s[n])
            n++;
    return n;
}
int ce_wcmp(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a - *b;
}
void ce_widen(WCHAR *out, uint32_t cap, const char *s)
{
    unsigned n = 0;
    while (s && *s && n + 1 < cap)
        out[n++] = (uint8_t)*s++;
    if (cap)
        out[n] = 0;
}
void ce_narrow(char *p, uint32_t cap, const WCHAR *s)
{
    unsigned n = 0;
    if (s)
        while (s[n] && n + 1 < cap) {
            p[n] = (char)s[n];
            n++;
        }
    p[n] = 0;
}
static unsigned notifications;
static CW last_notification;
static bool veto;
CW ce_send(CW w, CW msg, CW wp, CW lp)
{
    (void)w;
    (void)wp;
    assert(msg == 0x4e);
    CW *n = PTR(CW, lp);
    notifications++;
    last_notification = n[2];
    return veto;
}
struct ui_app ui;
gfx_rect ce_window_rect(CW h)
{
    (void)h;
    return gfx_r(0, 0, 240, 320);
}
const gfx_font *ui_font(int id)
{
    (void)id;
    return NULL;
}
void gfx_set_clip(gfx_surface *s, gfx_rect r)
{
    (void)s;
    (void)r;
}
void gfx_reset_clip(gfx_surface *s) { (void)s; }
void gfx_fill(gfx_surface *s, gfx_rect r, uint32_t c)
{
    (void)s;
    (void)r;
    (void)c;
}
int gfx_text(gfx_surface *s, int x, int y, const gfx_font *f, const char *t, uint32_t c, int alpha)
{
    (void)s;
    (void)x;
    (void)y;
    (void)f;
    (void)t;
    (void)c;
    (void)alpha;
    return 0;
}
#include "../apps/wince/thread.c"
#include "../apps/wince/view.c"
#include "../apps/wince/mapping.c"
#include "../apps/wince/string.c"
#include "../apps/wince/scan.c"
#include "../apps/wince/datetime.h"
#include <glaze/notify.h>
#include <glaze/tz.h>
static int clock_zone = 3;
static struct gz_datetime clock_value = {.year = 2026, .month = 7, .day = 1, .hour = 12};
int pref_get(const char *key, void *out, uint32_t size)
{
    (void)key;
    assert(size >= sizeof(clock_zone));
    memcpy(out, &clock_zone, sizeof(clock_zone));
    return sizeof(clock_zone);
}
static int gz_rtc_get(struct gz_datetime *out) { *out = clock_value; return 0; }
#include "../apps/wince/timezone.c"

#include <netinet/tcp.h>
#define exports winsock_exports
#include "../apps/wince/winsock.c"
#undef exports
#define exports ole_exports
#include "../apps/wince/ole.c"
#undef exports
int net_resolve(const char *host, uint32_t *addr, int timeout)
{
    (void)timeout;
    if (strcmp(host, "127.0.0.1"))
        return -1;
    *addr = 0x0100007f;
    return 0;
}
#define API(fn, ...) ce_##fn(__VA_ARGS__, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
/* API calls have twelve words; use an array wrapper to avoid ABI-dependent
 * varargs in the tests. */
static CW invoke(void *fn, CW a, CW b, CW c, CW d, CW e, CW f)
{
    return ((CW(*)(CW, CW, CW, CW, CW, CW, CW, CW, CW, CW, CW, CW))fn)(a, b, c, d, e, f, 0, 0, 0, 0, 0, 0);
}
static CW counter, slot, worker_token;
static CW worker(CW event, CW b, CW c, CW d)
{
    (void)b;
    (void)c;
    (void)d;
    worker_token = invoke(ce_GetCurrentThreadId, 0, 0, 0, 0, 0, 0);
    assert(!invoke(ce_TlsGetValue, UINT32_MAX, 0, 0, 0, 0, 0));
    assert(invoke(ce_TlsSetValue, UINT32_MAX, 91, 0, 0, 0, 0));
    assert(invoke(ce_TlsGetValue, slot, 0, 0, 0, 0, 0) == 0);
    assert(ce_error == 0);
    ce_error = 77;
    assert(invoke(ce_TlsSetValue, slot, 42, 0, 0, 0, 0));
    for (unsigned i = 0; i < 10000; i++)
        invoke(ce_InterlockedIncrement, WORD(&counter), 0, 0, 0, 0, 0);
    assert(invoke(ce_SetEvent, event, 0, 0, 0, 0, 0));
    return 123;
}
static void sync_test(void)
{
    gz_mutex_init(&lock);
    CW section[6] = {0, 0, 0, 0, 0, 0xfeedbeef};
    invoke(ce_InitializeCriticalSection, WORD(section), 0, 0, 0, 0, 0);
    assert(section[5] == 0xfeedbeef);
    invoke(ce_EnterCriticalSection, WORD(section), 0, 0, 0, 0, 0);
    invoke(ce_EnterCriticalSection, WORD(section), 0, 0, 0, 0, 0);
    invoke(ce_LeaveCriticalSection, WORD(section), 0, 0, 0, 0, 0);
    invoke(ce_LeaveCriticalSection, WORD(section), 0, 0, 0, 0, 0);
    invoke(ce_DeleteCriticalSection, WORD(section), 0, 0, 0, 0, 0);
    slot = invoke(ce_TlsAlloc, 0, 0, 0, 0, 0, 0);
    assert(invoke(ce_TlsSetValue, UINT32_MAX, 73, 0, 0, 0, 0));
    assert(invoke(ce_TlsSetValue, slot, 17, 0, 0, 0, 0));
    ce_error = 99;
    CW event = invoke(ce_CreateEventW, 0, 0, 0, 0, 0, 0);
    CW token = 0;
    CW thread = invoke(ce_CreateThread, 0, 0, WORD(worker), event, 4, WORD(&token));
    assert(thread);
    assert(token && token != invoke(ce_GetCurrentThreadId, 0, 0, 0, 0, 0, 0));
    assert(invoke(ce_WaitForSingleObject, event, 0, 0, 0, 0, 0) == 258);
    assert(invoke(ce_ResumeThread, thread, 0, 0, 0, 0, 0) == 1);
    ce_error = 99;
    for (unsigned i = 0; i < 10000; i++)
        invoke(ce_InterlockedIncrement, WORD(&counter), 0, 0, 0, 0, 0);
    assert(invoke(ce_WaitForSingleObject, event, 5000, 0, 0, 0, 0) == 0);
    assert(invoke(ce_WaitForSingleObject, event, 0, 0, 0, 0, 0) == 258);
    assert(invoke(ce_WaitForSingleObject, thread, 5000, 0, 0, 0, 0) == 0);
    CW code = 0;
    assert(invoke(ce_GetExitCodeThread, thread, WORD(&code), 0, 0, 0, 0) && code == 123);
    assert(counter == 20000 && ce_error == 99);
    assert(worker_token == token);
    assert(invoke(ce_TlsGetValue, UINT32_MAX, 0, 0, 0, 0, 0) == 73);
    assert(!invoke(ce_TlsSetValue, TLS_SLOTS, 5, 0, 0, 0, 0) && ce_error == 87);
    assert(invoke(ce_TlsGetValue, slot, 0, 0, 0, 0, 0) == 17);
    assert(invoke(ce_TlsFree, slot, 0, 0, 0, 0, 0));
    assert(invoke(ce_TlsAlloc, 0, 0, 0, 0, 0, 0) == slot);
    assert(invoke(ce_TlsGetValue, slot, 0, 0, 0, 0, 0) == 0);
    assert(invoke(ce_TlsCall, 1, slot, 0, 0, 0, 0));
    assert(invoke(ce_TlsCall, 0, 0, 0, 0, 0, 0) == slot);
    bool handled;
    assert(ce_sync_close(thread, &handled) && handled);
    assert(!ce_sync_close(thread, &handled));
    assert(ce_sync_close(event, &handled));
    WCHAR name[] = {'m', 0};
    CW a = invoke(ce_CreateMutexW, 0, 1, WORD(name), 0, 0, 0);
    CW alias = invoke(ce_CreateMutexW, 0, 0, WORD(name), 0, 0, 0);
    assert(a && alias && a != alias && ce_error == 183);
    assert(invoke(ce_WaitForSingleObject, alias, 0, 0, 0, 0, 0) == 0);
    assert(invoke(ce_ReleaseMutex, alias, 0, 0, 0, 0, 0));
    assert(ce_sync_close(a, &handled));
    assert(invoke(ce_ReleaseMutex, alias, 0, 0, 0, 0, 0));
    assert(ce_sync_close(alias, &handled));
    assert(!invoke(ce_ReleaseMutex, alias, 0, 0, 0, 0, 0));
    free(host_tcb.tls[1]);
    host_tcb.tls[1] = NULL;
}
static CW send_view(struct ce_window *w, CW m, CW wp, CW lp)
{
    CW r;
    assert(ce_view_message(w, m, wp, lp, &r));
    return r;
}
static void view_test(void)
{
    struct ce_window w = {.handle = 0x70000000, .parent = 0x70000001, .id = 12};
    strcpy(w.klass, "SysTreeView32");
    assert(ce_view_init(&w));
    WCHAR root[] = {'R', 0}, child[] = {'C', 0};
    CW item[12] = {TREE_ROOT, TREE_LAST, 1 | 4 | 0x40, 0, 0, 0, WORD(root), 0, 0, 0, 1, 123};
    CW h = send_view(&w, 0x1132, 0, WORD(item));
    assert(h);
    item[0] = h;
    item[6] = WORD(child);
    item[11] = 456;
    CW ch = send_view(&w, 0x1132, 0, WORD(item));
    assert(ch);
    unsigned order[VIEW_ITEMS];
    assert(rows(w.view, order) == 1);
    send_view(&w, 0x1102, 2, h);
    assert(rows(w.view, order) == 2);
    veto = true;
    send_view(&w, 0x110b, 9, ch);
    assert(w.view->selected == -1);
    veto = false;
    send_view(&w, 0x110b, 9, ch);
    assert(last_notification == (CW)-451);
    assert(send_view(&w, 0x110a, 3, ch) == h);
    assert(send_view(&w, 0x110a, 4, h) == ch);
    CW get[10] = {4, ch};
    assert(send_view(&w, 0x113e, 0, WORD(get)) && get[9] == 456);
    assert(send_view(&w, 0x1101, 0, h));
    assert(w.view->count == 0 && w.view->selected == -1);
    ce_view_free(&w);
    strcpy(w.klass, "SysListView32");
    assert(ce_view_init(&w));
    CW row[9] = {1 | 4, 0, 0, 0, 0, WORD(root), 0, 0, 999};
    assert(send_view(&w, 0x104d, 0, WORD(row)) == 0);
    assert(send_view(&w, 0x1004, 0, 0) == 1);
    CW data[9] = {4, 0};
    assert(send_view(&w, 0x104b, 0, WORD(data)) && data[8] == 999);
    assert(send_view(&w, 0x1009, 0, 0));
    assert(!w.view->count);
    ce_view_free(&w);
}
static void winsock_test(void)
{
    int pair[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    CW millis = 1000, size = 4, observed = 0;
    assert(!invoke(ce_setsockopt, pair[0], 0xffff, 0x1006, WORD(&millis), 4, 0));
    assert(!invoke(ce_getsockopt, pair[0], 0xffff, 0x1006, WORD(&observed), WORD(&size), 0));
    assert(observed == 1000 && size == 4);
    CW nonblocking = 1;
    assert(!invoke(ce_ioctlsocket, pair[0], 0x8004667e, WORD(&nonblocking), 0, 0, 0));
    char buf[16] = {0};
    assert(invoke(ce_recv, pair[0], WORD(buf), sizeof(buf), 0, 0, 0) == (CW)-1);
    assert(ce_error == 10035);
    CW set[65] = {1, (CW)pair[0]}, timeout[2] = {0, 0};
    assert(!invoke(ce_select, 0, WORD(set), 0, 0, WORD(timeout), 0));
    assert(set[0] == 0);
    assert(send(pair[1], "hello", 5, 0) == 5);
    set[0] = 1;
    set[1] = pair[0];
    assert(invoke(ce_select, 0, WORD(set), 0, 0, WORD(timeout), 0) == 1);
    assert(set[0] == 1 && set[1] == (CW)pair[0]);
    assert(invoke(ce___WSAFDIsSet, pair[0], WORD(set), 0, 0, 0, 0));
    assert(invoke(ce_recv, pair[0], WORD(buf), sizeof(buf), 0, 0, 0) == 5 && !memcmp(buf, "hello", 5));
    assert(invoke(ce_sock_send, pair[0], WORD(buf), 5, 0, 0, 0) == 5);
    assert(recv(pair[1], buf, sizeof(buf), 0) == 5);
    assert(!invoke(ce_closesocket, pair[0], 0, 0, 0, 0, 0));
    close(pair[1]);
    CW host = invoke(ce_gethostbyname, WORD("127.0.0.1"), 0, 0, 0, 0, 0);
    assert(host && PTR(CW, host)[2] == (2u | (4u << 16)));
    assert(*PTR(CW, PTR(CW, PTR(CW, host)[3])[0]) == 0x0100007f);
    free(host_tcb.tls[0]);
    host_tcb.tls[0] = NULL;
}
static void timezone_test(void)
{
    uint8_t info[172];
    assert(invoke(ce_GetTimeZoneInformation, WORD(info), 0, 0, 0, 0, 0) == 2);
    assert((int32_t)ce_u32(info) == 480 && (int32_t)ce_u32(info + 168) == -60);
    assert(ce_u16(info + 154) == 3 && ce_u16(info + 158) == 2);
    uint16_t utc[8] = {2026, 7, 0, 1, 19, 30, 0, 123}, local[8];
    uint64_t ticks;
    uint8_t input[8], output[8], roundtrip[8];
    assert(ce_datetime_ticks(utc, &ticks));
    ce_put32(input, ticks); ce_put32(input + 4, ticks >> 32);
    assert(invoke(ce_FileTimeToLocalFileTime, WORD(input), WORD(output), 0, 0, 0, 0));
    ticks = ce_u32(output) | (uint64_t)ce_u32(output + 4) << 32;
    assert(ce_datetime_from_ticks(ticks, local));
    assert(local[4] == 12 && local[5] == 30 && local[7] == 123);
    assert(invoke(ce_LocalFileTimeToFileTime, WORD(output), WORD(roundtrip), 0, 0, 0, 0));
    assert(!memcmp(input, roundtrip, 8));
    clock_value.month = 1;
    assert(invoke(ce_GetTimeZoneInformation, WORD(info), 0, 0, 0, 0, 0) == 1);
    clock_zone = 16;
    assert(invoke(ce_GetTimeZoneInformation, WORD(info), 0, 0, 0, 0, 0) == 2);
    clock_zone = 13; /* Kolkata: fixed half-hour offset. */
    assert(invoke(ce_GetTimeZoneInformation, WORD(info), 0, 0, 0, 0, 0) == 0);
    assert((int32_t)ce_u32(info) == -330);
    assert(invoke(ce_FileTimeToLocalFileTime, WORD(input), WORD(output), 0, 0, 0, 0));
    ticks = ce_u32(output) | (uint64_t)ce_u32(output + 4) << 32;
    assert(ce_datetime_from_ticks(ticks, local));
    assert(local[3] == 2 && local[4] == 1 && local[5] == 0);
    assert(!invoke(ce_FileTimeToLocalFileTime, 0, WORD(output), 0, 0, 0, 0) && ce_error == 87);
}
static void scan_test(void)
{
    WCHAR input[64], format[64];
    CW month = 0, day = 0, year = 0, used = 0;
    ce_widen(input, 64, "10/21/2026");
    ce_widen(format, 64, "%u/%u/%u%n");
    assert(ce_swscanf(WORD(input), WORD(format), WORD(&month), WORD(&day), WORD(&year),
        WORD(&used), 0, 0, 0, 0, 0, 0) == 3);
    assert(month == 10 && day == 21 && year == 2026 && used == 10);
    uint16_t small[2] = {0, 0xabcd};
    ce_widen(input, 64, "14:ff"); ce_widen(format, 64, "%*u:%hx");
    assert(invoke(ce_swscanf, WORD(input), WORD(format), WORD(small), 0, 0, 0) == 1);
    assert(small[0] == 255 && small[1] == 0xabcd);
    uint8_t large[8];
    ce_widen(input, 64, "0x100000000"); ce_widen(format, 64, "%lli");
    assert(invoke(ce_swscanf, WORD(input), WORD(format), WORD(large), 0, 0, 0) == 1);
    assert(!ce_u32(large) && ce_u32(large + 4) == 1);
    ce_widen(input, 64, "123"); ce_widen(format, 64, "%2u");
    assert(invoke(ce_swscanf, WORD(input), WORD(format), WORD(&used), 0, 0, 0) == 1 && used == 12);
    ce_widen(input, 64, "abc"); ce_widen(format, 64, "%u"); used = 99;
    assert(!invoke(ce_swscanf, WORD(input), WORD(format), WORD(&used), 0, 0, 0) && used == 99);
    input[0] = 0;
    assert(invoke(ce_swscanf, WORD(input), WORD(format), WORD(&used), 0, 0, 0) == (CW)-1);
}
static void string_test(void)
{
    WCHAR input[] = {',', 'a', ',', ',', 'b', 0}, delim[] = {',', 0};
    CW token = invoke(ce_wcstok, WORD(input), WORD(delim), 0, 0, 0, 0);
    assert(PTR(WCHAR, token)[0] == 'a' && !PTR(WCHAR, token)[1]);
    token = invoke(ce_wcstok, 0, WORD(delim), 0, 0, 0, 0);
    assert(PTR(WCHAR, token)[0] == 'b' && !PTR(WCHAR, token)[1]);
    assert(!invoke(ce_wcstok, 0, WORD(delim), 0, 0, 0, 0));
    WCHAR output[64] = {'x', 0}, src[] = {'y', 'z', 0};
    invoke(ce_wcsncat, WORD(output), WORD(src), 1, 0, 0, 0);
    assert(output[0] == 'x' && output[1] == 'y' && !output[2]);
    CW dup = invoke(ce__wcsdup, WORD(output), 0, 0, 0, 0, 0);
    assert(dup && !memcmp(PTR(void, dup), output, 6));
    free(PTR(void, dup));
    invoke(ce__itow, 0x80000000u, WORD(output), 10, 0, 0, 0);
    char narrow[64];
    ce_narrow(narrow, sizeof(narrow), output);
    assert(!strcmp(narrow, "-2147483648"));
    invoke(ce__itow, 0xabcdefu, WORD(output), 16, 0, 0, 0);
    ce_narrow(narrow, sizeof(narrow), output);
    assert(!strcmp(narrow, "abcdef"));
}
static void mapping_test(void)
{
    WCHAR name[] = {'P', 'I', 'M', 0};
    CW h = invoke(ce_CreateFileMappingW, (CW)-1, 0, 4, 0, 2048, WORD(name));
    assert(h && !ce_error);
    CW alias = invoke(ce_CreateFileMappingW, (CW)-1, 0, 4, 0, 4096, WORD(name));
    assert(alias && alias != h && ce_error == 183);
    CW v = invoke(ce_MapViewOfFile, h, 2, 0, 0, 2048, 0);
    CW other = invoke(ce_MapViewOfFile, alias, 4, 0, 0, 0, 0);
    assert(v && v == other && !*PTR(CW, v));
    *PTR(CW, v) = 0xcafebabe;
    bool handled;
    assert(ce_mapping_close(h, &handled) && handled);
    assert(!ce_mapping_close(h, &handled) && handled && ce_error == 6);
    assert(!invoke(ce_MapViewOfFile, alias, 2, 0, 0, 2049, 0) && ce_error == 87);
    assert(ce_mapping_close(alias, &handled));
    assert(*PTR(CW, other) == 0xcafebabe);
    assert(invoke(ce_UnmapViewOfFile, v, 0, 0, 0, 0, 0));
    assert(*PTR(CW, other) == 0xcafebabe);
    assert(invoke(ce_UnmapViewOfFile, other, 0, 0, 0, 0, 0));
    assert(!invoke(ce_UnmapViewOfFile, other, 0, 0, 0, 0, 0));
    h = invoke(ce_CreateFileMappingW, (CW)-1, 0, 2, 0, 2048, WORD(name));
    assert(h && !ce_error);
    assert(!invoke(ce_MapViewOfFile, h, 2, 0, 0, 0, 0) && ce_error == 5);
    v = invoke(ce_MapViewOfFile, h, 4, 0, 0, 0, 0);
    assert(v && !*PTR(CW, v));
    assert(invoke(ce_UnmapViewOfFile, v, 0, 0, 0, 0, 0));
    assert(ce_mapping_close(h, &handled));
}
static void ole_test(void)
{
    WCHAR text[] = {'a', 0, 'b', 0};
    CW b = invoke(ce_SysAllocStringLen, WORD(text), 3, 0, 0, 0, 0);
    assert(b && invoke(ce_SysStringLen, b, 0, 0, 0, 0, 0) == 3);
    assert(!memcmp(PTR(void, b), text, sizeof(text)));
    invoke(ce_SysFreeString, b, 0, 0, 0, 0, 0);
    b = invoke(ce_SysAllocString, WORD(text), 0, 0, 0, 0, 0);
    assert(invoke(ce_SysStringLen, b, 0, 0, 0, 0, 0) == 1);
    invoke(ce_SysFreeString, b, 0, 0, 0, 0, 0);
    assert(!invoke(ce_SysAllocString, 0, 0, 0, 0, 0, 0));
}
int main(void)
{
    sync_test();
    view_test();
    winsock_test();
    ole_test();
    mapping_test();
    string_test();
    scan_test();
    timezone_test();
    free(host_tcb.tls[1]);
    host_tcb.tls[1] = NULL;
    puts("WinCE runtime synchronization, views and Winsock passed");
    return 0;
}
