#ifndef CE_DATETIME_H
#define CE_DATETIME_H
#include <glaze/types.h>
/* SYSTEMTIME: year, month, weekday, day, hour, minute, second, millisecond. */
bool ce_datetime_ticks(const uint16_t value[8], uint64_t *ticks);
bool ce_datetime_from_ticks(uint64_t ticks, uint16_t value[8]);
/* English names, January and Sunday first. */
extern const char *const ce_month_names[12], *const ce_day_names[7];
unsigned ce_datetime_format(const uint16_t value[8], const uint16_t *format,
                            uint16_t *out, unsigned capacity);
#endif
