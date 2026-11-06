#ifndef EYEFI_RESEARCH_H
#define EYEFI_RESEARCH_H
#include <stdint.h>
#include "protocol.h"
/* Runs a newline-separated research script (commands in research.c),
 * modifying script in place. Returns 0, or the first command's error. */
int eyefi_research(const struct eyefi_io *io, uint8_t *buf, char *script);
#endif
