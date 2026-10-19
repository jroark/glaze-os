/*
 * Raw NAND page read/program and block erase through the VRC4173 SPL
 * transfer engine (doc: NAND Flash and Controller, "SPL Transfer Engine";
 * same command/address sequencing as the Linux port's nand.c and the SPL).
 * Samsung K9F2808-class part: 512-byte pages, 32 pages per block.
 */

#include "../kernel.h"
#include "../hw.h"
#include "nand.h"

#define CMD_READ0       0x00u
#define CMD_SEQIN       0x80u
#define CMD_PAGEPROG    0x10u
#define CMD_ERASE1      0x60u
#define CMD_ERASE2      0xD0u

#define PHASE_CMD       0x03u
#define PHASE_ADDR      0x06u

#define MODE_READ       0x05u   /* 520-byte read burst */
#define MODE_PROGRAM    0x07u   /* 512 data + first 8 OOB bytes */
#define MODE_OOB2       0x06u   /* last 8 OOB bytes */

static void wait_ready(void)
{
    for (int timeout = 1000000; timeout; timeout--) {
        if (NAND_XFER_STATUS & 1u)
            return;
    }
    kputs("nand: transfer engine timeout\n");
}

static void command(uint8_t cmd)
{
    NAND_XFER_CMD = PHASE_CMD;
    NAND_XFER_ADDR8 = cmd;
    NAND_XFER_ACK = 0;
    wait_ready();
}

/* Column + row address (reads and programs); the engine decodes 3 bytes. */
static void address_page(uint32_t page)
{
    NAND_XFER_CMD = PHASE_ADDR;
    NAND_XFER_ADDR8 = 0;
    NAND_XFER_ADDR8 = (uint8_t)page;
    NAND_XFER_ADDR8 = (uint8_t)(page >> 8);
    NAND_XFER_ACK = 0;
    wait_ready();
}

void nand_read_page(uint32_t page, void *buf)
{
    uint32_t *dst = buf;

    command(CMD_READ0);
    address_page(page);
    NAND_XFER_KICK = 1;
    NAND_XFER_MODE = MODE_READ;
    wait_ready();
    for (uint32_t i = 0; i < NAND_PAGE_DATA / 4; i++)
        dst[i] = NAND_STREAM32;
    (void)NAND_STREAM32;    /* drain the 8 OOB bytes of the burst */
    (void)NAND_STREAM32;
}

void nand_program_page(uint32_t page, const void *buf)
{
    const uint8_t *src = buf;

    command(CMD_SEQIN);
    address_page(page);
    NAND_XFER_KICK = 1;
    NAND_XFER_MODE = MODE_PROGRAM;
    wait_ready();
    for (uint32_t i = 0; i < NAND_PAGE_DATA; i++)
        NAND_STREAM8 = src[i];
    for (int i = 0; i < 8; i++)
        NAND_STREAM8 = 0xFF;
    NAND_XFER_MODE = MODE_OOB2;
    wait_ready();
    for (int i = 0; i < 8; i++)
        NAND_STREAM8 = 0xFF;
    command(CMD_PAGEPROG);
}

void nand_erase_block(uint32_t block)
{
    uint32_t page = block * NAND_PAGES_PER_BLOCK;

    command(CMD_ERASE1);
    NAND_XFER_CMD = PHASE_ADDR;
    NAND_XFER_ADDR8 = (uint8_t)page;
    NAND_XFER_ADDR8 = (uint8_t)(page >> 8);
    NAND_XFER_ACK = 0;
    wait_ready();
    command(CMD_ERASE2);
}
