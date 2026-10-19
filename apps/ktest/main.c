/* Kernel self-test: IPC round trips, notifications, sleep, shm, malloc, faults,
 * threads and semaphores, the key/value store, the file server, the audio
 * server and, with an NE2000, the network. */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <glaze/audio.h>
#include <glaze/fs.h>
#include <glaze/net.h>
#include <glaze/prefs.h>
#include <glaze/sys.h>
#include <glaze/thread.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

/* Apps don't get raw flash. */
static bool fs_raw_denied(void)
{
    static uint8_t page[GZ_FLASH_PAGE_SIZE];

    return gz_flash_read(0, page) == GZ_EPERM && gz_flash_erase(0) == GZ_EPERM &&
           gz_cf_read(0, 1, page) == GZ_EPERM;
}

#define CHECK(cond, what) do { \
    if (cond) printf("ktest: PASS %s\n", what); \
    else { printf("ktest: FAIL %s\n", what); failures++; } \
} while (0)

/* ---- threads -------------------------------------------------------------- */

static int sem_go, sem_done;
static volatile int counter;
static struct gz_mutex counter_lock = GZ_MUTEX_INIT;
static volatile int thread_ok;

static void handoff(void *arg)
{
    if (gz_sem_wait(sem_go, 1000) == 0 && arg == (void *)0x1234)
        thread_ok = gz_thread_id();
    gz_sem_post(sem_done);
}

static void increment(void *arg)
{
    for (int i = 0; i < 2000; i++) {
        gz_mutex_lock(&counter_lock);
        gz_mutex_lock(&counter_lock);       /* recursive */
        int v = counter;
        if (i % 97 == 0)
            gz_yield();                     /* let others contend */
        counter = v + 1;
        gz_mutex_unlock(&counter_lock);
        gz_mutex_unlock(&counter_lock);
    }
    gz_sem_post(sem_done);
}

static void own_errno(void *arg)
{
    errno = (int)(uintptr_t)arg;
    gz_sleep_ms(20);
    if (errno == (int)(uintptr_t)arg)
        thread_ok++;
    gz_sem_post(sem_done);
}

static void churn(void *arg)
{
    uint32_t seed = (uint32_t)(uintptr_t)arg;
    bool ok = true;

    for (int i = 0; i < 300 && ok; i++) {
        seed = seed * 1103515245u + 12345u;
        size_t n = 16 + (seed >> 16) % 3000;
        uint8_t *p = malloc(n);
        if (!p) {
            ok = false;
            break;
        }
        memset(p, (int)(seed >> 24), n);
        if (i % 13 == 0)
            gz_yield();
        for (size_t k = 0; k < n; k++)
            ok &= p[k] == (uint8_t)(seed >> 24);
        free(p);
    }
    if (!ok)
        thread_ok = -1000;
    gz_sem_post(sem_done);
}

static void quick(void *arg)
{
    gz_sem_post(sem_done);
}

static void wait_forever(void *arg)
{
    *(volatile int *)arg = gz_sem_wait(sem_go, -1);
    gz_sem_post(sem_done);
}

static bool wait_n(int n, int timeout_ms)
{
    for (int i = 0; i < n; i++)
        if (gz_sem_wait(sem_done, timeout_ms))
            return false;
    return true;
}

static void test_threads(void)
{
    int s = gz_sem_create(0);
    CHECK(s > 0 && gz_sem_wait(s, 0) == GZ_ETIMEDOUT && gz_sem_post(s) == 0 && gz_sem_wait(s, 0) == 0,
          "sem try/post");
    uint32_t t0 = gz_time_ms();
    int rv = gz_sem_wait(s, 50);
    uint32_t dt = gz_time_ms() - t0;
    CHECK(rv == GZ_ETIMEDOUT && dt >= 50 && dt < 100, "sem wait timeout");
    gz_sem_destroy(s);
    CHECK(gz_sem_post(s) == GZ_EINVAL, "sem destroyed");

    sem_go = gz_sem_create(0);
    sem_done = gz_sem_create(0);
    int tid = gz_thread_create(handoff, (void *)0x1234);
    CHECK(tid > 0 && tid != gz_thread_id(), "thread create");
    gz_sem_post(sem_go);
    CHECK(gz_sem_wait(sem_done, 1000) == 0 && thread_ok == tid, "thread handoff");

    counter = 0;
    t0 = gz_time_ms();
    for (int i = 0; i < 4; i++)
        gz_thread_create(increment, NULL);
    CHECK(wait_n(4, 5000) && counter == 8000, "mutex under contention (4 threads)");
    printf("ktest: 8000 locked increments in %u ms\n", gz_time_ms() - t0);

    thread_ok = 0;
    errno = 7;
    for (int i = 0; i < 3; i++)
        gz_thread_create(own_errno, (void *)(uintptr_t)(100 + i));
    CHECK(wait_n(3, 1000) && thread_ok == 3 && errno == 7, "errno per thread");

    thread_ok = 0;
    for (int i = 0; i < 4; i++)
        gz_thread_create(churn, (void *)(uintptr_t)(i + 1));
    CHECK(wait_n(4, 10000) && thread_ok == 0, "malloc from 4 threads");

    /* More threads than stack slots, one after another: slots are reused. */
    bool ok = true;
    for (int i = 0; i < 80 && ok; i++)
        ok = gz_thread_create(quick, NULL) > 0 && gz_sem_wait(sem_done, 1000) == 0;
    CHECK(ok, "thread exit and reuse x80");

    volatile int woke = 1;
    gz_thread_create(wait_forever, (void *)&woke);
    gz_sleep_ms(10);
    gz_sem_destroy(sem_go);
    CHECK(gz_sem_wait(sem_done, 1000) == 0 && woke == GZ_EDEAD, "sem destroy wakes waiters");
    sem_go = gz_sem_create(0);

    /* A process exiting with threads blocked on semaphores is reaped. */
    struct gz_sysinfo si0, si1;
    gz_sysinfo(&si0);
    int pong = gz_spawn("bin/ktest_pong", 0);
    int port;
    for (int i = 0; (port = gz_port_lookup("pong")) < 0 && i < 500; i++)
        gz_sleep_ms(2);
    struct gz_msg m, r;
    memset(&m, 0, sizeof(m));
    m.w[0] = 0xFFFFFFFC;
    rv = gz_send(port, &m, &r);
    for (int i = 0; gz_proc_alive(pong) && i < 100; i++)
        gz_sleep_ms(5);
    gz_sysinfo(&si1);
    printf("ktest: blocked-exit rv %d threads %u alive %d pages %u -> %u\n", rv, r.w[0], gz_proc_alive(pong),
           si0.free_pages, si1.free_pages);
    CHECK(rv == 0 && r.w[0] == 3 && !gz_proc_alive(pong) && si1.free_pages + 2 >= si0.free_pages,
          "exit with blocked threads");
}

static void test_fs(void)
{
    static uint8_t data[20000], back[20000];
    struct fs_dirent ents[8];
    struct fs_stat st;
    struct fs_info info;

    CHECK(fs_raw_denied(), "flash syscalls denied to apps");
    CHECK(fs_statfs(&info) == 0 && info.mounted == 0 && info.total > 4u << 20, "fs mounted");
    printf("ktest: fs %u KiB used of %u, %u bad blocks\n", info.used / 1024, info.total / 1024,
           info.bad_blocks);

    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 7 + i / 251);
    fs_remove("/ktest/sub/b.bin");
    fs_remove("/ktest/sub/deep");
    fs_remove("/ktest/sub");
    fs_remove("/ktest/a.bin");
    fs_remove("/ktest/c.bin");
    fs_remove("/ktest");
    CHECK(fs_mkdir("/ktest") == 0 && fs_mkdir("/ktest/sub") == 0, "fs mkdir");
    CHECK(fs_mkdir("/ktest") == GZ_EEXIST, "fs mkdir existing");
    CHECK(fs_mkdirs("/ktest/sub/deep") == 0 && fs_mkdirs("/ktest/sub/deep") == 0 &&
          fs_stat("/ktest/sub/deep", &st) == 0 && st.type == FS_DIR && fs_remove("/ktest/sub/deep") == 0,
          "fs mkdirs");
    CHECK(fs_write_file("/ktest/a.bin", data, sizeof(data)) == (int)sizeof(data), "fs write 20000 bytes");
    CHECK(fs_read_file("/ktest/a.bin", back, sizeof(back)) == (int)sizeof(back) &&
          !memcmp(data, back, sizeof(data)), "fs read back");
    CHECK(fs_stat("/ktest/a.bin", &st) == 0 && st.type == FS_FILE && st.size == sizeof(data), "fs stat file");
    CHECK(fs_stat("/ktest/sub", &st) == 0 && st.type == FS_DIR, "fs stat dir");
    CHECK(fs_stat("/ktest/none", &st) == GZ_ENOENT, "fs stat missing");

    int fd = fs_open("/ktest/a.bin", FS_O_READ | FS_O_WRITE);
    CHECK(fd >= 0 && fs_seek(fd, 10000, FS_SEEK_SET) == 10000 && fs_write(fd, "glaze", 5) == 5 &&
          fs_seek(fd, -5, FS_SEEK_CUR) == 10000 && fs_read(fd, back, 5) == 5 && !memcmp(back, "glaze", 5) &&
          fs_seek(fd, 0, FS_SEEK_END) == (int)sizeof(data) && fs_close(fd) == 0, "fs seek/overwrite");

    CHECK(fs_rename("/ktest/a.bin", "/ktest/sub/b.bin") == 0 && fs_stat("/ktest/a.bin", &st) == GZ_ENOENT,
          "fs rename");
    fs_write_file("/ktest/c.bin", "x", 1);
    int n = fs_list("/ktest", 0, ents, 8);
    CHECK(n == 2 && ((ents[0].type == FS_FILE && !strcmp(ents[0].name, "c.bin") && ents[1].type == FS_DIR) ||
                     (ents[1].type == FS_FILE && !strcmp(ents[1].name, "c.bin") && ents[0].type == FS_DIR)),
          "fs list");
    CHECK(fs_list("/ktest", 1, ents, 8) == 1, "fs list skip");
    CHECK(fs_remove("/ktest/sub") == GZ_ENOTEMPTY, "fs remove non-empty dir");
    CHECK(fs_remove("/ktest/sub/b.bin") == 0 && fs_remove("/ktest/sub") == 0 &&
          fs_remove("/ktest/c.bin") == 0 && fs_remove("/ktest") == 0, "fs remove");

    /* Throughput, for reference. */
    uint32_t t0 = gz_time_ms();
    fs_write_file("/ktest.bin", data, sizeof(data));
    uint32_t t1 = gz_time_ms();
    fs_read_file("/ktest.bin", back, sizeof(back));
    uint32_t t2 = gz_time_ms();
    fs_remove("/ktest.bin");
    printf("ktest: fs wrote 20000 bytes in %u ms, read in %u ms\n", t1 - t0, t2 - t1);
}

/* A CompactFlash card with a FAT volume, as /Card. Boot with --cf <image>
 * (tools/mkcfimg.sh) to run it; ktest leaves /Card/KTEST.TXT behind so the
 * image can be checked on the host after a clean shutdown. */
static void test_card(void)
{
    static uint8_t data[20000], back[20000];
    struct fs_dirent ents[16];
    struct fs_stat st;
    struct fs_info info;

    if (fs_stat("/Card", &st) < 0) {
        printf("ktest: SKIP card (no CF card; boot with --cf <image>)\n");
        return;
    }
    CHECK(st.type == FS_DIR, "card stat /Card");
    int n = fs_list("/", 0, ents, 16), found = 0;
    for (int i = 0; i < n; i++)
        found |= ents[i].type == FS_DIR && !strcmp(ents[i].name, "Card");
    CHECK(found, "card listed in /");
    CHECK(fs_statvfs("/Card", &info) == 0 && info.mounted == 0 && info.total > 0 &&
          info.used <= info.total && info.flash_blocks == 0, "card statvfs");
    printf("ktest: card %u KiB used of %u\n", info.used / 1024, info.total / 1024);

    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 13 + i / 509);
    fs_remove("/Card/ktest dir/Long file name.bin");
    fs_remove("/Card/ktest dir/moved.bin");
    fs_remove("/Card/ktest dir");
    CHECK(fs_mkdir("/Card/ktest dir") == 0 && fs_mkdir("/Card/ktest dir") == GZ_EEXIST, "card mkdir");
    CHECK(fs_write_file("/Card/ktest dir/Long file name.bin", data, sizeof(data)) == (int)sizeof(data) &&
          fs_read_file("/Card/ktest dir/Long file name.bin", back, sizeof(back)) == (int)sizeof(back) &&
          !memcmp(data, back, sizeof(data)), "card write/read 20000 bytes");
    CHECK(fs_stat("/Card/ktest dir/Long file name.bin", &st) == 0 && st.type == FS_FILE &&
          st.size == sizeof(data), "card stat file");

    int fd = fs_open("/Card/ktest dir/Long file name.bin", FS_O_READ | FS_O_WRITE);
    CHECK(fd >= 0 && fs_seek(fd, 10000, FS_SEEK_SET) == 10000 && fs_write(fd, "glaze", 5) == 5 &&
          fs_seek(fd, -5, FS_SEEK_CUR) == 10000 && fs_read(fd, back, 5) == 5 && !memcmp(back, "glaze", 5) &&
          fs_seek(fd, 0, FS_SEEK_END) == (int)sizeof(data) && fs_close(fd) == 0, "card seek/overwrite");
    fd = fs_open("/Card/ktest dir/Long file name.bin", FS_O_WRITE | FS_O_APPEND);
    CHECK(fd >= 0 && fs_seek(fd, 0, FS_SEEK_SET) == 0 && fs_write(fd, "end", 3) == 3 && fs_close(fd) == 0 &&
          fs_stat("/Card/ktest dir/Long file name.bin", &st) == 0 && st.size == sizeof(data) + 3, "card append");
    fd = fs_open("/Card/ktest dir/Long file name.bin", FS_O_WRITE | FS_O_TRUNC);
    CHECK(fd >= 0 && fs_close(fd) == 0 && fs_stat("/Card/ktest dir/Long file name.bin", &st) == 0 &&
          st.size == 0, "card truncate");

    CHECK(fs_rename("/Card/ktest dir/Long file name.bin", "/Card/ktest dir/moved.bin") == 0 &&
          fs_stat("/Card/ktest dir/Long file name.bin", &st) == GZ_ENOENT, "card rename");
    CHECK(fs_rename("/Card/ktest dir/moved.bin", "/moved.bin") == GZ_EINVAL, "card no cross-volume rename");
    CHECK(fs_remove("/Card") == GZ_EPERM && fs_mkdir("/Card") == GZ_EEXIST, "card mount point protected");
    n = fs_list("/Card/ktest dir", 0, ents, 16);
    CHECK(n == 1 && !strcmp(ents[0].name, "moved.bin"), "card list");
    CHECK(fs_remove("/Card/ktest dir") == GZ_ENOTEMPTY, "card remove non-empty dir");
    CHECK(fs_remove("/Card/ktest dir/moved.bin") == 0 && fs_remove("/Card/ktest dir") == 0 &&
          fs_stat("/Card/ktest dir", &st) == GZ_ENOENT, "card remove");

    uint32_t t0 = gz_time_ms();
    fs_write_file("/Card/ktest.bin", data, sizeof(data));
    uint32_t t1 = gz_time_ms();
    fs_read_file("/Card/ktest.bin", back, sizeof(back));
    uint32_t t2 = gz_time_ms();
    fs_remove("/Card/ktest.bin");
    printf("ktest: card wrote 20000 bytes in %u ms, read in %u ms\n", t1 - t0, t2 - t1);
    CHECK(fs_write_file("/Card/KTEST.TXT", "written by ktest\n", 17) == 17, "card leave KTEST.TXT");
}

static char *first_line(char *s)
{
    s[strcspn(s, "\r\n")] = 0;
    return s;
}

static int http_get(uint32_t addr, uint16_t port, const char *host, char *out, uint32_t max)
{
    char req[128];
    int s = net_socket(NET_TCP), rv, n = 0;

    if (s < 0)
        return s;
    rv = net_connect(s, addr, port, 3000);
    if (rv == 0) {
        snprintf(req, sizeof(req), "GET / HTTP/1.0\r\nHost: %s\r\n\r\n", host);
        rv = net_send(s, req, (uint32_t)strlen(req));
    }
    while (rv >= 0 && (uint32_t)n < max - 1) {
        rv = net_recv(s, out + n, max - 1 - (uint32_t)n, 5000);
        if (rv <= 0)
            break;
        n += rv;
    }
    out[n] = 0;
    net_close(s);
    return rv < 0 ? rv : n;
}

static const char *strerror_or(int e)
{
    static char b[16];

    snprintf(b, sizeof(b), "errno %d", e);
    return b;
}

/* Preferences: files under /Library/Preferences, and a persistent counter. */
static void test_prefs(void)
{
    struct fs_dirent ents[16];
    char small[8] = { 0 };
    uint32_t boots = 0, boots2 = 0;

    CHECK(pref_put("ktest.small", "one", 3) == 0 && pref_put("ktest.small", "two!", 4) == 0 &&
          pref_get("ktest.small", small, sizeof(small)) == 4 && !memcmp(small, "two!", 4), "pref overwrite");
    int n = fs_list(PREFS_DIR, 0, ents, 16);
    bool listed = false;
    for (int i = 0; i < n; i++)
        listed |= !strcmp(ents[i].name, "ktest.small");
    CHECK(listed, "pref is a file in " PREFS_DIR);
    CHECK(pref_delete("ktest.small") == 0 && pref_get("ktest.small", small, sizeof(small)) == GZ_ENOENT,
          "pref delete");
    CHECK(pref_get("no/such", small, sizeof(small)) == GZ_EINVAL, "pref key with a slash rejected");
    pref_get("ktest.boots", &boots, sizeof(boots));
    boots++;
    CHECK(pref_put("ktest.boots", &boots, sizeof(boots)) == 0 &&
          pref_get("ktest.boots", &boots2, sizeof(boots2)) == 4 && boots2 == boots, "pref counter");
    printf("ktest: boot count %u (persists across runs)\n", boots);
}

/* BSD sockets (libc) over the server's lwIP sockets, to ourselves. */
static void test_bsd(uint32_t self)
{
    struct sockaddr_in sa, peer;
    socklen_t len = sizeof(sa);
    char buf[64];
    int one = 1;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(7010);
    int l = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(l >= 0 && setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0 &&
          bind(l, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(l, 4) == 0 &&
          getsockname(l, (struct sockaddr *)&sa, &len) == 0 && ntohs(sa.sin_port) == 7010, "bsd listen");

    int c = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) | O_NONBLOCK);
    sa.sin_addr.s_addr = self;
    int rv = connect(c, (struct sockaddr *)&sa, sizeof(sa));
    CHECK(rv == -1 && errno == EINPROGRESS, "bsd non-blocking connect");
    struct pollfd pf = { c, POLLOUT, 0 };
    int err = -1;
    len = sizeof(err);
    CHECK(poll(&pf, 1, 2000) == 1 && (pf.revents & POLLOUT) &&
          getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0, "bsd poll for connect");
    fcntl(c, F_SETFL, 0);
    len = sizeof(peer);
    int a = accept(l, (struct sockaddr *)&peer, &len);
    CHECK(a >= 0 && peer.sin_addr.s_addr == self, "bsd accept");
    CHECK(send(c, "ping", 4, 0) == 4 && recv(a, buf, sizeof(buf), 0) == 4 && !memcmp(buf, "ping", 4),
          "bsd send/recv");
    CHECK(recv(a, buf, sizeof(buf), MSG_DONTWAIT) == -1 && errno == EWOULDBLOCK, "bsd recv MSG_DONTWAIT");
    struct timeval tv = { 0, 100000 };
    setsockopt(a, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint32_t t0 = gz_time_ms();
    rv = recv(a, buf, sizeof(buf), 0);
    uint32_t dt = gz_time_ms() - t0;
    CHECK(rv == -1 && errno == EAGAIN && dt >= 100 && dt < 400, "bsd SO_RCVTIMEO");
    pf = (struct pollfd){ a, POLLIN, 0 };
    CHECK(poll(&pf, 1, 50) == 0, "bsd poll timeout");
    write(c, "bye", 3);
    CHECK(read(a, buf, sizeof(buf)) == 3, "bsd read/write");
    close(c);
    tv.tv_sec = 2;
    setsockopt(a, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    CHECK(recv(a, buf, sizeof(buf), 0) == 0, "bsd end of stream");
    close(a);
    close(l);
    CHECK(close(l) == -1 && errno == EBADF, "bsd close twice");

    c = socket(AF_INET, SOCK_STREAM, 0);
    sa.sin_port = htons(7011);
    rv = connect(c, (struct sockaddr *)&sa, sizeof(sa));
    CHECK(rv == -1 && errno == ECONNREFUSED, "bsd connect refused");
    close(c);

    int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(7012);
    bind(u1, (struct sockaddr *)&sa, sizeof(sa));
    sa.sin_addr.s_addr = self;
    len = sizeof(peer);
    CHECK(sendto(u2, "hi", 2, 0, (struct sockaddr *)&sa, sizeof(sa)) == 2 &&
          recvfrom(u1, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &len) == 2 && !memcmp(buf, "hi", 2) &&
          peer.sin_addr.s_addr == self, "bsd udp sendto/recvfrom");
    CHECK(connect(u2, (struct sockaddr *)&sa, sizeof(sa)) == 0 && send(u2, "yo", 2, 0) == 2 &&
          recv(u1, buf, sizeof(buf), 0) == 2 && !memcmp(buf, "yo", 2), "bsd connected udp");
    close(u1);
    close(u2);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    char a1[16];
    CHECK(getaddrinfo("10.1.2.3", "http", &hints, &res) == 0 &&
          !strcmp(inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, a1, sizeof(a1)), "10.1.2.3") &&
          ntohs(((struct sockaddr_in *)res->ai_addr)->sin_port) == 80, "bsd getaddrinfo literal");
    freeaddrinfo(res);
    CHECK(getaddrinfo("localhost", "80", &hints, &res) == 0 &&
          ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr == htonl(INADDR_LOOPBACK), "bsd getaddrinfo name");
    freeaddrinfo(res);

    /* HTTP over BSD sockets, if the internet is reachable. */
    if (getaddrinfo("example.com", "80", &hints, &res) == 0) {
        int s = socket(res->ai_family, res->ai_socktype, 0);
        static char page[2048];
        int n = 0;
        if (connect(s, res->ai_addr, res->ai_addrlen) == 0) {
            const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
            send(s, req, strlen(req), 0);
            tv = (struct timeval){ 5, 0 };
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            while (n < (int)sizeof(page) - 1 && (rv = (int)recv(s, page + n, sizeof(page) - 1 - (size_t)n, 0)) > 0)
                n += rv;
        }
        page[n] = 0;
        page[strcspn(page, "\r\n")] = 0;
        printf("ktest: INFO bsd example.com: %s\n", n ? page : strerror_or(errno));
        close(s);
        freeaddrinfo(res);
    }
}

/* ---- audio ---------------------------------------------------------------- */

/* A 440 Hz triangle at 22050 Hz mono (integer only), continuing from *phase. */
static void tone(int16_t *out, int frames, uint32_t *phase)
{
    for (int i = 0; i < frames; i++) {
        uint32_t p = (*phase += 1308) & 0xFFFF;     /* 440 Hz: 65536 * 440 / 22050 */
        int v = p < 0x8000 ? (int)p * 2 - 0x8000 : 0x17FFF - (int)p * 2;
        out[i] = (int16_t)(v / 4);
    }
}

static void test_audio(void)
{
    static int16_t pcm[2048];
    struct audio_state st;
    uint32_t phase = 0, played = 0, queued = 0;

    CHECK(gz_audio_start(22050, 1) == GZ_EPERM && gz_audio_write(pcm, 4) == GZ_EPERM,
          "raw audio denied to apps");

    /* Stream mode: the clock must follow wall time with no underruns. */
    CHECK(audio_stream_open(22050, 1) == 0, "audio stream open");
    audio_get_state(&st);
    uint32_t under0 = st.underruns;
    int64_t t0 = -1;
    uint32_t p0 = 0;
    int pending = 0, off = 0;
    uint64_t start = gz_time_us();
    while (gz_time_us() - start < 3000000) {
        if (!pending) {
            tone(pcm, 2048, &phase);
            pending = 4096;
            off = 0;
        }
        int n = audio_stream_write((uint8_t *)pcm + off, (uint32_t)pending);
        if (n > 0) {
            off += n;
            pending -= n;
            continue;
        }
        audio_stream_clock(&played, &queued);
        if (t0 < 0 && played > 200) {
            t0 = (int64_t)gz_time_us();
            p0 = played;
        }
        gz_sleep_ms(20);
    }
    audio_stream_clock(&played, &queued);
    uint32_t wall = t0 < 0 ? 0 : (uint32_t)((gz_time_us() - (uint64_t)t0) / 1000);
    uint32_t heard = played - p0;
    audio_get_state(&st);
    printf("ktest: audio stream %u ms played in %u ms, %u ms queued, %u underruns\n",
           heard, wall, queued, st.underruns - under0);
    /* The DMA runs on the emulator host's clock while guest time follows the
     * emulated Count, so allow for drift between them. */
    CHECK(wall > 1000 && heard * 10 > wall * 6 && heard * 10 < wall * 14, "audio stream clock");
    CHECK(st.underruns == under0, "audio stream without underruns");
    CHECK(audio_stream_close() == 0, "audio stream close");

    /* Player mode on the factory MP3. */
    int rv = audio_play("/Music/Demo.mp3");
    CHECK(rv == 0, "audio play mp3");
    gz_sleep_ms(2000);
    audio_get_state(&st);
    printf("ktest: audio '%s' by '%s' at %u/%u ms, %u Hz %u ch %u kbps\n", st.title, st.artist,
           st.position_ms, st.duration_ms, st.rate, st.channels, st.bitrate);
    CHECK(st.state == AUDIO_PLAYING && st.position_ms > 1000 && st.position_ms < 3000 &&
          !strcmp(st.title, "Glaze Demo") && st.duration_ms > 19000, "audio mp3 playing");
    CHECK(audio_seek(15000) == 0, "audio seek");
    gz_sleep_ms(500);
    audio_get_state(&st);
    CHECK(st.position_ms >= 15000 && st.position_ms < 16500, "audio position after seek");
    audio_pause();
    audio_get_state(&st);
    uint32_t paused_at = st.position_ms;
    gz_sleep_ms(300);
    audio_get_state(&st);
    CHECK(st.state == AUDIO_PAUSED && st.position_ms == paused_at, "audio pause");
    CHECK(audio_play("/Music/Chime.wav") == 0, "audio play adpcm wav");
    gz_sleep_ms(1000);
    audio_get_state(&st);
    CHECK(st.state == AUDIO_PLAYING && st.position_ms > 300 && st.rate == 22050, "audio wav playing");
    audio_stop();
    audio_get_state(&st);
    CHECK(st.state == AUDIO_STOPPED, "audio stop");
}

static void test_net(void)
{
    static uint8_t frame[64], data[20000], back[20000];
    static char page[4096];
    struct net_status st;
    char a[16];

    CHECK(gz_net_send(frame, sizeof(frame)) == GZ_EPERM && gz_net_recv(frame, sizeof(frame)) == GZ_EPERM,
          "net syscalls denied to apps");
    CHECK(net_status(&st) == 0, "net status");
    if (st.state == NET_NO_CARD) {
        printf("ktest: SKIP network (no NE2000; boot with --ne2000)\n");
        CHECK(net_socket(NET_TCP) == GZ_ENETDOWN, "net down without a card");
        return;
    }
    CHECK(net_wait_up(15000) == 0 && net_status(&st) == 0 && st.addr, "net address");
    printf("ktest: net %s state %u\n", net_ntoa(st.addr, a), st.state);

    int rtt = net_ping(st.gateway, 3000);
    CHECK(rtt >= 0, "net ping router");
    CHECK(net_ping(st.addr, 3000) >= 0, "net ping self");

    uint32_t ip = 0;
    CHECK(net_resolve("localhost", &ip, 5000) == 0 && ip == NET_IP(127, 0, 0, 1), "net resolve via router");
    CHECK(net_resolve("10.1.2.3", &ip, 1000) == 0 && ip == NET_IP(10, 1, 2, 3), "net resolve literal");
    CHECK(net_aton("192.168.1.20", &ip) == 0 && ip == NET_IP(192, 168, 1, 20) &&
          net_aton("1.2.3", &ip) == GZ_EINVAL && net_aton("1.2.3.256", &ip) == GZ_EINVAL, "net aton");

    /* TCP to ourselves: the listener queues the connection, so one thread
     * can connect first and accept afterwards. */
    int l = net_socket(NET_TCP);
    CHECK(l >= 0 && net_bind(l, 7000) == 7000 && net_listen(l) == 0, "tcp listen");
    int c = net_socket(NET_TCP);
    CHECK(net_connect(c, st.addr, 7000, 3000) == 0, "tcp connect");
    uint32_t peer = 0;
    uint16_t peer_port = 0;
    int srv = net_accept(l, &peer, &peer_port, 1000);
    CHECK(srv >= 0 && peer == st.addr && peer_port, "tcp accept");
    for (uint32_t i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 7 + (i >> 8));
    bool ok = true;
    uint32_t got = 0, sent = 0;
    while (ok && got < sizeof(back)) {
        /* Send in slices so the receiver drains the window in between. */
        if (sent < sizeof(data)) {
            uint32_t n = MIN(sizeof(data) - sent, 3000u);
            ok = net_send(c, data + sent, n) == (int)n;
            sent += n;
        }
        int rv = net_recv(srv, back + got, sizeof(back) - got, 2000);
        ok = ok && rv > 0;
        got += rv > 0 ? (uint32_t)rv : 0;
    }
    CHECK(ok && got == sizeof(data) && !memcmp(data, back, sizeof(data)), "tcp 20000 bytes");
    uint32_t t1 = gz_time_ms();
    int rv = net_recv(srv, back, 16, 100);
    uint32_t dt = gz_time_ms() - t1;
    CHECK(rv == GZ_ETIMEDOUT && dt >= 100 && dt < 400, "tcp recv timeout");
    net_close(c);
    CHECK(net_recv(srv, back, 16, 2000) == 0, "tcp peer close");
    net_close(srv);
    CHECK(net_accept(l, NULL, NULL, 0) == GZ_ETIMEDOUT, "tcp accept timeout");
    c = net_socket(NET_TCP);
    CHECK(net_connect(c, st.addr, 7001, 3000) == GZ_ECONNREFUSED, "tcp connect refused");
    net_close(c);
    net_close(l);

    /* UDP to ourselves. */
    int u1 = net_socket(NET_UDP), u2 = net_socket(NET_UDP);
    CHECK(u1 >= 0 && u2 >= 0 && net_bind(u1, 7002) == 7002, "udp bind");
    CHECK(net_sendto(u2, "hello", 5, st.addr, 7002) == 5, "udp sendto");
    char dg[16] = { 0 };
    rv = net_recvfrom(u1, dg, sizeof(dg), &peer, &peer_port, 1000);
    CHECK(rv == 5 && !memcmp(dg, "hello", 5) && peer == st.addr, "udp recvfrom");
    CHECK(net_recvfrom(u1, dg, sizeof(dg), NULL, NULL, 50) == GZ_ETIMEDOUT, "udp recv timeout");
    net_close(u1);
    net_close(u2);

    test_bsd(st.addr);

    /* An HTTP/1.0 GET through the router; the wider internet may not be
     * reachable from where the emulator runs. */
    if (net_resolve("example.com", &ip, 5000) == 0) {
        rv = http_get(ip, 80, "example.com", page, sizeof(page));
        printf("ktest: INFO example.com %s: %s\n", net_ntoa(ip, a),
               rv > 0 ? first_line(page) : net_strerror(rv));
    } else {
        printf("ktest: INFO no internet DNS\n");
    }
}

int main(void)
{
    printf("ktest: hello from pid %d\n", gz_getpid());

    /* malloc */
    char *a = malloc(100), *b = malloc(200000);
    CHECK(a && b, "malloc");
    memset(b, 0x5A, 200000);
    CHECK(b[199999] == 0x5A, "malloc large write");
    free(a);
    free(b);

    /* sleep timing */
    uint32_t t0 = gz_time_ms();
    gz_sleep_ms(100);
    uint32_t dt = gz_time_ms() - t0;
    printf("ktest: slept %u ms\n", dt);
    CHECK(dt >= 100 && dt < 150, "sleep_ms(100)");

    /* IPC with a separate process */
    int pong = gz_spawn("bin/ktest_pong", 0);
    CHECK(pong > 0, "spawn pong");
    int port;
    while ((port = gz_port_lookup("pong")) < 0)
        gz_sleep_ms(1);

    struct gz_msg m, r;
    bool ok = true;
    t0 = gz_time_ms();
    for (uint32_t i = 0; i < 1000; i++) {
        memset(&m, 0, sizeof(m));
        m.w[0] = i;
        m.w[15] = ~i;
        if (gz_send(port, &m, &r) || r.w[0] != i + 1 || r.w[15] != ~i) {
            ok = false;
            break;
        }
    }
    dt = gz_time_ms() - t0;
    printf("ktest: 1000 IPC round trips in %u ms\n", dt);
    CHECK(ok, "ipc call/reply x1000");

    /* shared memory: pong writes a pattern into a buffer we create */
    int shm = gz_shm_create(64 * 1024);
    uint32_t *sp = gz_shm_map(shm);
    CHECK(shm > 0 && sp, "shm create+map");
    memset(&m, 0, sizeof(m));
    m.w[0] = 0xFFFFFFFF;
    m.w[1] = (uint32_t)shm;
    gz_send(port, &m, &r);
    CHECK(sp[0] == 0xC0FFEE && sp[16383] == 0xBEEF, "shm visible across processes");

    /* notification: pong notifies our port */
    int myport = gz_port_create();
    gz_port_register(myport, "ktest");
    m.w[0] = 0xFFFFFFFE;
    gz_send(port, &m, &r);
    int tok = gz_recv(myport, &m, 1000);
    CHECK(tok == GZ_RECV_NOTIFY && m.w[0] == 0x55, "notify");

    /* recv timeout */
    t0 = gz_time_ms();
    tok = gz_recv(myport, &m, 50);
    dt = gz_time_ms() - t0;
    CHECK(tok == GZ_ETIMEDOUT && dt >= 50 && dt < 100, "recv timeout");

    /* fault isolation: pong dereferences NULL on request; send fails EDEAD */
    m.w[0] = 0xFFFFFFFD;
    int rv = gz_send(port, &m, &r);
    CHECK(rv == GZ_EDEAD, "faulting peer reported dead");
    gz_sleep_ms(20);
    CHECK(!gz_proc_alive(pong), "faulting process reaped");

    /* RTC */
    struct gz_datetime now;
    CHECK(gz_rtc_get(&now) == 0 && now.year >= 2000, "rtc_get");
    printf("ktest: rtc %u-%02u-%02u %02u:%02u:%02u wd%u\n", now.year, now.month,
           now.day, now.hour, now.minute, now.second, now.weekday);

    test_threads();
    test_fs();
    test_card();
    test_prefs();
    test_audio();
    test_net();

    struct gz_sysinfo si;
    gz_sysinfo(&si);
    printf("ktest: %u/%u pages free, %u procs\n", si.free_pages, si.total_pages, si.nprocs);

    printf("ktest: %s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    for (;;)
        gz_sleep_ms(1000);
}
