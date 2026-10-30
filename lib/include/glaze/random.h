/* Device-local seed material; subsequent bytes should come from a DRBG. */
#ifndef GLAZE_RANDOM_H
#define GLAZE_RANDOM_H
#include <glaze/abi.h>
/* Synchronous and potentially slow (seconds on a BE-300). Call from a worker
 * or before starting a handshake. Returns 0 or a GZ_* error, wiping output
 * on collection failure. No host, card or saved seed is needed. */
int gz_entropy_seed(void *out, uint32_t size);
/* Load an exact 32-byte saved seed, or collect one on this device. The caller
 * owns saving/ratcheting it; a short or oversized file is not a valid seed. */
int gz_entropy_load_seed(const char *path, uint8_t seed[32]);
#endif
