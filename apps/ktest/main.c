/* Kernel self-test: IPC round trips, notifications, sleep, shm, malloc, faults. */

#include <glaze/sys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, what) do { \
    if (cond) printf("ktest: PASS %s\n", what); \
    else { printf("ktest: FAIL %s\n", what); failures++; } \
} while (0)

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

    struct gz_sysinfo si;
    gz_sysinfo(&si);
    printf("ktest: %u/%u pages free, %u procs\n", si.free_pages, si.total_pages, si.nprocs);

    printf("ktest: %s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    for (;;)
        gz_sleep_ms(1000);
}
