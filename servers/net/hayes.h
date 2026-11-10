/* Hayes (AT) dialing for the CF modem: a byte-stream state machine with no
 * I/O of its own, so it is tested natively (tests/native/modem_chat.c).
 * Result codes are the verbose ones (ATV1) of the Rockwell RC56 command set. */
#ifndef GLAZE_HAYES_H
#define GLAZE_HAYES_H
#include <stdbool.h>
#include <stdint.h>
#include <glaze/net.h>

enum hayes_result {
    HAYES_NONE, HAYES_OK, HAYES_CONNECT, HAYES_RING, HAYES_NO_CARRIER, HAYES_ERROR,
    HAYES_NO_DIALTONE, HAYES_BUSY, HAYES_NO_ANSWER,
};
/* A result line's code; CONNECT's rate (bit/s, 0 if absent) goes to rate. */
enum hayes_result hayes_classify(const char *line, uint32_t *rate);

#define HAYES_RESET_MS      3000u
#define HAYES_SETUP_MS      3000u
#define HAYES_DIAL_MS       90000u
#define HAYES_TRIES         3
/* Pause before each command. The RC56LD takes a command right after its
 * last result code; the USR XJ/CC1560 drops one sent that soon (the echo
 * and reply never come), seen on a BE-300 through the jacket, 2026-11-09. */
#define HAYES_GAP_MS        250u

struct hayes {
    int state;              /* NET_DIAL_IDLE, _STARTING, _DIALING, _NEGOTIATING (CONNECT) */
    int error;              /* NET_DIAL_E_* once the attempt failed (state IDLE) */
    int step, tries;
    uint32_t deadline, rate;
    char command[NET_DIAL_NUMBER_MAX + NET_DIAL_INIT_MAX + 8];
    bool pending;           /* command[] is waiting to be sent */
    char sent[sizeof(((struct net_dial_config *)0)->init) + NET_DIAL_NUMBER_MAX + 8];   /* its echo */
    char line[96];
    unsigned len;
    char number[NET_DIAL_NUMBER_MAX], init[NET_DIAL_INIT_MAX + 4];
    bool pulse;
    char info[32];          /* the ATI3 answer */
    /* Optional: each command sent (">") and line received ("<"), for logs. */
    void (*trace)(char direction, const char *text);
};

/* Starts an attempt; the config must have passed net_dial_config_validate. */
/* Starts an attempt (trace survives); the config must have passed net_dial_config_validate. */
void hayes_start(struct hayes *h, const struct net_dial_config *c, uint32_t now);
/* The command to send next (ending in CR), or NULL; returning it clears it. */
const char *hayes_command(struct hayes *h);
/* Feeds modem bytes. Returns how many were taken: after CONNECT the rest
 * of the buffer is PPP data and is left to the caller. */
unsigned hayes_input(struct hayes *h, const uint8_t *p, unsigned n, uint32_t now);
/* Times out a step: retries ATZ, else fails the attempt. */
void hayes_tick(struct hayes *h, uint32_t now);
#endif
