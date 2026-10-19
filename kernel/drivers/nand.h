#ifndef GLAZE_NAND_H
#define GLAZE_NAND_H

#include <stdint.h>

void nand_read_page(uint32_t page, void *buf);
void nand_program_page(uint32_t page, const void *buf);
void nand_erase_block(uint32_t block);

#endif
