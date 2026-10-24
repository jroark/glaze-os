/*
 * RAM boot: what GlazeBoot (wince/glazeboot), a loader run from Windows CE,
 * hands the kernel. The loader copies the kernel to its link address, the
 * romfs archive to an 8 KiB-aligned region at the top of RAM, and this
 * block to GZ_BOOTINFO_PA, then jumps to the entry with a0 = its KSEG0
 * address. The SPL passes a0 = 0, a NAND boot.
 *
 * In a RAM boot the file server's volume is a RAM disk of ramfs_blocks
 * erase blocks at ramfs_pa (lost on reset), and the whole NAND, which still
 * holds Windows CE, is the file server's through SYS_NAND_*.
 */
#ifndef GLAZE_BOOTINFO_H
#define GLAZE_BOOTINFO_H

#include <glaze/types.h>

#define GZ_BOOTINFO_MAGIC   0x42525a47u     /* "GZRB" */
#define GZ_BOOTINFO_VERSION 1u
#define GZ_BOOTINFO_PA      0xF000u         /* the page below the kernel */

#define GZ_BOOT_NAND_RW     (1u << 0)       /* SYS_NAND_PROG/ERASE allowed */

struct gz_bootinfo {
    uint32_t magic, version, flags;
    uint32_t romfs_pa, romfs_size;          /* romfs_pa 8 KiB aligned */
    uint32_t ramfs_pa, ramfs_blocks;        /* 16 KiB erase blocks */
    uint32_t reserved;
};

/* The flat kernel file the loader reads (tools/mkkernelbin.py): this header
 * in a page of its own, then the image from `load` for `size` bytes. */
#define GZ_KERNEL_BIN_MAGIC 0x424b5a47u     /* "GZKB" */
#define GZ_KERNEL_BIN_HEADER 4096u

struct gz_kernel_bin {
    uint32_t magic;
    uint32_t load, entry, size;             /* KSEG0 addresses */
    uint32_t end;                           /* __kernel_end, past the BSS */
    uint32_t reserved[3];
};

#endif
