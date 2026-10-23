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
static CW activation_data, activation_item;
static unsigned double_clicks, activations;
static unsigned list_changing, list_changed;
static CW list_before, list_after;
static bool veto;
static struct ce_window *combo_fixture;
static const WCHAR *owner_caption;
struct ce_window *ce_window(CW handle)
{
    return combo_fixture && combo_fixture->handle == handle ? combo_fixture : NULL;
}
bool ce_window_visible(CW handle) { return ce_window(handle) != NULL; }
void ce_redraw_all(void) {}
CW ce_send(CW w, CW msg, CW wp, CW lp)
{
    if (combo_fixture) {
        if (msg == 0xc) {
            assert(w == combo_fixture->handle);
            unsigned n = ce_wlen(PTR(WCHAR, lp));
            combo_fixture->text = realloc(combo_fixture->text, (n + 1) * 2);
            assert(combo_fixture->text);
            memcpy(combo_fixture->text, PTR(void, lp), (n + 1) * 2);
            combo_fixture->length = n;
        } else {
            assert(msg == 0x111);
            last_notification = wp >> 16;
            notifications++;
        }
        return 0;
    }
    (void)w;
    (void)wp;
    assert(msg == 0x4e);
    CW *n = PTR(CW, lp);
    if (n[2] == (CW)-100) {
        list_changing++;
        list_before = n[6]; list_after = n[5];
    }
    if (n[2] == (CW)-101) list_changed++;
    if (n[2] == (CW)-3) double_clicks++;
    if (n[2] == (CW)-114) activations++;
    if (n[2] == (CW)-2 || n[2] == (CW)-3 || n[2] == (CW)-114) {
        activation_item = n[3];
        activation_data = n[10];
    }
    if (owner_caption && n[2] == (CW)-177) {
        assert(n[3] == 1 && n[4] == 1 && n[5] == 0);
        n[8] = WORD(owner_caption);
        return 0;
    }
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
#include "../apps/wince/list.c"
#include "../apps/wince/tabs.c"

static void tabs_test(void)
{
    struct ce_tabs tabs = {.selected = -1};
    struct ce_window w = {.handle = 11, .parent = 9, .id = 1001, .tabs = &tabs};
    static const WCHAR label[] = {'C','h','a','t',0};
    CW item[] = {11, 0, 0, WORD(label), 0, 7, 0x12345678};
    bool handled;
    assert(ce_tabs_message(&w, 0x133e, 0, WORD(item), &handled) == 0 && handled);
    item[6] = 0x87654321;
    assert(ce_tabs_message(&w, 0x133e, 0, WORD(item), &handled) == 0);
    WCHAR text[3];
    CW out[] = {11, 0, 0, WORD(text), 3, 0, 0};
    assert(ce_tabs_message(&w, 0x133c, 1, WORD(out), &handled) == 1);
    assert(out[6] == 0x12345678 && out[5] == 7 && text[0] == 'C' && text[1] == 'h' && !text[2]);
    item[0] = 8; item[6] = 0xfeed;
    assert(ce_tabs_message(&w, 0x133d, 1, WORD(item), &handled) == 1);
    assert(tabs.param[1] == 0xfeed && tabs.image[1] == 7 && tabs.text[1][0] == 'C');
    assert(ce_tabs_message(&w, 0x1308, 0, 0, &handled) == 1);
    assert(tabs.count == 1 && tabs.param[0] == 0xfeed);
    while (tabs.count < 16) ce_tabs_message(&w, 0x133e, tabs.count, WORD(item), &handled);
    assert(ce_tabs_message(&w, 0x133e, 16, WORD(item), &handled) == (CW)-1);
    ce_tabs_message(&w, 0x1309, 0, 0, &handled);
    assert(!tabs.count && tabs.selected == -1);
    ce_tabs_message(&w, 0xc, 0, 0, &handled);
    assert(!handled);
}
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
#include "../apps/wince/date_control.c"

static void date_control_test(void)
{
    struct ce_window w = {.handle = 10, .parent = 9, .id = 1000};
    strcpy(w.klass, "SysDateTimePick32");
    CW result;
    uint16_t value[8] = {2024, 2, 4, 29, 10, 30, 0, 0}, out[16] = {0};
    assert(ce_date_message(&w, 0x1002, 0, WORD(value), &result) && result == 1);
    ce_date_message(&w, 0x1001, 0, WORD(out), &result);
    assert(!result && !memcmp(value, out, 16));
    value[0] = 2023; /* An invalid leap day must leave the old value intact. */
    ce_date_message(&w, 0x1002, 0, WORD(value), &result);
    assert(!result && w.date.value[0] == 2024);
    uint16_t bounds[16] = {2024, 2, 0, 28, 0, 0, 0, 0, 2024, 3, 0, 1, 23, 59, 59, 0};
    ce_date_message(&w, 0x1004, 3, WORD(bounds), &result);
    assert(result == 1);
    ce_date_message(&w, 0x1003, 0, WORD(out), &result);
    assert(result == 3 && !memcmp(bounds, out, 32));
    unsigned before = notifications;
    ce_date_message(&w, 0x100, 0x26, 0, &result);
    assert(w.date.value[1] == 3 && w.date.value[3] == 1);
    assert(notifications == before + 1 && last_notification == (CW)-759);
    ce_date_message(&w, 0x100, 0x26, 0, &result);
    assert(w.date.value[3] == 1 && notifications == before + 1);
    ce_date_message(&w, 0x1002, 1, 0, &result);
    assert(!result); /* GDT_NONE requires DTS_SHOWNONE. */
    w.style |= 2;
    ce_date_message(&w, 0x1002, 1, 0, &result);
    assert(result == 1);
    ce_date_message(&w, 0x1001, 0, WORD(out), &result);
    assert(result == 1);
    WCHAR text[64];
    ce_date_text(&w, text, ARRAY_SIZE(text));
    assert(!text[0]);
    ce_date_message(&w, 0x1002, 0, WORD(bounds), &result);
    ce_date_text(&w, text, ARRAY_SIZE(text));
    static const WCHAR expected[] = {'2','0','2','4','-','0','2','-','2','8',0};
    assert(!ce_wcmp(text, expected));
    /* An inverted range is rejected without replacing the previous range. */
    ce_date_message(&w, 0x1004, 3, WORD(out), &result);
    assert(result == 1); /* out still holds the valid range from GETRANGE. */
    memcpy(out, bounds + 8, 16); memcpy(out + 8, bounds, 16);
    ce_date_message(&w, 0x1004, 3, WORD(out), &result);
    assert(!result && !memcmp(w.date.minimum, bounds, 16));
}

#include <netinet/tcp.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
static pthread_mutex_t serial_network = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t poll_notice = PTHREAD_COND_INITIALIZER;
static bool serial_enabled, poll_entered;
static bool connect_in_progress;
static int progress_connect(int fd, const struct sockaddr *address, socklen_t length)
{
    if (connect_in_progress) { errno = EINPROGRESS; return -1; }
    return connect(fd, address, length);
}
static int serial_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    if (!serial_enabled) return poll(fds, count, timeout);
    pthread_mutex_lock(&serial_network);
    pthread_mutex_lock(&host_lock);
    poll_entered = true;
    pthread_cond_signal(&poll_notice);
    pthread_mutex_unlock(&host_lock);
    int result = poll(fds, count, timeout);
    pthread_mutex_unlock(&serial_network);
    return result;
}
static ssize_t serial_send(int fd, const void *data, size_t size, int flags)
{
    if (!serial_enabled) return send(fd, data, size, flags);
    pthread_mutex_lock(&serial_network);
    ssize_t result = send(fd, data, size, flags);
    pthread_mutex_unlock(&serial_network);
    return result;
}
#define poll serial_poll
#define send serial_send
#define connect progress_connect
#define exports winsock_exports
#include "../apps/wince/winsock.c"
#undef exports
#undef poll
#undef send
#undef connect
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
    CW row[9] = {1 | 2 | 4, 0, 0, 0, 0, WORD(root), 0, (CW)-1, 999};
    assert(send_view(&w, 0x104d, 0, WORD(row)) == 0);
    assert(send_view(&w, 0x1004, 0, 0) == 1);
    CW data[9] = {2 | 4, 0};
    assert(send_view(&w, 0x104b, 0, WORD(data)) && data[8] == 999 && data[7] == (CW)-1);
    data[7] = 3;
    assert(send_view(&w, 0x104c, 0, WORD(data)));
    data[7] = 0;
    assert(send_view(&w, 0x104b, 0, WORD(data)) && data[7] == 3);
    WCHAR row_text[16];
    CW text_item[9] = {0xffffffffu, 77, 0, 0xabcdef, 0xffffffffu,
        WORD(row_text), ARRAY_SIZE(row_text), 456, 789};
    assert(send_view(&w, 0x1073, 0, WORD(text_item)) == ce_wlen(root));
    assert(!ce_wcmp(row_text, root) && text_item[3] == 0xabcdef && text_item[7] == 456 && text_item[8] == 789);
    text_item[5] = WORD(child);
    assert(send_view(&w, 0x1074, 0, WORD(text_item)));
    assert(send_view(&w, 0x104b, 0, WORD(data)) && data[8] == 999 && data[7] == 3);
    CW state[9] = {0, 0, 0, 3, 3};
    unsigned changes = list_changed;
    veto = true;
    assert(send_view(&w, 0x102b, 0, WORD(state)));
    assert(send_view(&w, 0x102c, 0, 3) == 0 && list_changed == changes);
    veto = false;
    assert(send_view(&w, 0x102b, 0, WORD(state)));
    assert(send_view(&w, 0x102c, 0, 3) == 3);
    assert(list_changing && list_before == 0 && list_after == 3);
    send_view(&w, 8, 0, 0);
    assert(send_view(&w, 0x102c, 0, 3) == 2 && list_before == 3 && list_after == 2);
    send_view(&w, 7, 0, 0);
    assert(send_view(&w, 0x102c, 0, 3) == 3 && list_before == 2 && list_after == 3);
    state[3] = 0x2000; state[4] = 0xf000;
    assert(send_view(&w, 0x102b, 0, WORD(state)));
    assert(send_view(&w, 0x102c, 0, 0xffff) == 0x2003);
    state[3] = 0; state[4] = 2;
    assert(send_view(&w, 0x102b, (CW)-1, WORD(state)));
    assert(send_view(&w, 0x102c, 0, 2) == 0 && send_view(&w, 0x1032, 0, 0) == 0);
    assert(send_view(&w, 0x102c, 0, 0xf000) == 0x2000);
    assert(send_view(&w, 0x102c, 99, 3) == 0);
    send_view(&w, 0x202, 0, 1u << 16 | 1);
    assert(last_notification == (CW)-2 && activation_item == 0 && activation_data == 999);
    unsigned doubles = double_clicks, activated = activations;
    send_view(&w, 0x202, 0, 1u << 16 | 1);
    assert(last_notification == (CW)-114 && activation_item == 0 && activation_data == 999);
    assert(double_clicks == doubles + 1 && activations == activated + 1);
    send_view(&w, 0x100, 13, 0);
    assert(last_notification == (CW)-114 && activations == activated + 2 && double_clicks == doubles + 1);
    assert(send_view(&w, 0x1009, 0, 0));
    assert(!w.view->count);
    ce_view_free(&w);
    w.style = 0x1000;
    assert(ce_view_init(&w));
    assert(send_view(&w, 0x102f, 3, 0));
    static const WCHAR before[] = {'O', 'l', 'd', 0}, after[] = {'N', 'e', 'w', 0};
    WCHAR caption[16];
    CW virtual_row[9] = {1, 1, 0, 0, 0, WORD(caption), ARRAY_SIZE(caption)};
    owner_caption = before;
    assert(send_view(&w, 0x1073, 1, WORD(virtual_row)) == 3 && caption[0] == 'O');
    owner_caption = after;
    assert(send_view(&w, 0x1073, 1, WORD(virtual_row)) == 3 && caption[0] == 'N');
    assert(!send_view(&w, 0x102f, VIEW_ITEMS + 1, 0));
    assert(send_view(&w, 0x1004, 0, 0) == 3);
    assert(send_view(&w, 0x102f, 0, 0));
    assert(send_view(&w, 0x1004, 0, 0) == 0);
    owner_caption = NULL;
    ce_view_free(&w);
}
struct duplex_select { CW fd, result; };
static void *duplex_wait(void *arg)
{
    struct duplex_select *state = arg;
    CW set[65] = {1, state->fd}, timeout[] = {0, 500000};
    state->result = invoke(ce_select, 0, WORD(set), 0, 0, WORD(timeout), 0);
    free(host_tcb.tls[1]);
    host_tcb.tls[1] = NULL;
    return NULL;
}
static void winsock_test(void)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(5190)};
    connect_in_progress = true;
    assert(invoke(ce_connect, 0, WORD(&address), sizeof(address), 0, 0, 0) == (CW)-1);
    assert(ce_error == 10035); /* Winsock connect reports WSAEWOULDBLOCK. */
    connect_in_progress = false;
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
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    serial_enabled = true;
    poll_entered = false;
    struct duplex_select waiting = {(CW)pair[0], (CW)-1};
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, duplex_wait, &waiting));
    pthread_mutex_lock(&host_lock);
    while (!poll_entered)
        pthread_cond_wait(&poll_notice, &host_lock);
    pthread_mutex_unlock(&host_lock);
    assert(invoke(ce_sock_send, pair[1], WORD("x"), 1, 0, 0, 0) == 1);
    pthread_join(worker, NULL);
    assert(waiting.result == 1); /* A blocking select must allow concurrent writes. */
    serial_enabled = false;
    close(pair[0]); close(pair[1]);
    host_tcb.tls[0] = NULL;
}
static void combo_test(void)
{
    struct ce_window w = {.handle = 0x70000000, .parent = 0x70000001, .id = 1010,
        .style = 0x50000002, .rect = {0, 0, 240, 24}};
    strcpy(w.klass, "ComboBox");
    combo_fixture = &w;
    assert(ce_list_init(&w));
    WCHAR name[] = {'T', 'e', 's', 't', 0};
    CW result;
    assert(ce_list_message(&w, 0x143, 0, WORD(name), &result) && result == 0);
    assert(ce_list_message(&w, 0x14e, 0, 0, &result) && result == 0);
    assert(w.length == 4 && !memcmp(w.text, name, sizeof(name)));
    assert(ce_list_message(&w, 0x142, 0, 3u << 16 | 1, &result));
    CW start = 99, end = 99;
    assert(ce_list_message(&w, 0x140, WORD(&start), WORD(&end), &result));
    assert(start == 1 && end == 3 && result == (3u << 16 | 1));
    assert(ce_list_message(&w, 0x202, 0, 5, &result) && !dropdown);
    assert(ce_list_message(&w, 0x202, 0, 230, &result) && dropdown == w.handle);
    assert(ce_list_message(&w, 0x14e, (CW)-1, 0, &result));
    assert(!w.length && result == (CW)-1);
    static const WCHAR prefix[] = {'t','e',0}, exact[] = {'t','E','s','T',0};
    assert(ce_list_message(&w, 0x14c, (CW)-1, WORD(prefix), &result) && result == 0);
    assert(ce_list_message(&w, 0x158, (CW)-1, WORD(prefix), &result) && result == (CW)-1);
    assert(ce_list_message(&w, 0x158, 0, WORD(exact), &result) && result == 0);
    unsigned before = notifications;
    assert(ce_list_message(&w, 0x14d, (CW)-1, WORD(prefix), &result) && result == 0);
    assert(w.length == 4 && !memcmp(w.text, name, sizeof(name)) && notifications == before);
    assert(ce_list_message(&w, 0x141, 20, 0, &result) && w.text_limit == 20);
    ce_list_free(&w);
    free(w.text);
    combo_fixture = NULL;
}
static void timezone_test(void)
{
    uint8_t info[172] = {0};
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
    static const struct { const char *text; CW expected; } numbers[] = {
        {"0x00000001", 1}, {"  0Xabcdef", 0xabcdef}, {"xFf!", 255},
        {"-2147483648", 0x80000000u}, {"+123 tail", 123}, {"0123", 123},
        {"0xffffffff", 0xffffffffu}, {"4294967296", 0}, {"0", 0},
        {"-0xff", 0}, {"0x", 0}, {"", 0}, {"\v\f42", 42},
    };
    assert(ce_resolve_string("_wtol") == (void *)ce__wtol);
    for (unsigned i = 0; i < ARRAY_SIZE(numbers); i++) {
        ce_widen(output, ARRAY_SIZE(output), numbers[i].text);
        assert(invoke(ce__wtol, WORD(output), 0, 0, 0, 0, 0) == numbers[i].expected);
    }
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
    combo_test();
    tabs_test();
    winsock_test();
    ole_test();
    mapping_test();
    string_test();
    scan_test();
    timezone_test();
    date_control_test();
    free(host_tcb.tls[1]);
    host_tcb.tls[1] = NULL;
    puts("WinCE runtime synchronization, views and Winsock passed");
    return 0;
}
