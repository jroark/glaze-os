#ifndef CE_NUMBER_H
#define CE_NUMBER_H
#include <stdbool.h>
#include <stdint.h>
bool ce_number_parse(const uint16_t *, bool, uint32_t *);
unsigned ce_number_format(uint16_t[12], uint32_t, bool);
#endif
