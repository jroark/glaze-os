#ifndef CE_ORDINALS_H
#define CE_ORDINALS_H
#include <glaze/types.h>
bool ce_ordinal_find(const char *map, unsigned bytes, const char *dll, uint16_t ordinal,
                     char *name, unsigned capacity);
#endif
