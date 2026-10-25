/*
 * BE-300 hardware register map used by the Glaze kernel.
 *
 * Every address here is documented in be300/docs/EMULATED_HARDWARE_LINUX_PORTING.md
 * (and the VR4131 / VRC4173 user manuals it cites). All accesses go through
 * KSEG1 (uncached) aliases.
 */
#ifndef GLAZE_HW_H
#define GLAZE_HW_H

#include "types.h"

#define KSEG0(pa)   ((uintptr_t)(pa) | 0x80000000u)
#define KSEG1(pa)   ((uintptr_t)(pa) | 0xA0000000u)
#define PHYS(va)    ((uintptr_t)(va) & 0x1FFFFFFFu)

#define REG8(pa)    (*(volatile uint8_t  *)KSEG1(pa))
#define REG16(pa)   (*(volatile uint16_t *)KSEG1(pa))
#define REG32(pa)   (*(volatile uint32_t *)KSEG1(pa))

#define SDRAM_SIZE          (16u * 1024u * 1024u)

/* VR4131 ICU (doc: Interrupts). */
#define ICU_BASE            0x0F000080u
#define ICU_SYSINT1         REG16(ICU_BASE + 0x00)
#define ICU_GIUINTL         REG16(ICU_BASE + 0x08)
#define ICU_MSYSINT1        REG16(ICU_BASE + 0x0C)
#define ICU_MGIUINTL        REG16(ICU_BASE + 0x14)
#define ICU_SYSINT2         REG16(0x0F0000A0u)
#define ICU_GIUINTH         REG16(0x0F0000A2u)
#define ICU_MSYSINT2        REG16(0x0F0000A6u)
#define ICU_MGIUINTH        REG16(0x0F0000A8u)
#define SYSINT1_RTCL1       (1u << 2)
#define SYSINT1_GIU         (1u << 8)

/* VR4131 PMU (doc: PMU). */
#define PMU_BASE            0x0F0000C0u
#define PMU_INT             REG16(PMU_BASE + 0x00)
#define PMU_CNT             REG16(PMU_BASE + 0x02)
#define PMU_CNT2            REG16(PMU_BASE + 0x06)
#define PMUCNT_HALTIMERRST  (1u << 2)
#define PMUCNT2_SOFTRST     (1u << 4)

/* VR4131 RTC (doc: RTC / timers). */
#define RTC_BASE            0x0F000100u
#define RTC_ETIME_L         REG16(RTC_BASE + 0x00)
#define RTC_ETIME_M         REG16(RTC_BASE + 0x02)
#define RTC_ETIME_H         REG16(RTC_BASE + 0x04)
#define RTC_HZ              32768u

/* VR4131 GIU (doc: GPIO). */
#define GIU_BASE            0x0F000140u
#define GIU_INTSTATL        REG16(GIU_BASE + 0x08)
#define GIU_INTSTATH        REG16(GIU_BASE + 0x0A)
#define GIU_INTENL          REG16(GIU_BASE + 0x0C)
#define GIU_INTENH          REG16(GIU_BASE + 0x0E)

/* VRC4173 companion: GIRQ0 cascade on GIU line 0 (doc: Interrupts; the
 * Linux port's irq.c). KJ_INTSTAT shows each source's level. */
#define KJ_INTSTAT          REG32(0x0A000004u)
/* Per-source control for GIRQ0 bits 0 (PCMCIA) and 1 (buttons): bit 3
 * enables the source. Windows CE's OAL masks a source in its ISR with
 * `&= ~0x08` (docs hardware.txt, GIRQ0-0 and GIRQ0-1), and both read 0x0C
 * under Windows CE (hw_dump_vrc4173.txt core block). The emulator takes any
 * write to either as that source's acknowledge. */
#define KJ_GIRQ0_EN(n)      REG32(0x0A000010u + 4u * (n))   /* GIRQ0 source n enable */
#define KJ_SRC_PCMCIA       REG32(0x0A000010u)
#define KJ_SRC_KEYS         REG32(0x0A000014u)
#define KJ_SRC_ENABLE       0x08u


/* ScCmcu, the companion's interface to Casio's power-management
 * microcontroller (hardware.txt:189, "ScCmcu Base on companion"). */
#define CMCU_BASE           0x0A007800u
#define CMCU_CMD            REG32(CMCU_BASE + 0x08)
#define CMCU_ARG            REG32(CMCU_BASE + 0x0C)
#define CMCU_CONFIG         REG32(CMCU_BASE + 0x10)
#define CMCU_TIMEOUT        REG32(CMCU_BASE + 0x14)
#define CMCU_STATUS         REG32(CMCU_BASE + 0x34)
#define KJ_CF_CAUSE         REG32(0x0A00A03Cu)
#define KJ_INT_CF           (1u << 0)
#define KJ_INT_KEYS         (1u << 1)
#define KJ_INT_AUDIO        (1u << 8)
#define KJ_INT_TOUCH        (1u << 9)
#define KJ_SRC_EN           REG32(0x0A000030u)   /* bit 3: audio source enable */

/* Wave output: a Casio ping-pong DMA block in the Kj window, not the
 * VRC4173 UM's AIU (UM ch. 10 offsets 0x0E0..0x0FC see no traffic). The
 * map comes from WinCE wavedev.dll: be300 docs/BE300_AUDIO.md, modelled in
 * be300 src/hw/aiu.c. Buffer registers take KSEG1 addresses, end
 * inclusive; samples are S16LE, mono or interleaved stereo. */
#define AUD_CODEC_CMD       REG32(0x0A0003C0u)   /* serial codec command */
#define AUD_CODEC_READY     REG32(0x0A0003C4u)   /* non-zero when idle */
#define AUD_CODEC_STATUS    REG32(0x0A0003F4u)   /* bit 7 folds into 0xD007 */
#define AUD_CTRL            REG32(0x0A000880u)   /* 6:4 rate, 3 output on */
#define AUD_POWER           REG32(0x0A000884u)   /* 0 enable, 1 codec on */
#define AUD_FORMAT          REG32(0x0A000888u)   /* 1 mono, 2 cleared on setup */
#define AUD_START           REG32(0x0A000890u)   /* bit 1 set on start */
#define AUD_DMA_RUN         REG32(0x0A000898u)
#define AUD_ARM             REG32(0x0A0008A0u)
#define AUD_STATUS          REG32(0x0A0008A8u)   /* bit n: buffer n drained */
#define AUD_BUF0_START      REG32(0x0A0008B0u)
#define AUD_BUF0_END        REG32(0x0A0008B4u)
#define AUD_BUF1_START      REG32(0x0A0008B8u)
#define AUD_BUF1_END        REG32(0x0A0008BCu)
#define AUD_GO              REG32(0x0A0008C4u)   /* written 1 on start */
#define AUD_ACK0            REG32(0x0A0008C8u)
#define AUD_ACK1            REG32(0x0A0008CCu)
#define AUD_CTRL_RATE_MASK  0x70u
#define AUD_CTRL_OUT        0x08u
#define AUD_SRC_EN_BIT      0x08u

/* PCMCIA card socket. Setting bit 2 of the bridge register at 0x0A00A01C
 * turns the C000/D000 pages into card windows (the stock pcmcia.dll does
 * this before its CIS scan; be300 src/devices/nand_window.c, and the
 * Linux port's setup.c). The companion's card status at 0x0A001000 reads
 * 0x4 with a card, 0xC for an empty attached socket (doc: CompactFlash and
 * PCMCIA), and 0 on the emulator when nothing is attached. */
/* Socket power and reset, as Windows CE's pcmcia.dll drives them when a card
 * is inserted (traced in the emulator, its code at 0x0198BFD4-0x0198C1F8):
 * CTRL 0 -> 8 -> 9 with RESET bit 0 held, then RESET released and CTRL 0x0B,
 * STATUS reading 0x2F9 once the card is up (hw_dump_vrc4173.txt). */
#define PCMCIA_CTRL         REG32(0x0A00A000u)
#define PCMCIA_RESET        REG32(0x0A00A004u)
#define PCMCIA_STATUS       REG32(0x0A00A008u)
#define PCMCIA_A020         REG32(0x0A00A020u)
#define PCMCIA_A024         REG32(0x0A00A024u)
/* Written 1 by Windows CE's interrupt handlers on every interrupt
 * (hardware.txt:15); the emulator opens the card windows on it. */
#define PCMCIA_WINDOWS      REG32(0x0A001B20u)
#define PCMCIA_BRIDGE       REG32(0x0A00A01Cu)
#define PCMCIA_BRIDGE_EN    0x04u
#define PCMCIA_CARD_STATUS  REG32(0x0A001000u)
#define PCMCIA_STATUS_MASK  0x0Cu
#define PCMCIA_CARD_IN      0x04u

/* NE2000 (DP8390) in the card I/O window at ISA port 0x300; the window
 * decodes the low five address bits (be300 src/hw/cf_ne2000.c). */
#define NE_BASE             0x0A00C300u
#define NE_REG(r)           REG8(NE_BASE + (r))
#define NE_DATA16           REG16(NE_BASE + 0x10)
#define NE_RESET            REG8(NE_BASE + 0x1F)

/* CompactFlash ATA card in the same window, in I/O mode at the secondary
 * IDE ports: the taskfile at ISA 0x170..0x177 and device control /
 * alternate status at 0x376 (CF+ spec 4.1 "I/O Primary/Secondary"; be300
 * src/devices/nand_window.c, the Linux port's pata_platform resources).
 * The card information structure is attribute memory at 0x0A00D000, one
 * tuple byte per even address (PC Card spec, "Attribute Memory"). */
#define CF_ATA_BASE         0x0A00C170u
#define CF_ATA(r)           REG8(CF_ATA_BASE + (r))
#define CF_ATA_CTL          REG8(0x0A00C376u)
/* Tuples only: 0x0A00D200/D202 is the NAND direct-I/O data/control port
 * (the boot ROM sends NAND commands there), so attribute offset 0x200, the
 * card's configuration register, is not reachable as CF_CIS(0x100); writing
 * it on a real BE-300 sent the NAND a stray byte and hung its next read. */
#define CF_CIS(i)           REG8(0x0A00D000u + 2 * (i))

/* VRC4173 SIU UART, register stride 4 (doc: UARTs). */
#define UART_BASE           0x0A008680u
#define UART_THR            REG8(UART_BASE + 0 * 4)
#define UART_RBR            REG8(UART_BASE + 0 * 4)
#define UART_LSR            REG8(UART_BASE + 5 * 4)
#define UART_LSR_DR         0x01u
#define UART_LSR_THRE       0x20u

/* Linear framebuffer (doc: Framebuffer). */
/* Stock ddi.dll 0x01A56BB0/BD0 (backlight), 0x01A56A5C (contrast).
 * See docs/DISPLAY.md for the register trace and native level tables. */
#define DISP_BACKLIGHT      REG32(0x0A00098Cu)
#define DISP_CONTRAST       REG32(0x0A000C4Cu)

#define FB_PA               0x0A200000u
#define FB_WIDTH            240
#define FB_HEIGHT           320
#define FB_STRIDE_PX        256
#define FB_SIZE             (FB_STRIDE_PX * 324 * 2)

/* PIU touch panel (doc: Touch Panel: VRC4173 PIU). */
#define PIU_BASE            0x0A000300u
#define PIU_CNT             REG16(PIU_BASE + 0x00)
#define PIU_INT             REG16(PIU_BASE + 0x04)
#define PIU_SIVL            REG16(PIU_BASE + 0x08)
#define PIU_STBL            REG16(PIU_BASE + 0x0C)
#define PIU_PAGE0           (PIU_BASE + 0x20)
#define PIU_PAGE1           (PIU_BASE + 0x50)
#define PIUCNT_PIUPWR       0x0002u
#define PIUCNT_PIUSEQEN     0x0004u
#define PIUCNT_PADATSTART   0x0100u
#define PIUCNT_PADATSTOP    0x0200u
#define PIUCNT_PENDOWN      0x2000u
#define PIUINT_PENCHG       0x01u
#define PIUINT_DATALOST     0x04u
#define PIUINT_PAGE0        0x08u
#define PIUINT_OTHER        0xC0u   /* enabled by Windows CE; acknowledged */
#define PIUINT_PAGE1        0x10u

/* Buttons (doc: Buttons). A dummy halfword read at +2 precedes the live
 * bytes, matching the stock driver. */
#define KEYS_LATCH          REG16(0x0A00A042u)
#define KEYS_SET1           REG8(0x0A00A044u)
#define KEYS_SET2           REG8(0x0A00A045u)
#define KEY1_OK             0x04u
#define KEY1_ESC            0x08u
#define KEY1_UP             0x10u
#define KEY1_DOWN           0x20u
#define KEY1_RIGHT          0x40u
#define KEY1_LEFT           0x80u
#define KEY2_ROCKET         0x10u
#define KEY2_POWER          0x80u

/* NAND via the VRC4173 SPL transfer engine (doc: NAND Flash and Controller). */
/* Written 1 before every operation by Windows CE's kernel (PIO) NAND path
 * (0x80F0A658-64); its nanddisk.dll clears A40C after each DMA transfer, so
 * after Windows CE the stream port isn't fed until they are set again. */
/* The companion's NAND DMA, which Windows CE's nanddisk.dll turns on while
 * it owns the NAND (0x1CC0 = 1) and off when it hands the engine back to
 * the kernel's PIO path (0x1CC0 = 0, PC 0x019A58A4); 0x1CF4 = 1 after each
 * transfer. While it is on, page data goes to DMA, not the stream port. */
#define NAND_DMA_ENABLE     REG32(0x0A001CC0u)
#define NAND_DMA_STATE      REG32(0x0A001CF4u)
#define NAND_XFER_EN0       REG32(0x0A00A400u)
#define NAND_XFER_EN2       REG32(0x0A00A408u)
#define NAND_XFER_PIO       REG32(0x0A00A40Cu)
#define NAND_XFER_CTRL      REG32(0x0A00A410u)
#define NAND_XFER_CMD       REG32(0x0A00A414u)
#define NAND_XFER_ADDR8     REG8(0x0A00A420u)
#define NAND_XFER_ACK       REG32(0x0A00A430u)
#define NAND_XFER_STATUS    REG32(0x0A00A440u)
#define NAND_XFER_KICK      REG32(0x0A00A460u)
#define NAND_XFER_MODE      REG32(0x0A00A464u)
#define NAND_STREAM32       REG32(0x0A00B000u)
#define NAND_STREAM8        REG8(0x0A00B000u)
#define NAND_PAGE_DATA      512u
#define NAND_PAGES_PER_BLOCK 32u
/* Flash layout above the kernel: the file server's region runs from here to
 * the end of the NAND. */
/* romfs (tools/mkromfs.py) sits raw between the kernel and the file system;
 * tools/mk_nand.py places it, and romfs.c pages it in. */
#define ROMFS_OFFSET        0x40000u
#define FLASH_FS_OFFSET     0x500000u

#endif
