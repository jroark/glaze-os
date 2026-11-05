/*
 * Dial-up networking over a PC Card modem (GZ_NET_MODEM): the network
 * server dials with Hayes AT commands and runs PPP (lwIP's PPPoS, PAP or
 * CHAP-MD5) once the modem reports CONNECT. Settings → Dial-up edits the
 * saved configuration (DIALUP_PREF) and connects through these calls.
 */
#ifndef GLAZE_DIALUP_H
#define GLAZE_DIALUP_H
#include <stdint.h>

#define DIALUP_PREF         "net.dialup"
#define DIALUP_VERSION      1
#define DIALUP_NUMBER_MAX   32
#define DIALUP_USER_MAX     64
#define DIALUP_PASSWORD_MAX 64
#define DIALUP_INIT_MAX     48
#define DIALUP_DEFAULT_INIT "ATE0V1&C1&D2"

/* Strings are NUL-terminated within their arrays. */
struct dialup_config {
    uint32_t version;                       /* DIALUP_VERSION */
    char number[DIALUP_NUMBER_MAX + 1];     /* digits, ',', '*', '#', 'W', 'P' */
    char user[DIALUP_USER_MAX + 1];
    char password[DIALUP_PASSWORD_MAX + 1];
    char init[DIALUP_INIT_MAX + 1];         /* empty: DIALUP_DEFAULT_INIT */
    uint8_t pulse;                          /* ATDP instead of ATDT */
    uint8_t reconnect;                      /* dial when the modem appears */
    uint8_t reserved[2];
};

enum {
    DIALUP_IDLE,            /* modem present, not connected */
    DIALUP_INITIALIZING,    /* ATZ and the init string */
    DIALUP_DIALING,         /* ATD, waiting for CONNECT */
    DIALUP_NEGOTIATING,     /* CONNECT: PPP LCP/authentication/IPCP */
    DIALUP_CONNECTED,       /* PPP up, an address from the peer */
    DIALUP_HANGING_UP,
    DIALUP_FAILED,          /* error says why */
};

/* Failure reasons (dialup_status.error, negative like GZ_*). */
#define DIALUP_ENOMODEM     (-200)  /* no answer to AT */
#define DIALUP_EBUSY        (-201)  /* BUSY */
#define DIALUP_ENOCARRIER   (-202)  /* NO CARRIER, or carrier lost */
#define DIALUP_ENODIALTONE  (-203)
#define DIALUP_ENOANSWER    (-204)
#define DIALUP_EAUTH        (-205)  /* PPP authentication refused */
#define DIALUP_EPPP         (-206)  /* PPP negotiation failed */

struct dialup_status {
    uint32_t state;                 /* DIALUP_* */
    int32_t error;                  /* 0 or DIALUP_E*, GZ_E* */
    uint32_t connect_speed;         /* from "CONNECT <speed>", 0 if none */
    uint32_t addr, peer, dns;       /* while connected */
    uint32_t msr;                   /* modem lines (glaze/modem.h MODEM_*) */
    uint32_t configured;            /* a saved configuration exists */
    char modem[48];                 /* the card's identification */
    char response[32];              /* the modem's last result line */
    char number[DIALUP_NUMBER_MAX + 1];
};

int net_dialup_status(struct dialup_status *status);
/* Saves config (with reconnect set) and dials. */
int net_dialup_connect(const struct dialup_config *config);
/* Hangs up and clears reconnect, keeping the configuration. */
int net_dialup_disconnect(void);
/* Hangs up and deletes the saved configuration. */
int net_dialup_forget(void);
/* Reads the saved configuration, the password emptied. */
int net_dialup_config(struct dialup_config *config);

#endif
