/* The Hayes dialer (servers/net/hayes.c) against scripted RC56LD replies,
 * as captured on a real BE-300 (docs/hardware/MODEM.md), and the dial
 * configuration checks in lib/libc/net.c. */
#include "../../servers/net/hayes.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int net_dial_config_validate(const struct net_dial_config *c);

static void feed(struct hayes *h, const char *s, uint32_t now)
{
    unsigned used = hayes_input(h, (const uint8_t *)s, (unsigned)strlen(s), now);
    /* Everything is taken while the dialer is still listening. */
    assert(used == strlen(s) || h->state == NET_DIAL_IDLE || h->state == NET_DIAL_NEGOTIATING);
}

static const char *next(struct hayes *h)
{
    const char *c = hayes_command(h);
    assert(c);
    return c;
}

/* Runs ATZ, setup and ATI3 with the card's echo; leaves the dial pending. */
static void start(struct hayes *h, const char *number, const char *init)
{
    struct net_dial_config c = {0};
    strcpy(c.number, number); strcpy(c.init, init);
    assert(!net_dial_config_validate(&c));
    hayes_start(h, &c, 0);
    assert(h->state == NET_DIAL_STARTING && !strcmp(next(h), "\rATZ\r") && !hayes_command(h));
    feed(h, "ATZ\r\r\nOK\r\n", 10);
    assert(!strcmp(next(h), "ATE0V1X4&D2&C1\r"));
    feed(h, "ATE0V1X4&D2&C1\r\r\nOK\r\n", 20);
    assert(!strcmp(next(h), "ATI3\r"));
    feed(h, "ATI3\r\r\nL27-01\r\n\r\nOK\r\n", 30);
    assert(!strcmp(h->info, "L27-01"));
    if (init[0]) {
        assert(!strncmp(next(h), "AT", 2));
        feed(h, "\r\nOK\r\n", 40);
    }
}

int main(void)
{
    uint32_t rate = 1;
    assert(hayes_classify("CONNECT 52000/V90", &rate) == HAYES_CONNECT && rate == 52000);
    assert(hayes_classify("CONNECT", &rate) == HAYES_CONNECT && rate == 0);
    assert(hayes_classify("NO DIALTONE", NULL) == HAYES_NO_DIALTONE);
    assert(hayes_classify("NO DIAL TONE", NULL) == HAYES_NO_DIALTONE);
    assert(hayes_classify("L27-01", NULL) == HAYES_NONE);

    struct hayes h;
    memset(&h, 0, sizeof(h));
    start(&h, "9,555-0100", "M0");
    assert(!strcmp(next(&h), "ATDT9,555-0100\r") && h.state == NET_DIAL_DIALING);
    /* CONNECT, then PPP bytes in the same read stay with the caller. */
    const char tail[] = "ATDT9,555-0100\r\r\nRING\r\nCONNECT 48000\r\n~\xff}#\xc0!";
    unsigned used = hayes_input(&h, (const uint8_t *)tail, sizeof(tail) - 1, 50);
    assert(h.state == NET_DIAL_NEGOTIATING && h.rate == 48000);
    assert(tail[used] == '\n' || tail[used] == '~');

    static const struct { const char *reply; int error; } failures[] = {
        { "BUSY", NET_DIAL_E_BUSY }, { "NO DIALTONE", NET_DIAL_E_NO_DIALTONE },
        { "NO CARRIER", NET_DIAL_E_NO_CARRIER }, { "NO ANSWER", NET_DIAL_E_NO_ANSWER },
        { "OK", NET_DIAL_E_MODEM },   /* "ATDT," on the real card */
    };
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        start(&h, "5550100", "");
        assert(!strcmp(next(&h), "ATDT5550100\r"));
        char reply[32]; snprintf(reply, sizeof(reply), "\r\n%s\r\n", failures[i].reply);
        feed(&h, reply, 60);
        assert(h.state == NET_DIAL_IDLE && h.error == failures[i].error);
    }

    /* A setup command refused: the init string. */
    start(&h, "5550100", "");
    struct net_dial_config c = { .number = "1", .init = "&Q9" };
    hayes_start(&h, &c, 0);
    next(&h); feed(&h, "OK\r", 1); next(&h); feed(&h, "OK\r", 2); next(&h); feed(&h, "OK\r", 3);
    assert(!strcmp(next(&h), "AT&Q9\r"));
    feed(&h, "ERROR\r", 4);
    assert(h.state == NET_DIAL_IDLE && h.error == NET_DIAL_E_MODEM);

    /* No reply: ATZ is tried HAYES_TRIES times, then NO_REPLY. */
    hayes_start(&h, &c, 0);
    uint32_t now = 0;
    for (int i = 0; i < HAYES_TRIES; i++) {
        assert(!strcmp(next(&h), "\rATZ\r"));
        now += HAYES_RESET_MS;
        hayes_tick(&h, now);
    }
    assert(h.state == NET_DIAL_IDLE && h.error == NET_DIAL_E_NO_REPLY);

    /* The dial itself times out. */
    start(&h, "5550100", "");
    next(&h);
    hayes_tick(&h, 30 + HAYES_DIAL_MS - 1);   /* sent when ATI3 answered, at 30 */
    assert(h.state == NET_DIAL_DIALING);
    hayes_tick(&h, 30 + HAYES_DIAL_MS);
    assert(h.state == NET_DIAL_IDLE && h.error == NET_DIAL_E_TIMEOUT);

    /* Pulse dialing. */
    c = (struct net_dial_config){ .number = "123", .pulse = 1 };
    hayes_start(&h, &c, 0);
    next(&h); feed(&h, "OK\r", 1); next(&h); feed(&h, "OK\r", 2); next(&h); feed(&h, "x\rOK\r", 3);
    assert(!strcmp(next(&h), "ATDP123\r"));

    /* Configuration checks: nothing that ends the dial command. */
    struct net_dial_config bad = { .number = "123;ATH" };
    assert(net_dial_config_validate(&bad));
    bad = (struct net_dial_config){ .number = "123\rATZ" };
    assert(net_dial_config_validate(&bad));
    bad = (struct net_dial_config){ .number = "1", .init = "M0;H1" };
    assert(net_dial_config_validate(&bad));
    memset(&bad, 'x', sizeof(bad));
    assert(net_dial_config_validate(&bad));
    struct net_dial_config good = { .number = "*70, 1 (555) 010-0#", .user = "me", .password = "pw" };
    assert(!net_dial_config_validate(&good));
    puts("modem chat: result codes, echo, CONNECT with PPP bytes, failures, retries, timeouts, validation PASS");
}
