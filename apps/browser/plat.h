/*
 * What the engine needs from its platform: implemented by main.c on the
 * device and by host/browse.c on the host.
 */
#ifndef BROWSER_PLAT_H
#define BROWSER_PLAT_H

#include <stdint.h>

uint32_t plat_ms(void);
/* file: URLs: a malloc'd copy of the file, or a WE_* error. */
int plat_read_file(const char *path, uint8_t **data, int *len);
/* res: URLs: the browser's bundled pages (romfs "web/"), or NULL. */
const void *plat_res(const char *name, uint32_t *size);

/* Name lookups run in the background so the UI stays live: start returns
 * an id (or a WE_* error); poll returns 1 with the IPv4 address (network
 * order), 0 while pending, or a WE_* error. Finished or cancelled lookups
 * free their id. */
int plat_resolve_start(const char *host);
int plat_resolve_poll(int id, uint32_t *addr);
void plat_resolve_cancel(int id);

/* TLS (tls.c). The trusted roots: concatenated DER certificates, or NULL. */
const void *plat_roots(uint32_t *size);
/* Seconds since 1970 by the clock (local time is close enough). */
long long plat_time(void);
/* Fills buf with timing noise, a little entropy per byte at best. */
void plat_noise(uint8_t *buf, int n);
/* A random seed kept between runs: load leaves seed alone if there is none. */
#define PLAT_SEED_LEN   32
void plat_load_seed(uint8_t *seed);
void plat_save_seed(const uint8_t *seed);

#endif
