/* Windows CE's NAND page ECC on the BE-300 (nand_ecc.c). */
#ifndef GLAZE_NAND_ECC_H
#define GLAZE_NAND_ECC_H

#include <stdint.h>

#define NAND_ECC_BYTES  520u    /* data and the first 8 spare bytes */

/* Errors located from syndromes S0..S5: 0 none, 1 or 2 (pos, val), -1
 * uncorrectable. Exposed for the host test. */
int nand_ecc_solve(const uint16_t s[6], uint32_t pos[2], uint16_t val[2]);

/* Checks and corrects a raw page (512 data and 16 spare bytes) in
 * software: 0 if clean, 1 or 2 corrected RS symbols (data/tag or parity),
 * or -1. Failed corrections leave the page unchanged. */
int nand_ecc_check(uint8_t page[528]);
/* Preserve bytes 0..519, generate ROM/CE-compatible packed parity. */
void nand_ecc_encode(uint8_t page[528]);

#endif
