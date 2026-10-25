#ifndef SOMAFM_CLIENT_H
#define SOMAFM_CLIENT_H
#include <glaze/radio.h>
int somafm_state(struct radio_state *state);
int somafm_play(const struct radio_request *request);
int somafm_stop(void);
#endif
