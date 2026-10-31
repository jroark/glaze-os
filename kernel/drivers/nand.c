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
 * drains the whole 528-byte page as the ROM (9FC01A98-9FC01AAE) and
 * Casio's SPL (80F0ADFC-80F0AE28) do: 520 bytes in MODE 5, then MODE 4
 * for the last 8 spare bytes, as programming splits MODE 7/6. After Windows CE (a
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
#define CMD_STATUS      0x70u

#define PHASE_IDLE      0x00u
#define PHASE_CLOSE     0x02u
#define PHASE_CMD       0x03u
#define PHASE_ADDR      0x06u

#define MODE_RESET      0x00u
#define MODE_READ       0x05u   /* page read burst: 512 data + first 8 OOB */
#define MODE_OOB2_READ  0x04u   /* last 8 OOB bytes */
#define MODE_PROGRAM    0x07u   /* 512 data + first 8 OOB bytes */
#define MODE_OOB2       0x06u   /* last 8 OOB bytes */

static void wait_ready(void)
{
    for (int timeout = 1000000; timeout; timeout--) {
        if (NAND_XFER_STATUS & 1u)
            return;
    }
    /* Readers have no error return: continuing would expose stale page data
     * as executable text or filesystem contents. Stop before consuming it. */
    NAND_XFER_CMD = PHASE_IDLE;
    NAND_XFER_CTRL = 0;
    panic("nand: transfer engine timeout");
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
    NAND_UNIT_CLOCK = 1;
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
    for (int i = 0; i < 2; i++)
        (void)NAND_STREAM32;    /* the first 8 spare bytes */
    NAND_XFER_MODE = MODE_OOB2_READ;
    for (int i = 0; i < 2; i++)
        (void)NAND_STREAM32;    /* the last 8 */
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
    for (uint32_t i = 0; i < 520 / 4; i++)
        ((uint32_t *)buf)[i] = NAND_STREAM32;
    NAND_XFER_MODE = MODE_OOB2_READ;
    for (uint32_t i = 520 / 4; i < 528 / 4; i++)
        ((uint32_t *)buf)[i] = NAND_STREAM32;
    NAND_XFER_MODE = MODE_RESET;
    deselect();
}

/* Stock SPL 80F0AAFC/80F0AC3C issues 70h, then 80F0A7E0 reads
 * A420 bit 0. A440 reports engine completion, not program/erase success. */
static uint8_t chip_status(void)
{
    command(CMD_STATUS);
    return (uint8_t)NAND_XFER_DATA32;
}

uint8_t nand_status(void)
{
    select();
    uint8_t status = chip_status();
    deselect();
    return status;
}

static int program(uint32_t page, const uint8_t *src, bool spare)
{
    if (page >= 32768) return -1;

    select();
    command(CMD_READ0); /* Reset the small-page area pointer before SEQIN. */
    command(CMD_SEQIN);
    address_page(page);
    NAND_XFER_KICK = 1;
    NAND_XFER_MODE = MODE_PROGRAM;
    wait_ready();
    for (uint32_t i = 0; i < NAND_PAGE_DATA; i++)
        NAND_STREAM8 = src[i];
    for (int i = 0; i < 8; i++)
        NAND_STREAM8 = spare ? src[512+i] : 0xFF;
    NAND_XFER_MODE = MODE_OOB2;
    wait_ready();
    for (int i = 0; i < 8; i++)
        NAND_STREAM8 = spare ? src[520+i] : 0xFF;
    command(CMD_PAGEPROG);
    uint8_t status = chip_status();
    NAND_XFER_MODE = MODE_RESET;
    deselect();
    return (status & 0xC1u) == 0xC0u ? 0 : -1;
}

int nand_program_page(uint32_t page, const void *buf)
{
    return program(page, buf, false);
}

int nand_program_raw(uint32_t page, const uint8_t buf[528])
{
    return program(page, buf, true);
}

int nand_erase_block(uint32_t block)
{
    if (block >= 1024) return -1;
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
    uint8_t status = chip_status();
    deselect();
    return (status & 0xC1u) == 0xC0u ? 0 : -1;
}
