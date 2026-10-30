#ifndef GLAZE_NAND_H
#define GLAZE_NAND_H

#include <stdint.h>

void nand_reset(void);
void nand_read_page(uint32_t page, void *buf);
/* A page's data and all 16 spare bytes (528), for nand_ecc_check(). */
void nand_read_raw(uint32_t page, uint8_t buf[528]);
/* Chip status is distinct from the transfer engine's ready bit. */
uint8_t nand_status(void);
/* 0 on verified chip completion, -1 on failure/write protection. */
int nand_program_page(uint32_t page, const void *buf);
int nand_program_raw(uint32_t page, const uint8_t buf[528]);
int nand_erase_block(uint32_t block);

#endif
