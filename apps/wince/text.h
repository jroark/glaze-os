#ifndef CE_TEXT_H
#define CE_TEXT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Windows-1252 (the BE-300's ANSI code page) and UTF-8 conversions. UTF-16
 * units are converted one by one, as ce_narrow/ce_widen do. */
uint16_t ce_cp1252_decode(uint8_t);
int ce_cp1252_encode(uint16_t); /* -1 if the character has no byte */
unsigned ce_utf8_decode(const uint8_t *in, size_t length, uint16_t *out, unsigned capacity);
unsigned ce_utf8_encode(const uint16_t *in, size_t length, uint8_t *out, unsigned capacity);
/* wsprintfW: `args` are the variadic words, `capacity` counts the terminator.
 * Returns the length written, or -1 for an unsupported conversion. */
int ce_wformat(uint16_t *out, unsigned capacity, const uint16_t *format, const uintptr_t *args,
               unsigned count);
#endif
