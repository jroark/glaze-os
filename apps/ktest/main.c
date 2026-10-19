/* Kernel self-test: IPC round trips, notifications, sleep, shm, malloc, faults,
 * the key/value store, the file server and, with an NE2000, the network. */

#include <glaze/fs.h>
#include <glaze/net.h>
#include <glaze/sys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

/* Apps don't get raw flash. */
static bool fs_raw_denied(void)
{
    static uint8_t page[GZ_FLASH_PAGE_SIZE];

    return gz_flash_read(0, page) == GZ_EPERM && gz_flash_erase(0) == GZ_EPERM;
}

#define CHECK(cond, what) do { \
    if (cond) printf("ktest: PASS %s\n", what); \
    else { printf("ktest: FAIL %s\n", what); failures++; } \
} while (0)

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
    fs_remove("/ktest/sub");
    fs_remove("/ktest/a.bin");
    fs_remove("/ktest/c.bin");
    fs_remove("/ktest");
    CHECK(fs_mkdir("/ktest") == 0 && fs_mkdir("/ktest/sub") == 0, "fs mkdir");
    CHECK(fs_mkdir("/ktest") == GZ_EEXIST, "fs mkdir existing");
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

    /* KV store: round trip, overwrite, delete, and a persistent counter */
    static char big[12000];
    for (uint32_t i = 0; i < sizeof(big); i++)
        big[i] = (char)('a' + i % 26);
    CHECK(gz_kv_put("ktest.big", big, sizeof(big)) == 0, "kv_put 12000 bytes");
    static char back[12000];
    CHECK(gz_kv_get("ktest.big", back, sizeof(back)) == (int)sizeof(big) &&
          !memcmp(big, back, sizeof(big)), "kv_get round trip");
    gz_kv_put("ktest.small", "one", 3);
    gz_kv_put("ktest.small", "two!", 4);
    char small[8] = { 0 };
    CHECK(gz_kv_get("ktest.small", small, sizeof(small)) == 4 && !memcmp(small, "two!", 4), "kv overwrite");
    gz_kv_put("ktest.small", NULL, GZ_KV_DELETE);
    CHECK(gz_kv_get("ktest.small", small, sizeof(small)) == GZ_ENOENT, "kv delete");
    uint32_t boots = 0;
    gz_kv_get("ktest.boots", &boots, sizeof(boots));
    boots++;
    gz_kv_put("ktest.boots", &boots, sizeof(boots));
    printf("ktest: boot count %u (persists across runs)\n", boots);
    /* Churn enough to force a compaction (>1024 pages written). */
    for (int i = 0; i < 50; i++)
        gz_kv_put("ktest.big", big, sizeof(big));
    CHECK(gz_kv_get("ktest.big", back, sizeof(back)) == (int)sizeof(big) &&
          !memcmp(big, back, sizeof(big)), "kv survives compaction");
    uint32_t boots2 = 0;
    CHECK(gz_kv_get("ktest.boots", &boots2, sizeof(boots2)) == 4 && boots2 == boots, "kv counter after compaction");

    test_fs();
    test_net();

    struct gz_sysinfo si;
    gz_sysinfo(&si);
    printf("ktest: %u/%u pages free, %u procs\n", si.free_pages, si.total_pages, si.nprocs);

    printf("ktest: %s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    for (;;)
        gz_sleep_ms(1000);
}
