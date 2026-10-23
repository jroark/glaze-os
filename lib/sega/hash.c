#include "core.h"
uint32_t sega_hash(const void *data,size_t size) {
    const uint8_t *p=data; uint32_t h=2166136261u;
    while (size--) h=(h^*p++)*16777619u;
    return h;
}
