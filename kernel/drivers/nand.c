/*
 * Raw NAND page read/program and block erase through the VRC4173 SPL
 * transfer engine (doc: NAND Flash and Controller, "SPL Transfer Engine";
 * same command/address sequencing as the Linux port's nand.c and the SPL).
 * Samsung K9F2808-class part: 512-byte pages, 32 pages per block.
 *
 * Every operation is framed as Windows CE's kernel NAND path frames it
 * (traced in the emulator, its code at 0x80F0A658-0x80F0AE80): A400, A408,
 * A40C and CTRL are set to 1 first, and phase 0, CTRL 0 after; phases are
 * closed with phase 2, and ACK is read. The boot ROM leaves the engine set
 * up, but Windows CE's nanddisk.dll leaves A40C and CTRL clear. A read
 * drains the whole 528-byte page as Windows CE does. After Windows CE (a
 * RAM boot) nand_reset() runs first. On a real BE-300 that reads Windows
 * CE's FAT volume, but without nanddisk.dll's ECC correction and
 * bad-sector replacement some sectors come back wrong, so /NAND stays off
 * by default there (GlazeBoot nand=off).
 */

#include "../kernel.h"
#include "../hw.h"
#include "nand.h"


#define CMD_READ0       0x00u
#define CMD_RESET       0xFFu
#define CMD_SEQIN       0x80u
#define CMD_PAGEPROG    0x10u
#define CMD_ERASE1      0x60u
#define CMD_ERASE2      0xD0u

#define PHASE_IDLE      0x00u
#define PHASE_CLOSE     0x02u
#define PHASE_CMD       0x03u
#define PHASE_ADDR      0x06u

#define MODE_RESET      0x00u
#define MODE_READ       0x05u   /* page read burst */
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

static void select(void)
{
    NAND_UNIT_CLOCK = 1;
    NAND_XFER_EN0 = 1;
    NAND_XFER_EN2 = 1;
    NAND_XFER_PIO = 1;
    NAND_XFER_CTRL = 1;
}

static void deselect(void)
{
    NAND_XFER_CMD = PHASE_IDLE;
    NAND_XFER_CTRL = 0;
}

static void command(uint8_t cmd)
{
    NAND_XFER_CMD = PHASE_CLOSE;
    NAND_XFER_CMD = PHASE_CMD;
    NAND_XFER_ADDR8 = cmd;
    (void)NAND_XFER_ACK;
    NAND_XFER_CMD = PHASE_CLOSE;
    wait_ready();
}

/* Column + row address (reads and programs); the engine decodes 3 bytes. */
static void address_page(uint32_t page)
{
    NAND_XFER_CMD = PHASE_ADDR;
    NAND_XFER_ADDR8 = 0;
    NAND_XFER_ADDR8 = (uint8_t)page;
    NAND_XFER_ADDR8 = (uint8_t)(page >> 8);
    (void)NAND_XFER_ACK;
    NAND_XFER_CMD = PHASE_CLOSE;
    wait_ready();
}

/* Brings the NAND back to the state the kernel's PIO path expects after a
 * RAM boot: Windows CE's nanddisk.dll owns it through DMA and may be stopped
 * in the middle of a page. The DMA off, then the engine (MODE 0, phase 0,
 * deselected), then the chip's RESET command (K9F2808: aborts any
 * operation). */
void nand_reset(void)
{
    /* Take the NAND back from nanddisk.dll's DMA as it hands it to the
     * kernel's PIO path (see hw.h), so page data reaches the stream port. */
    NAND_DMA_STATE = 1;
    NAND_DMA_ENABLE = 0;
    NAND_XFER_EN0 = 0;
    NAND_XFER_MODE = MODE_RESET;
    deselect();
    select();
    command(CMD_RESET);
    deselect();
}

void nand_read_page(uint32_t page, void *buf)
{
    uint32_t *dst = buf;

    select();
    command(CMD_READ0);
    address_page(page);
    NAND_XFER_KICK = 1;
    NAND_XFER_MODE = MODE_READ;
    wait_ready();
    for (uint32_t i = 0; i < NAND_PAGE_DATA / 4; i++)
        dst[i] = NAND_STREAM32;
    for (int i = 0; i < 4; i++)
        (void)NAND_STREAM32;    /* the 16 spare bytes */
    NAND_XFER_MODE = MODE_RESET;
    deselect();
}

void nand_read_raw(uint32_t page, uint8_t buf[528])
{
    select();
    command(CMD_READ0);
    address_page(page);
    NAND_XFER_KICK = 1;
    NAND_XFER_MODE = MODE_READ;
    wait_ready();
    for (uint32_t i = 0; i < 528 / 4; i++)
        ((uint32_t *)buf)[i] = NAND_STREAM32;
    NAND_XFER_MODE = MODE_RESET;
    deselect();
}

void nand_program_page(uint32_t page, const void *buf)
{
    const uint8_t *src = buf;

    select();
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
    deselect();
}

void nand_erase_block(uint32_t block)
{
    uint32_t page = block * NAND_PAGES_PER_BLOCK;

    select();
    command(CMD_ERASE1);
    NAND_XFER_CMD = PHASE_ADDR;
    NAND_XFER_ADDR8 = (uint8_t)page;
    NAND_XFER_ADDR8 = (uint8_t)(page >> 8);
    (void)NAND_XFER_ACK;
    NAND_XFER_CMD = PHASE_CLOSE;
    wait_ready();
    command(CMD_ERASE2);
    deselect();
}
