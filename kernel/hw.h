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
#define ICU_MSYSINT2        REG16(0x0F0000A6u)
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
#define GIU_INTENL          REG16(GIU_BASE + 0x0C)

/* VRC4173 companion: GIRQ0 cascade on GIU line 0 (doc: Interrupts; the
 * Linux port's irq.c). Status and ack are write-1-to-clear. */
#define KJ_INTSTAT          REG32(0x0A000004u)
#define KJ_INTACK           REG32(0x0A000014u)
#define KJ_CF_CAUSE         REG32(0x0A00A03Cu)
#define KJ_INT_CF           (1u << 0)
#define KJ_INT_KEYS         (1u << 1)
#define KJ_INT_TOUCH        (1u << 9)

/* VRC4173 SIU UART, register stride 4 (doc: UARTs). */
#define UART_BASE           0x0A008680u
#define UART_THR            REG8(UART_BASE + 0 * 4)
#define UART_RBR            REG8(UART_BASE + 0 * 4)
#define UART_LSR            REG8(UART_BASE + 5 * 4)
#define UART_LSR_DR         0x01u
#define UART_LSR_THRE       0x20u

/* Linear framebuffer (doc: Framebuffer). */
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
#define PIUCNT_PENDOWN      0x2000u
#define PIUINT_PENCHG       0x01u
#define PIUINT_DATALOST     0x04u
#define PIUINT_PAGE0        0x08u
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

#endif
