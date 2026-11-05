/* Hayes dialing state machine (hayes.h). */
#include "hayes.h"
#include <string.h>
#if !defined(GLAZE_MODEM) || GLAZE_MODEM     /* host tests build it too */

/* Steps of an attempt; each waits for a final result code. */
enum { STEP_RESET, STEP_SETUP, STEP_INFO, STEP_INIT, STEP_DIAL };

static bool starts(const char *line, const char *word)
{
    return !strncmp(line, word, strlen(word));
}

enum hayes_result hayes_classify(const char *line, uint32_t *rate)
{
    if (!strcmp(line, "OK")) return HAYES_OK;
    if (starts(line, "CONNECT")) {
        uint32_t r = 0;
        for (const char *p = line + 7; *p == ' ' || (*p >= '0' && *p <= '9'); p++)
            if (*p != ' ' && r < 100000000u) r = r * 10 + (uint32_t)(*p - '0');
        if (rate) *rate = r;
        return HAYES_CONNECT;
    }
    if (!strcmp(line, "RING")) return HAYES_RING;
    if (!strcmp(line, "NO CARRIER")) return HAYES_NO_CARRIER;
    if (!strcmp(line, "ERROR")) return HAYES_ERROR;
    if (!strcmp(line, "NO DIALTONE") || !strcmp(line, "NO DIAL TONE")) return HAYES_NO_DIALTONE;
    if (!strcmp(line, "BUSY")) return HAYES_BUSY;
    if (!strcmp(line, "NO ANSWER")) return HAYES_NO_ANSWER;
    return HAYES_NONE;
}

static void send_step(struct hayes *h, int step, uint32_t now)
{
    h->step = step;
    h->len = 0;
    h->deadline = now + (step == STEP_DIAL ? HAYES_DIAL_MS
                         : step == STEP_RESET ? HAYES_RESET_MS : HAYES_SETUP_MS);
    switch (step) {
    case STEP_RESET:
        /* A leading CR ends any half-typed command left in the modem. */
        strcpy(h->command, "\rATZ\r");
        break;
    case STEP_SETUP:
        /* Echo off, verbose results, extended result codes (CONNECT rate),
         * hang up when DTR drops, DCD follows the carrier. */
        strcpy(h->command, "ATE0V1X4&D2&C1\r");
        break;
    case STEP_INFO:
        strcpy(h->command, "ATI3\r");
        break;
    case STEP_INIT:
        strcpy(h->command, h->init);
        strcat(h->command, "\r");
        break;
    case STEP_DIAL:
        strcpy(h->command, h->pulse ? "ATDP" : "ATDT");
        strcat(h->command, h->number);
        strcat(h->command, "\r");
        h->state = NET_DIAL_DIALING;
        break;
    }
    h->pending = true;
}

static void fail(struct hayes *h, int error)
{
    h->state = NET_DIAL_IDLE;
    h->error = error;
    h->pending = false;
}

void hayes_start(struct hayes *h, const struct net_dial_config *c, uint32_t now)
{
    void (*trace)(char, const char *) = h->trace;
    memset(h, 0, sizeof(*h));
    h->trace = trace;
    strcpy(h->number, c->number);
    h->pulse = c->pulse != 0;
    if (c->init[0]) {
        /* "M0" and "ATM0" both mean ATM0. */
        if (strncmp(c->init, "AT", 2) && strncmp(c->init, "at", 2)) strcpy(h->init, "AT");
        strcat(h->init, c->init);
    }
    h->state = NET_DIAL_STARTING;
    h->tries = 1;
    send_step(h, STEP_RESET, now);
}

const char *hayes_command(struct hayes *h)
{
    if (!h->pending) return NULL;
    h->pending = false;
    if (h->trace) h->trace('>', h->command);
    /* The echo, if any, comes back without the CRs. The RC56LD echoes
     * even after ATE0, so echoes are recognised rather than turned off. */
    unsigned n = 0;
    for (const char *p = h->command; *p && n + 1 < sizeof(h->sent); p++)
        if (*p != '\r') h->sent[n++] = *p;
    h->sent[n] = 0;
    return h->command;
}

static void result(struct hayes *h, enum hayes_result r, uint32_t now)
{
    if (r == HAYES_NONE || r == HAYES_RING) return;
    if (h->step != STEP_DIAL) {
        if (r != HAYES_OK) {
            fail(h, NET_DIAL_E_MODEM);
            return;
        }
        int next = h->step + 1;
        if (next == STEP_INIT && !h->init[0]) next = STEP_DIAL;
        send_step(h, next, now);
        return;
    }
    switch (r) {
    case HAYES_CONNECT: h->state = NET_DIAL_NEGOTIATING; break;
    case HAYES_BUSY: fail(h, NET_DIAL_E_BUSY); break;
    case HAYES_NO_DIALTONE: fail(h, NET_DIAL_E_NO_DIALTONE); break;
    case HAYES_NO_ANSWER: fail(h, NET_DIAL_E_NO_ANSWER); break;
    case HAYES_NO_CARRIER: fail(h, NET_DIAL_E_NO_CARRIER); break;
    default: fail(h, NET_DIAL_E_MODEM); break;
    }
}

unsigned hayes_input(struct hayes *h, const uint8_t *p, unsigned n, uint32_t now)
{
    unsigned i = 0;
    while (i < n && (h->state == NET_DIAL_STARTING || h->state == NET_DIAL_DIALING)) {
        uint8_t c = p[i++];
        if (c != '\r' && c != '\n') {
            if (h->len + 1 < sizeof(h->line) && c >= 32 && c < 127) h->line[h->len++] = (char)c;
            continue;
        }
        if (!h->len) continue;
        h->line[h->len] = 0;
        h->len = 0;
        if (h->trace) h->trace('<', h->line);
        if (!strcmp(h->line, h->sent)) continue;
        enum hayes_result r = hayes_classify(h->line, &h->rate);
        if (h->step == STEP_INFO && r == HAYES_NONE && !h->info[0])
            strncpy(h->info, h->line, sizeof(h->info) - 1);
        /* The CONNECT line ends at its CR; a following LF isn't PPP data
         * (PPP frames start with 0x7e) but stays harmless to pass on. */
        result(h, r, now);
    }
    return i;
}

void hayes_tick(struct hayes *h, uint32_t now)
{
    if (h->state != NET_DIAL_STARTING && h->state != NET_DIAL_DIALING) return;
    if ((int32_t)(now - h->deadline) < 0) return;
    if (h->step == STEP_RESET && h->tries < HAYES_TRIES) {
        h->tries++;
        send_step(h, STEP_RESET, now);
        return;
    }
    fail(h, h->step == STEP_RESET ? NET_DIAL_E_NO_REPLY
            : h->step == STEP_DIAL ? NET_DIAL_E_TIMEOUT : NET_DIAL_E_MODEM);
}
#endif
