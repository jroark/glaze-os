/*
 * Gmail: internal interfaces shared between main.c, conn.c, and sound.c.
 */
#ifndef GMAIL_GMAIL_H
#define GMAIL_GMAIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- conn.c: background TCP + optional wolfSSL channels ----------------- */

enum {
    CONN_CH_IMAP = 0,
    CONN_CH_SMTP = 1,
    CONN_CH_COUNT = 2,
};

enum {
    CONN_NONE = 0,
    CONN_CONNECTING,
    CONN_UP,
    CONN_FAILED,
};

void conn_start(int ch, const char *host, uint16_t port, bool use_tls);
int conn_state(int ch);
const char *conn_error(int ch);
int conn_send(int ch, const void *buf, size_t len);
/* Bytes read (> 0), 0 if nothing is waiting, < 0 once closed or broken. */
int conn_recv(int ch, void *buf, size_t len);
void conn_close(int ch);

/* ---- sound.c: bundle WAV effects ---------------------------------------- */

enum {
    SND_NEW_MAIL = 0,
    SND_MAIL_SENT,
    SND_COUNT,
};

void sound_play(int snd);
bool sound_pump(void);

#endif
