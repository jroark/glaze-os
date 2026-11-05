/*
 * Dial-up networking: a Hayes-compatible PC Card modem (GZ_NET_MODEM, the
 * kernel's kernel/drivers/modem.c) dialled with AT commands, then lwIP's
 * PPP over serial (PPPoS) with PAP or CHAP-MD5, the peer's address and DNS.
 *
 * The kernel moves the serial byte stream through SYS_NET_SEND/RECV and
 * takes the modem-line operations (DTR, status) through SYS_WIFI_CTL
 * (glaze/modem.h). The main thread runs the dialer from net_if_poll: it
 * writes commands, splits the modem's replies into result lines (ITU-T
 * V.250 result codes), and after CONNECT feeds every byte to pppos_input
 * under the tcpip core lock. lwIP's link status callback runs in the tcpip
 * thread and only records the outcome for the main thread.
 *
 * The configuration (glaze/dialup.h) is the net.dialup preference; with
 * reconnect set the server dials when the modem appears, and again after
 * the line drops, with the same backoff as Wi-Fi (2 s up to 30 s).
 */
#include <glaze/sys.h>
#include <glaze/modem.h>
#include <glaze/dialup.h>
#include <glaze/prefs.h>
#include <glaze/fs.h>
#include <string.h>

#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/dns.h"
#include "netif/ppp/pppos.h"
#include "server.h"
#include "modem.h"

#define RESET_MS        3000u   /* ATZ answers */
#define COMMAND_MS      3000u
#define DIAL_MS         90000u  /* ATD to CONNECT: dialling, training */
#define PPP_MS          45000u  /* CONNECT to IPCP up */
#define HANGUP_MS       1200u   /* DTR held low */
#define RETRY_MIN_MS    2000u
#define RETRY_MAX_MS    30000u

enum { STEP_NONE, STEP_RESET, STEP_INIT, STEP_DIAL };

static struct dialup_config config;
static struct dialup_status status;
static struct modem_status lines;
static struct netif ppp_if;
static ppp_pcb *ppp;
static volatile int ppp_result = -1;    /* PPPERR_*, from the tcpip thread */
static volatile bool ppp_up_flag;
static bool attached, have_config;
static int step;
static uint32_t step_started, retry_at, retry_delay = RETRY_MIN_MS;
static char line[96];
static unsigned line_len;
static uint8_t io[1024];

static void set_state(unsigned s, int error)
{
    status.state = s;
    status.error = error;
    net_if_set_state(s == DIALUP_CONNECTED ? NET_UP :
                     s == DIALUP_IDLE || s == DIALUP_FAILED ? NET_DISCONNECTED : NET_ASSOCIATING);
}

static int write_all(const void *data, uint32_t len)
{
    const uint8_t *p = data;
    uint32_t start = gz_time_ms();
    while (len) {
        int n = gz_net_send(p, len);
        if (n < 0) return n;
        if (n == 0) {
            if (gz_time_ms() - start > 2000u) return GZ_ETIMEDOUT;
            gz_sleep_ms(5);
            continue;
        }
        p += n;
        len -= (uint32_t)n;
    }
    return 0;
}

static void command(const char *text, int next)
{
    write_all(text, (uint32_t)strlen(text));
    write_all("\r", 1);
    step = next;
    step_started = gz_time_ms();
    line_len = 0;
}

/* PPPoS output, in the tcpip thread. */
static u32_t ppp_output(ppp_pcb *pcb, const void *data, u32_t len, void *ctx)
{
    (void)pcb; (void)ctx;
    return write_all(data, len) ? 0 : len;
}

static void ppp_status(ppp_pcb *pcb, int err, void *ctx)
{
    (void)pcb; (void)ctx;
    if (err == PPPERR_NONE) ppp_up_flag = true;
    else { ppp_up_flag = false; ppp_result = err; }
}

static void hang_up(void)
{
    gz_wifi_ctl(MODEM_DTR, (void *)0);
    gz_sleep_ms(HANGUP_MS);
    gz_wifi_ctl(MODEM_DTR, (void *)1);
    step = STEP_NONE;
    line_len = 0;
}

static void stop_ppp(bool carrier_lost)
{
    if (!ppp) return;
    LOCK_TCPIP_CORE();
    ppp_close(ppp, carrier_lost);
    UNLOCK_TCPIP_CORE();
    /* ppp_close with nocarrier finishes at once; otherwise LCP terminate
     * takes a few seconds at most. */
    for (unsigned i = 0; i < 100 && ppp_result < 0; i++) gz_sleep_ms(50);
    LOCK_TCPIP_CORE();
    ppp_free(ppp);
    ppp = NULL;
    dns_setserver(0, IP_ADDR_ANY);
    UNLOCK_TCPIP_CORE();
    ppp_up_flag = false;
}

static void fail(int error, const char *why)
{
    net_log("dialup: %s (%d)\n", why, error);
    stop_ppp(true);
    status.state = DIALUP_HANGING_UP;
    hang_up();
    set_state(DIALUP_FAILED, error);
    memset(&status.addr, 0, 3 * sizeof(status.addr));
    /* No redial after a refused login or with no line plugged in: neither
     * gets better by itself (a real USR/Megahertz modem with no line
     * answered NO DIALTONE every 2-30 s). Busy, no answer and a dropped
     * line do. */
    if (config.reconnect && error != DIALUP_EAUTH && error != DIALUP_ENODIALTONE) {
        retry_at = gz_time_ms() + retry_delay;
        retry_delay = retry_delay * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : retry_delay * 2;
    }
}

static void dial(void)
{
    if (!attached || !config.number[0]) return;
    stop_ppp(true);
    retry_at = 0;
    memset(&status.addr, 0, 3 * sizeof(status.addr));
    status.connect_speed = 0;
    status.response[0] = 0;
    memcpy(status.number, config.number, sizeof(status.number));
    set_state(DIALUP_INITIALIZING, 0);
    gz_wifi_ctl(MODEM_DTR, (void *)1);
    net_log("dialup: dialling %s\n", config.number);
    command("ATZ", STEP_RESET);
}

static void start_ppp(void)
{
    LOCK_TCPIP_CORE();
    ppp_result = -1;
    ppp_up_flag = false;
    ppp = pppos_create(&ppp_if, ppp_output, ppp_status, NULL);
    if (ppp) {
        ppp_set_default(ppp);
        ppp_set_usepeerdns(ppp, 1);
        if (config.user[0])
            ppp_set_auth(ppp, PPPAUTHTYPE_ANY, config.user, config.password);
        netif_set_hostname(&ppp_if, "be300");
        if (ppp_connect(ppp, 0) != ERR_OK) { ppp_free(ppp); ppp = NULL; }
    }
    UNLOCK_TCPIP_CORE();
    if (!ppp) { fail(GZ_ENOMEM, "PPP couldn't start"); return; }
    set_state(DIALUP_NEGOTIATING, 0);
    step = STEP_NONE;
    step_started = gz_time_ms();
}

/* A result line from the modem while in command mode. */
static void result(const char *text)
{
    if (!text[0]) return;
    strlcpy(status.response, text, sizeof(status.response));
    if (!strcmp(text, "ATZ") || !strncmp(text, "AT", 2)) return;   /* echo */
    if (!strcmp(text, "OK")) {
        if (step == STEP_RESET) {
            command(config.init[0] ? config.init : DIALUP_DEFAULT_INIT, STEP_INIT);
        } else if (step == STEP_INIT) {
            char cmd[DIALUP_NUMBER_MAX + 8];
            strlcpy(cmd, config.pulse ? "ATDP" : "ATDT", sizeof(cmd));
            strlcat(cmd, config.number, sizeof(cmd));
            set_state(DIALUP_DIALING, 0);
            command(cmd, STEP_DIAL);
        }
        return;
    }
    if (!strncmp(text, "CONNECT", 7)) {
        unsigned speed = 0;
        for (const char *p = text + 7; *p == ' ' || (*p >= '0' && *p <= '9'); p++)
            if (*p != ' ') speed = speed * 10 + (unsigned)(*p - '0');
        status.connect_speed = speed;
        net_log("dialup: %s\n", text);
        start_ppp();
        return;
    }
    if (!strcmp(text, "BUSY")) fail(DIALUP_EBUSY, "line busy");
    else if (!strcmp(text, "NO CARRIER")) fail(DIALUP_ENOCARRIER, "no carrier");
    else if (!strcmp(text, "NO DIALTONE") || !strcmp(text, "NO DIAL TONE"))
        fail(DIALUP_ENODIALTONE, "no dial tone");
    else if (!strcmp(text, "NO ANSWER")) fail(DIALUP_ENOANSWER, "no answer");
    else if (!strcmp(text, "ERROR")) fail(GZ_EIO, step == STEP_INIT ? "init string refused" : "modem error");
}

static void command_bytes(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++) {
        char c = (char)p[i];
        if (c == '\r' || c == '\n') {
            line[line_len] = 0;
            if (line_len) result(line);
            line_len = 0;
            if (status.state == DIALUP_NEGOTIATING) {
                /* CONNECT handed the line to PPP: the rest is PPP. */
                LOCK_TCPIP_CORE();
                pppos_input(ppp, p + i + 1, n - i - 1);
                UNLOCK_TCPIP_CORE();
                return;
            }
        } else if (line_len + 1 < sizeof(line) && c >= ' ') {
            line[line_len++] = c;
        }
    }
}

static bool load_config(void)
{
    struct dialup_config c;
    if (pref_get(DIALUP_PREF, &c, sizeof(c)) != sizeof(c) || c.version != DIALUP_VERSION)
        return false;
    c.number[DIALUP_NUMBER_MAX] = c.user[DIALUP_USER_MAX] = 0;
    c.password[DIALUP_PASSWORD_MAX] = c.init[DIALUP_INIT_MAX] = 0;
    config = c;
    memset(&c, 0, sizeof(c));
    return true;
}

static int save_config(void)
{
    int rv = fs_write_file(PREFS_DIR "/" DIALUP_PREF, &config, sizeof(config));
    return rv == sizeof(config) ? 0 : rv < 0 ? rv : GZ_EIO;
}

void modem_attach(void)
{
    attached = true;
    memset(&status, 0, sizeof(status));
    if (gz_wifi_ctl(MODEM_STATUS, &lines) == 0)
        memcpy(status.modem, lines.identification, sizeof(status.modem));
    status.modem[sizeof(status.modem) - 1] = 0;
    have_config = load_config();
    status.configured = have_config;
    retry_delay = RETRY_MIN_MS;
    set_state(DIALUP_IDLE, 0);
    net_log("dialup: modem \"%s\" ready\n", status.modem);
    if (have_config && config.reconnect) dial();
}

void modem_detach(void)
{
    stop_ppp(true);
    attached = false;
    step = STEP_NONE;
    memset(&status, 0, sizeof(status));
    memset(&config, 0, sizeof(config));
    net_log("dialup: modem removed\n");
}

void modem_poll(void)
{
    if (!attached) return;
    int n;
    while ((n = gz_net_recv(io, sizeof(io))) > 0) {
        if (status.state == DIALUP_NEGOTIATING || status.state == DIALUP_CONNECTED) {
            LOCK_TCPIP_CORE();
            pppos_input(ppp, io, n);
            UNLOCK_TCPIP_CORE();
        } else {
            command_bytes(io, n);
        }
    }
    gz_wifi_ctl(MODEM_STATUS, &lines);
    status.msr = lines.msr;
    uint32_t now = gz_time_ms();
    if (status.state == DIALUP_NEGOTIATING && ppp_up_flag) {
        LOCK_TCPIP_CORE();
        status.addr = ip4_addr_get_u32(netif_ip4_addr(&ppp_if));
        status.peer = ip4_addr_get_u32(netif_ip4_gw(&ppp_if));
        status.dns = ip4_addr_get_u32(ip_2_ip4(dns_getserver(0)));
        UNLOCK_TCPIP_CORE();
        retry_delay = RETRY_MIN_MS;
        set_state(DIALUP_CONNECTED, 0);
        char a[16], d[16];
        net_log("dialup: PPP up, address %s, dns %s\n", net_ntoa(status.addr, a), net_ntoa(status.dns, d));
    }
    if ((status.state == DIALUP_NEGOTIATING || status.state == DIALUP_CONNECTED) && ppp_result >= 0) {
        int err = ppp_result;
        fail(err == PPPERR_AUTHFAIL ? DIALUP_EAUTH : err == PPPERR_CONNECT ? DIALUP_ENOCARRIER : DIALUP_EPPP,
             "PPP ended");
    } else if (status.state == DIALUP_CONNECTED && !(lines.msr & MODEM_DCD)) {
        /* The default init string's &C1 makes DCD follow the carrier. */
        fail(DIALUP_ENOCARRIER, "carrier lost");
    } else if (status.state == DIALUP_NEGOTIATING && now - step_started > PPP_MS) {
        fail(DIALUP_EPPP, "PPP negotiation timed out");
    } else if (step == STEP_RESET && now - step_started > RESET_MS) {
        fail(DIALUP_ENOMODEM, "modem doesn't answer AT");
    } else if (step == STEP_INIT && now - step_started > COMMAND_MS) {
        fail(DIALUP_ENOMODEM, "modem doesn't answer the init string");
    } else if (step == STEP_DIAL && now - step_started > DIAL_MS) {
        fail(DIALUP_ENOANSWER, "no connection");
    }
    if (retry_at && (int32_t)(now - retry_at) >= 0 && status.state == DIALUP_FAILED) {
        retry_at = 0;
        dial();
    }
}

int modem_request(int op, void *buf)
{
    if (!attached) return GZ_ENOENT;
    switch (op) {
    case NET_DIALUP_STATUS:
        status.configured = have_config;
        memcpy(buf, &status, sizeof(status));
        return 0;
    case NET_DIALUP_CONFIG: {
        struct dialup_config *out = buf;
        if (!have_config) return GZ_ENOENT;
        *out = config;
        memset(out->password, 0, sizeof(out->password));
        return 0;
    }
    case NET_DIALUP_CONNECT: {
        struct dialup_config *in = buf;
        in->number[DIALUP_NUMBER_MAX] = in->user[DIALUP_USER_MAX] = 0;
        in->password[DIALUP_PASSWORD_MAX] = in->init[DIALUP_INIT_MAX] = 0;
        if (in->version != DIALUP_VERSION || !in->number[0]) return GZ_EINVAL;
        for (const char *p = in->number; *p; p++)
            if (!strchr("0123456789,*#WPwp ", *p)) return GZ_EINVAL;
        for (const char *p = in->init; *p; p++)
            if (*p < ' ' || *p > '~') return GZ_EINVAL;
        bool keep_password = !in->password[0] && have_config && !strcmp(in->user, config.user);
        char password[DIALUP_PASSWORD_MAX + 1];
        memcpy(password, config.password, sizeof(password));
        config = *in;
        if (keep_password) memcpy(config.password, password, sizeof(password));
        memset(password, 0, sizeof(password));
        memset(in->password, 0, sizeof(in->password));
        config.reconnect = 1;
        int rv = save_config();
        if (rv) return rv;
        have_config = true;
        retry_delay = RETRY_MIN_MS;
        dial();
        return 0;
    }
    case NET_DIALUP_DISCONNECT:
    case NET_DIALUP_FORGET:
        config.reconnect = 0;
        retry_at = 0;
        if (status.state != DIALUP_IDLE) {
            stop_ppp(false);
            status.state = DIALUP_HANGING_UP;
            hang_up();
        }
        memset(&status.addr, 0, 3 * sizeof(status.addr));
        set_state(DIALUP_IDLE, 0);
        if (op == NET_DIALUP_FORGET) {
            memset(&config, 0, sizeof(config));
            have_config = false;
            int rv = pref_delete(DIALUP_PREF);
            return rv == GZ_ENOENT ? 0 : rv;
        }
        return have_config ? save_config() : 0;
    }
    return GZ_EINVAL;
}

void modem_fill_status(struct net_status *st)
{
    if (status.state != DIALUP_CONNECTED) return;
    st->addr = status.addr;
    st->mask = 0xFFFFFFFFu;
    st->gateway = status.peer;
    st->dns = status.dns;
}
