/* Native harness for the exact HTTP/TLS transport linked into both apps. */
#include "../apps/browser/net.h"
#include "../apps/browser/plat.h"
#include "../apps/browser/web.h"
#include <arpa/inet.h>
#include <glaze/types.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static uint32_t address;
static uint32_t seed_clock_advance;
static uint32_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
uint32_t plat_ms(void) { return wall_ms() + seed_clock_advance; }
int plat_resolve_start(const char *host)
{
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res))
        return WE_DNS;
    address = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return 0;
}
int plat_resolve_poll(int id, uint32_t *addr)
{
    (void)id;
    *addr = address;
    return 1;
}
void plat_resolve_cancel(int id)
{
    (void)id;
}
long long plat_time(void)
{
    return time(NULL);
}
void plat_noise(uint8_t *b, int n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(b, 1, n, f) != (size_t)n)
        abort();
    fclose(f);
}
int plat_load_seed(uint8_t *b)
{
    if (getenv("STORE_TEST_ENTROPY_FAIL")) return -1;
    if (getenv("STORE_TEST_ENTROPY_SLOW")) seed_clock_advance = 40000;
    plat_noise(b, PLAT_SEED_LEN);
    return 0;
}
void plat_save_seed(const uint8_t *b)
{
    (void)b;
}
const void *plat_roots(uint32_t *size)
{
    static unsigned char data[8192];
    *size = 0;
    const char *path = getenv("STORE_TEST_ROOTS");
    if (!path)
        return NULL;
    FILE *f = fopen(path, "rb");
    if (f) {
        *size = (uint32_t)fread(data, 1, sizeof(data), f);
        fclose(f);
    }
    return data;
}
static int sink(const uint8_t *data, int n, void *ctx)
{
    return fwrite(data, 1, n, ctx) == (size_t)n ? 0 : WE_PROTOCOL;
}
int main(int argc, char **argv)
{
    if (argc < 4)
        return 2;
    FILE *f = fopen(argv[2], "wb");
    if (!f)
        return 2;
    bool buffered = argc > 4 && !strcmp(argv[4], "buffered");
    bool cancel = argc > 4 && !strcmp(argv[4], "cancel");
    struct http *h = buffered ? http_start(argv[1], NULL, 0, NULL, NULL, atoi(argv[3]))
                              : http_start_stream(argv[1], atoi(argv[3]), sink, f);
    if (!h) {
        fclose(f);
        return 2;
    }
    int result = 0;
    uint32_t start = wall_ms();
    while (!(result = http_step(h)) && wall_ms() - start < 5000) {
        int got, total;
        http_progress(h, &got, &total);
        if (cancel && got > 0)
            break;
        usleep(1000);
    }
    int status = http_status(h);
    if (buffered && result == 1 && status == 200) {
        int n;
        uint8_t *data = http_take_body(h, &n);
        if (data) {
            fwrite(data, 1, n, f);
            free(data);
        } else
            result = WE_NOMEM;
    }
    http_free(h);
    fclose(f);
    if (cancel)
        return result == 0 ? 0 : 3;
    if (result != 1 || status != 200) {
        fprintf(stderr, "fetch: result %d HTTP %d\n", result, status);
        return 1;
    }
    return 0;
}
