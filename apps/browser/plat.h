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

#endif
