/* SNTP client (glaze/ntp.h, RFC 4330). */

#include <glaze/net.h>
#include <glaze/notify.h>
#include <glaze/ntp.h>
#include <glaze/prefs.h>
#include <glaze/sys.h>
#include <glaze/tz.h>
#include <string.h>

#define NTP_PORT        123
#define NTP_TO_2000     3155673600u     /* seconds from 1900-01-01 to 2000-01-01 */

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* One request and its reply; the transmit time we send is a nonce the
 * server echoes as the originate time. */
static int exchange(int s, uint32_t addr, int timeout_ms, uint32_t *utc)
{
    uint8_t pkt[48];
    uint32_t nonce = gz_time_ms() ^ (uint32_t)gz_getpid() << 20, from;
    uint16_t port;

    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x23;                          /* LI 0, version 4, mode 3 (client) */
    put32(pkt + 40, nonce);
    put32(pkt + 44, ~nonce);
    int rv = net_sendto(s, pkt, sizeof(pkt), addr, NTP_PORT);
    if (rv < 0)
        return rv;
    uint32_t t0 = gz_time_ms();
    for (;;) {
        int left = timeout_ms - (int)(gz_time_ms() - t0);
        if (left <= 0)
            return GZ_ETIMEDOUT;
        rv = net_recvfrom(s, pkt, sizeof(pkt), &from, &port, left);
        if (rv < 0)
            return rv;
        if (rv < 48 || from != addr || port != NTP_PORT || get32(pkt + 24) != nonce || get32(pkt + 28) != ~nonce)
            continue;                       /* not the reply to this request */
        if ((pkt[0] & 7) != 4 || pkt[1] == 0 || pkt[1] > 15 || (pkt[0] >> 6) == 3)
            return GZ_EIO;                  /* not a server, kiss-o'-death, or unsynchronized */
        uint32_t secs = get32(pkt + 40), frac = get32(pkt + 44);
        if (!secs && !frac)
            return GZ_EIO;
        /* Half the round trip, plus rounding. Era wraps (2036) cancel out
         * in the unsigned subtraction. */
        uint32_t ms = (frac >> 22) * 1000 / 1024 + (gz_time_ms() - t0) / 2;
        *utc = secs - NTP_TO_2000 + (ms + 500) / 1000;
        return 0;
    }
}

int ntp_query(const char *host, int timeout_ms, uint32_t *utc)
{
    uint32_t addr;
    int rv = net_resolve(host, &addr, timeout_ms);

    if (rv < 0)
        return rv;
    int s = net_socket(NET_UDP);
    if (s < 0)
        return s;
    for (int tries = 0; tries < 3; tries++) {
        rv = exchange(s, addr, timeout_ms / 3, utc);
        if (rv != GZ_ETIMEDOUT)
            break;
    }
    net_close(s);
    return rv;
}

int ntp_sync(void)
{
    struct gz_datetime dt;
    uint32_t utc;
    int rv = net_wait_up(5000);

    if (rv == 0)
        rv = ntp_query(NTP_SERVER, 6000, &utc);
    if (rv < 0)
        return rv;
    uint32_t local = tz_utc_to_local(tz_current(), utc);
    gz_secs_to_dt(local, &dt);
    rv = gz_rtc_set(&dt);
    if (rv < 0)
        return rv;
    pref_put(TZ_PREF_SYNCED, &local, sizeof(local));
    return 0;
}
