/*
 * AIM: the parts of the app shared between its files.
 */
#ifndef AIM_AIM_H
#define AIM_AIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- conn.c: one TCP connection, opened in the background ----------------- */

enum { CONN_NONE, CONN_CONNECTING, CONN_UP, CONN_FAILED };

/* Starts resolving and connecting; any previous connection is closed. */
void conn_start(const char *host, uint16_t port);
/* CONN_*; once CONN_FAILED, conn_error() says why. */
int conn_state(void);
const char *conn_error(void);
int conn_send(const void *buf, size_t len);
/* Bytes read (> 0), 0 if nothing is waiting, < 0 once closed or broken. */
int conn_recv(void *buf, size_t len);
/* Abandons a connection or an attempt still in progress. */
void conn_close(void);

/* ---- sound.c ---------------------------------------------------------------- */

enum { SND_DOOR_OPEN, SND_DOOR_SLAM, SND_IM_IN, SND_IM_OUT, SND_COUNT };

void sound_play(int snd);
/* Feeds the clip playing; true while one is (call it every ~50 ms then). */
bool sound_pump(void);

#endif
