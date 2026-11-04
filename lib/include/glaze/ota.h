/*
 * Over-the-air OS updates (docs/os/OTA.md): the formats shared by the build
 * tools (tools/mkota.py, tools/mk_nand.py), the SPL (boot/), the kernel
 * (kernel/ota.c) and userspace. All words are little-endian.
 *
 * An update replaces the logical boot blocks from the kernel slot to the
 * end of the romfs slot (GZ_OTA_FIRST_BLOCK onwards). The running OS only
 * verifies the image and arms a journal; the SPL rewrites the blocks on the
 * next boot, from the image staged in the file system, and does so again
 * after any interruption.
 */
#ifndef GLAZE_OTA_H
#define GLAZE_OTA_H

#include <stdint.h>

/* ---- NAND layout (logical byte offsets, tools/mk_nand.py) -------------- */

#define GZ_NAND_BLOCK_BYTES     (32u * 512u)
#define GZ_KERNEL_NAND_OFFSET   0x14000u    /* struct gz_kernel_header */
#define GZ_KERNEL_IMAGE_OFFSET  0x14200u    /* the flat kernel image */
#define GZ_ROMFS_NAND_OFFSET    0x40000u
#define GZ_BOOT_END             0x500000u   /* the file system follows */
#define GZ_KERNEL_SLOT          (GZ_ROMFS_NAND_OFFSET - GZ_KERNEL_NAND_OFFSET)
#define GZ_KERNEL_IMAGE_MAX     (GZ_ROMFS_NAND_OFFSET - GZ_KERNEL_IMAGE_OFFSET)
#define GZ_ROMFS_MAX            (GZ_BOOT_END - GZ_ROMFS_NAND_OFFSET)

/* The blocks an update writes: the kernel slot (header, image, 0xFF padding
 * to GZ_KERNEL_SLOT), then romfs, then erased blocks to GZ_BOOT_END. */
#define GZ_OTA_FIRST_BLOCK      (GZ_KERNEL_NAND_OFFSET / GZ_NAND_BLOCK_BYTES)
#define GZ_OTA_BLOCKS           (GZ_BOOT_END / GZ_NAND_BLOCK_BYTES - GZ_OTA_FIRST_BLOCK)
#define GZ_OTA_KERNEL_BLOCKS    (GZ_KERNEL_SLOT / GZ_NAND_BLOCK_BYTES)  /* then romfs */

/* Kernel load limits the SPL and kernel enforce: above the low-RAM pages
 * the boot ROM uses, below the SPL at 0x80F00000. */
#define GZ_KERNEL_LOAD_MIN      0x80010000u
#define GZ_KERNEL_LOAD_END      0x80F00000u

/* ---- the kernel slot's header ------------------------------------------ */

#define GZ_KHDR_MAGIC           0x324B5A47u     /* "GZK2" */
#define GZ_KHDR_FORMAT          1u

struct gz_kernel_header {           /* 512 bytes at GZ_KERNEL_NAND_OFFSET */
    uint32_t magic;                 /* GZ_KHDR_MAGIC */
    uint32_t format;                /* GZ_KHDR_FORMAT */
    uint32_t load, entry, size;     /* the flat image (tools/mkkernelbin.py) */
    uint32_t image_crc;             /* gz_crc32 of the size bytes after this page */
    uint32_t reserved0[2];
    char version[16];               /* GLAZE_VERSION, NUL-terminated */
    char build[32];                 /* BUILD_ID, informational */
    uint8_t reserved[428];
    uint32_t header_crc;            /* gz_crc32 of the preceding 508 bytes */
};
_Static_assert(sizeof(struct gz_kernel_header) == 512, "kernel header page");

/* ---- the update file (.gzu) -------------------------------------------- */

/* The signed header, then a table of the target blocks from the kernel slot
 * on (the blocks after the last are erased), then the blocks, each its
 * 16 KiB as raw deflate (RFC 1951), or stored as is when that isn't smaller,
 * so the SPL can inflate them one at a time into a fixed buffer. The
 * signature covers the header; the header's digest covers the rest. */
#define GZ_OTA_MAGIC            "GZOTA1"        /* 8 bytes with the NULs */
#define GZ_OTA_HEADER_SIZE      512u
#define GZ_OTA_SIGNED_BYTES     448u            /* the header before the signature */
#define GZ_OTA_MIN_BLOCKS       (GZ_OTA_KERNEL_BLOCKS + 1)      /* a kernel and a romfs table */
#define GZ_OTA_FILE_MAX         (GZ_OTA_HEADER_SIZE + GZ_OTA_BLOCKS * (12u + GZ_NAND_BLOCK_BYTES))

struct gz_ota_header {              /* 512 bytes at file offset 0 */
    char magic[8];                  /* GZ_OTA_MAGIC */
    uint32_t header_size;           /* GZ_OTA_HEADER_SIZE: the table follows */
    uint32_t file_size;             /* the whole file */
    uint32_t blocks;                /* table entries, GZ_OTA_MIN_BLOCKS..GZ_OTA_BLOCKS */
    uint32_t spl_min;               /* the oldest SPL that can apply it */
    char version[16];               /* the OS version it installs */
    char build[32];
    uint8_t body_sha256[32];        /* the table and the blocks: bytes header_size.. */
    uint8_t reserved[344];
    uint8_t signature[64];          /* Ed25519 over the first GZ_OTA_SIGNED_BYTES */
};
_Static_assert(sizeof(struct gz_ota_header) == GZ_OTA_HEADER_SIZE, "update header");
_Static_assert(__builtin_offsetof(struct gz_ota_header, signature) == GZ_OTA_SIGNED_BYTES,
               "signature placement");

struct gz_ota_block {               /* a table entry */
    uint32_t offset;                /* in the file */
    uint32_t size;                  /* GZ_NAND_BLOCK_BYTES: stored, else deflated */
    uint32_t crc;                   /* gz_crc32 of the block's 16 KiB */
};

/* ---- the journal (one reserve-pool NAND block) -------------------------- */

/* Its pages carry this spare tag in place of the stock AA 55 0F FF + logical
 * number, so neither the boot ROM's tag quorum nor Windows CE takes the
 * block for boot or file-system content; nand_boot_scan records where it is.
 * Pages 0..GZ_OTA_JOURNAL_PAGES-1 hold the journal, and it is tagged on the
 * first five pages, as the ROM's quorum reads. */
#define GZ_OTA_JOURNAL_TAG      0x4A4F5A47u     /* "GZOJ" */
#define GZ_OTA_JOURNAL_FORMAT   1u
#define GZ_OTA_JOURNAL_PAGES    3u
#define GZ_OTA_TAG_PAGES        5u
#define GZ_OTA_PATH_MAX         64u

struct gz_ota_journal {             /* journal pages 0-2 */
    uint32_t magic;                 /* GZ_OTA_JOURNAL_TAG */
    uint32_t format;                /* GZ_OTA_JOURNAL_FORMAT */
    uint32_t file_size;             /* the staged file */
    uint32_t table_offset, blocks;  /* its block table */
    uint32_t reserved0[3];
    char version[16];               /* what it installs, for the status report */
    char path[GZ_OTA_PATH_MAX];     /* the staged file on the flash volume */
    uint32_t block_crc[GZ_OTA_BLOCKS];  /* gz_crc32 of each target block, from the
                                           verified table (erased blocks' after it) */
    uint8_t reserved[1536 - 112 - 4 * GZ_OTA_BLOCKS - 4];
    uint32_t crc;                   /* gz_crc32 of the preceding bytes */
};
_Static_assert(sizeof(struct gz_ota_journal) == GZ_OTA_JOURNAL_PAGES * 512, "journal pages");

/* ---- what the SPL tells the kernel -------------------------------------- */

/* The SPL leaves this in the page with the hibernate record (kernel/resume.h,
 * which neither the boot ROM nor the kernel's allocator touches). A RAM boot
 * or an SPL before OTA support leaves none (magic absent: version 0). */
#define GZ_SPL_INFO_PA          0xE800u
#define GZ_SPL_INFO_MAGIC       0x49535A47u     /* "GZSI" */
#define GZ_SPL_VERSION          3u       /* compares the kernel header beyond its CRC residue */
#define GZ_SPL_F_OTA            (1u << 0)       /* applies journals */

enum {
    GZ_OTA_NONE,                    /* no journal at boot */
    GZ_OTA_APPLIED,                 /* applied version[]; the journal is gone */
    GZ_OTA_REJECTED,                /* didn't verify; nothing written, journal gone */
};

struct gz_spl_info {
    uint32_t magic;                 /* GZ_SPL_INFO_MAGIC */
    uint32_t version;               /* GZ_SPL_VERSION */
    uint32_t features;              /* GZ_SPL_F_* */
    uint32_t ota_result;            /* GZ_OTA_* */
    uint32_t ota_detail;            /* GZ_OTA_E_* for REJECTED, else blocks written */
    uint32_t reserved[3];
    char ota_version[16];
};

/* Why an SPL rejected a journal, or a kernel an image. */
enum {
    GZ_OTA_E_OK,
    GZ_OTA_E_JOURNAL,               /* journal unreadable or inconsistent */
    GZ_OTA_E_MOUNT,                 /* the file system wouldn't mount */
    GZ_OTA_E_FILE,                  /* staged file missing or the wrong size */
    GZ_OTA_E_READ,                  /* a staged page couldn't be read */
    GZ_OTA_E_CRC,                   /* a staged block didn't match the journal */
    GZ_OTA_E_FORMAT,                /* not an update file, or a malformed payload */
    GZ_OTA_E_SIGNATURE,             /* not signed by the release key */
    GZ_OTA_E_DIGEST,                /* payload SHA-256 mismatch */
    GZ_OTA_E_VERSION,               /* not newer than the running OS */
    GZ_OTA_E_SPL,                   /* this SPL can't apply it */
    GZ_OTA_E_SPACE,                 /* no free reserve block for the journal */
    GZ_OTA_E_FLASH,                 /* the journal couldn't be written */
};

/* SYS_OTA_STAGE returns GZ_OTA_ERR(GZ_OTA_E_*) for a rejected image, which
 * GZ_OTA_ERRNO recovers (0 for the ordinary GZ_E* errors). */
#define GZ_OTA_ERR(e)           (-0x100 - (int)(e))
#define GZ_OTA_ERRNO(rv)        ((rv) <= -0x100 ? -(rv) - 0x100 : 0)

/* SYS_OTA_STAGE operations, for the file server only (PROC_FLASH). */
enum {
    GZ_OTA_OP_BEGIN = 1,            /* a1 = header (512 bytes), a2 = path, a3 = file size */
    GZ_OTA_OP_DATA,                 /* a1 = payload bytes, a2 = count (<= GZ_OTA_DATA_MAX) */
    GZ_OTA_OP_COMMIT,
    GZ_OTA_OP_ABORT,
    GZ_OTA_OP_ARM,                  /* writes the journal and resets; returns only on error */
};
#define GZ_OTA_DATA_MAX         8192u

/* ---- SYS_OTA_STATUS ------------------------------------------------------ */

#define GZ_OTA_S_SUPPORTED      (1u << 0)       /* physical layout, SPL with GZ_SPL_F_OTA */
#define GZ_OTA_S_PHYSICAL       (1u << 1)       /* a GZNAND1 physical image */
#define GZ_OTA_S_STAGED         (1u << 2)       /* a verified image awaits SYS_OTA_ARM */

struct gz_ota_status {
    uint32_t flags;                 /* GZ_OTA_S_* */
    uint32_t spl_version;           /* 0: no OTA-capable SPL */
    uint32_t last_result;           /* this boot's struct gz_spl_info ota_result */
    uint32_t last_detail;
    char version[16];               /* the running OS */
    char build[32];
    char last_version[16];          /* what this boot's SPL applied or rejected */
    uint32_t reserved[4];
};

/* ---- helpers --------------------------------------------------------------- */

/* The zlib/PNG CRC-32 (gz_nand_manifest_crc's), continued from crc. */
static inline uint32_t gz_crc32(uint32_t crc, const void *p, uint32_t n)
{
    const uint8_t *b = p;

    crc = ~crc;
    while (n--) {
        crc ^= *b++;
        for (unsigned j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* Dotted numeric versions, as the App Store compares them ("1.10" > "1.9",
 * "1" == "1.0"): <0, 0 or >0. */
static inline int gz_version_cmp(const char *a, const char *b)
{
    while (*a || *b) {
        uint32_t av = 0, bv = 0;
        while (*a >= '0' && *a <= '9')
            av = av * 10 + (uint32_t)(*a++ - '0');
        while (*b >= '0' && *b <= '9')
            bv = bv * 10 + (uint32_t)(*b++ - '0');
        if (av != bv)
            return av > bv ? 1 : -1;
        if (*a == '.')
            a++;
        else if (*a)
            return -1;      /* malformed: never newer */
        if (*b == '.')
            b++;
        else if (*b)
            return 1;
    }
    return 0;
}

/* Whether s (at most n bytes) is a dotted numeric version: digits, single
 * dots between groups of at most six, NUL-terminated within n. */
static inline int gz_version_ok(const char *s, uint32_t n)
{
    uint32_t digits = 0, i = 0;

    for (; i < n && s[i]; i++) {
        if (s[i] == '.') {
            if (!digits)
                return 0;
            digits = 0;
        } else if (s[i] >= '0' && s[i] <= '9') {
            if (++digits > 6)
                return 0;
        } else
            return 0;
    }
    return i < n && digits != 0;
}

#endif
