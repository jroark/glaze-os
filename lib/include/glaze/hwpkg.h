/*
 * Hardware-support packages (docs/os/HARDWARE_PACKAGES.md).
 *
 * A hardware package is an ordinary .gzp (glaze/pkg.h) whose Info says
 * "type=hardware". It is installed on the flash volume only, as
 * /Applications/<id>.app like an app, but SpringBoard never lists it. Besides
 * Info it holds:
 *
 *   Manifest      struct gz_hw_manifest, then `count` struct gz_hw_entry,
 *                 then (when `flags & GZ_HW_SIGNED`) a 64-byte Ed25519
 *                 signature over everything before it, by the OTA release key.
 *   hw/<path>     each file the manifest lists, e.g. hw/drivers/hermes.ko.
 *
 * Paths are the romfs names the kernel would otherwise look up: drivers/<name>.ko
 * (kernel modules), bin/<service> (a replacement for an overridable service
 * in the catalog, such as bin/net with PPP) and firmware/<file> (blobs loaded into
 * a card). Code (drivers/, bin/) is accepted only in a signed manifest built
 * for exactly this OS version; firmware may come in an unsigned one, since it
 * runs on the card, not on the VR4131.
 *
 * The file server registers every installed hardware package's manifest with
 * the kernel at boot and after each install or uninstall (SYS_HW_CTL). The
 * kernel keeps only the table; when a driver, service or firmware file is
 * needed it asks the file server for the bytes (GZ_NOTIFY_HW), and checks
 * them against the manifest's SHA-256 before using them.
 */
#ifndef GLAZE_HWPKG_H
#define GLAZE_HWPKG_H

#include <glaze/types.h>

#define GZ_HW_MAGIC         "GZHWMF1"
#define GZ_HW_FORMAT        1u
#define GZ_HW_ENTRIES_MAX   8u
#define GZ_HW_PATH_MAX      48u
#define GZ_HW_ID_MAX        28u     /* PKG_ID_MAX + 1, padded */
#define GZ_HW_FILE_MAX      (512u * 1024u)
#define GZ_HW_SIGNATURE     64u
#define GZ_HW_SIGNED        1u      /* gz_hw_manifest.flags */

enum { GZ_HW_CODE = 1, GZ_HW_DATA = 2 };

struct gz_hw_manifest {
    char magic[8];                  /* GZ_HW_MAGIC, NUL-padded */
    uint32_t format;                /* GZ_HW_FORMAT */
    uint32_t flags;                 /* GZ_HW_SIGNED */
    uint32_t count;                 /* entries that follow, 1..GZ_HW_ENTRIES_MAX */
    char id[GZ_HW_ID_MAX];          /* the package id, as in Info */
    char os[16];                    /* GLAZE_VERSION the code was built for */
};

struct gz_hw_entry {
    char path[GZ_HW_PATH_MAX];      /* romfs-style name; the file is hw/<path> */
    uint32_t size;
    uint32_t kind;                  /* GZ_HW_CODE or GZ_HW_DATA */
    uint8_t sha256[32];
};

#define GZ_HW_MANIFEST_MAX  (sizeof(struct gz_hw_manifest) + \
                             GZ_HW_ENTRIES_MAX * sizeof(struct gz_hw_entry) + GZ_HW_SIGNATURE)

/* SYS_HW_CTL operations: PROC_FLASH (the file server) only, except STATUS. */
enum {
    GZ_HW_OP_BEGIN = 1,     /* a1 = the file server's port: forget every package */
    GZ_HW_OP_ADD,           /* a1 = manifest, a2 = its size: register one package */
    GZ_HW_OP_END,           /* the table is complete; retry waiting cards and services */
    GZ_HW_OP_FETCH,         /* a1 = struct gz_hw_request out: the file the kernel wants */
    GZ_HW_OP_SUPPLY,        /* a1 = bytes, a2 = count: the next part of that file */
    GZ_HW_OP_FAIL,          /* the file server can't read it */
    GZ_HW_OP_STATUS,        /* any process: a1 = struct gz_hw_status out, a2 = its size */
};

/* The kernel's current request, if any (GZ_HW_OP_FETCH returns 1, or 0 for none). */
struct gz_hw_request {
    char id[GZ_HW_ID_MAX];
    char path[GZ_HW_PATH_MAX];
    uint32_t size;
};

/* GZ_HW_OP_STATUS: what the kernel holds, for Settings. */
enum { GZ_HW_FILE_IDLE, GZ_HW_FILE_FETCHING, GZ_HW_FILE_READY, GZ_HW_FILE_BAD, GZ_HW_FILE_USED };
#define GZ_HW_STATUS_FILES  16u
struct gz_hw_status {
    uint32_t generation;    /* bumped by each registration */
    uint32_t loaded;        /* the file server has registered the installed packages */
    uint32_t count;
    struct {
        char id[GZ_HW_ID_MAX];
        char path[GZ_HW_PATH_MAX];
        uint32_t size, kind, state;
    } files[GZ_HW_STATUS_FILES];
};

#endif
