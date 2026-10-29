#ifndef USB_MIRROR_H
#define USB_MIRROR_H
#include <stdint.h>
int mirror_active(void);
void mirror_close(void);
int mirror_dispatch(uint32_t op,const uint8_t *input,uint32_t len,uint8_t *out,uint32_t *size);
#endif
