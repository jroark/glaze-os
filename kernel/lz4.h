/* LZ4 block decoding (kernel/lz4.c). */
#ifndef GLAZE_KERNEL_LZ4_H
#define GLAZE_KERNEL_LZ4_H
#include <glaze/types.h>
/* Decodes one block into at most dst_len bytes; returns the decoded length,
 * or -1 if the input is malformed or would overflow dst. */
int lz4_decode(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_len);
#endif
