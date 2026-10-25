/* Glaze kernel <-> userspace ABI. */
#ifndef GLAZE_ABI_H
#define GLAZE_ABI_H

#include <glaze/types.h>

/* Syscall numbers: v0 = number, a0..a3 = args, result in v0. */
enum {
    SYS_EXIT = 0,
    SYS_YIELD,
    SYS_SLEEP_MS,
    SYS_LOG,
    SYS_SPAWN,
    SYS_KILL,
    SYS_PORT_CREATE,
    SYS_PORT_REGISTER,
    SYS_PORT_LOOKUP,
    SYS_SEND,
    SYS_RECV,
    SYS_REPLY,
    SYS_NOTIFY,
    SYS_MMAP_ANON,
    SYS_SHM_CREATE,
    SYS_SHM_MAP,
    SYS_TIME_US,
    SYS_RTC_GET,
    SYS_RTC_SET,
    SYS_FB_MAP,
    SYS_INPUT_BIND,
    SYS_INPUT_READ,
    SYS_SYSINFO,
    SYS_GETPID,
    SYS_ROMFS_MAP,
    SYS_PROC_ALIVE,
    SYS_FLASH_READ,
    SYS_FLASH_PROG,
    SYS_FLASH_ERASE,
    SYS_NET_INFO,
    SYS_NET_BIND,
    SYS_NET_SEND,
    SYS_NET_RECV,
    SYS_THREAD_CREATE,
    SYS_THREAD_EXIT,
    SYS_GETTID,
    SYS_SEM_CREATE,
    SYS_SEM_WAIT,
    SYS_SEM_POST,
    SYS_SEM_DESTROY,
    SYS_INPUT_STATUS,       /* GZ_INPUT_* capability bits */
    SYS_IMAGE_MAP,
    SYS_IMAGE_RELEASE,
    SYS_IMAGE_SEAL,
    SYS_IMAGE_STATS,
    SYS_IMAGE_QUERY,
    SYS_AUDIO_BIND,
    SYS_AUDIO_START,
    SYS_AUDIO_WRITE,
    SYS_AUDIO_STOP,
    SYS_AUDIO_STATUS,
    SYS_CF_INFO,
    SYS_CF_READ,
    SYS_CF_WRITE,
    SYS_SPAWN_IMAGE,        /* an installed app's ELF from a buffer, unprivileged */
    SYS_AUDIO_MIX,          /* effects mixed over the ring at DMA refill */
    SYS_TASK_SNAPSHOT,      /* atomic, read-only process/performance snapshot */
    SYS_TASK_STOP,          /* end a managed app; system services are protected */
    SYS_VM_MAP,             /* private fixed-address pages for userspace loaders */
    SYS_VM_PUBLISH,         /* publish code; a2 = writable after publication */
    SYS_VM_RELEASE,         /* release a complete SYS_VM_MAP allocation */
    SYS_PROC_BG,            /* a managed app's GZ_PROC_* state (priority, suspension) */
    SYS_SHM_UNMAP,          /* release the caller's mapping at a0 */
    SYS_CHILD_NOTIFY,       /* a0 = own port: GZ_NOTIFY_CHILD when a child exits, 0 = none */
    SYS_FLASH_INFO,         /* struct gz_flash_info: the SYS_FLASH_* region */
    SYS_NAND_READ,          /* the whole NAND, in a RAM boot only */
    SYS_NAND_PROG,
    SYS_NAND_ERASE,
    SYS_MUNMAP_ANON,        /* free a whole SYS_MMAP_ANON allocation at a0 */
    SYS_DISPLAY_SET,        /* privileged: a0 = native brightness, a1 = contrast */
    SYS_DISPLAY_POWER,      /* privileged: a0 = backlight on (0 or 1), CPU/input remain active */
    SYS_WIFI_CTL,          /* PROC_NET only: operation, buffer */
    SYS_COUNT
};

/* Error returns (negative). */
#define GZ_OK           0
#define GZ_EINVAL       (-1)
#define GZ_ENOMEM       (-2)
#define GZ_ENOENT       (-3)
#define GZ_ETIMEDOUT    (-4)
#define GZ_EPERM        (-5)
#define GZ_EDEAD        (-6)    /* peer exited */
#define GZ_EFULL        (-7)
/* File server (glaze/fs.h). */
#define GZ_EEXIST       (-8)
#define GZ_ENOTEMPTY    (-9)
#define GZ_ENOSPC       (-10)
#define GZ_EISDIR       (-11)
#define GZ_ENOTDIR      (-12)
#define GZ_EIO          (-13)
#define GZ_ECORRUPT     (-14)
/* Network server (glaze/net.h). */
#define GZ_ENETDOWN     (-15)   /* no card, or no address yet */
#define GZ_ECONNREFUSED (-16)
#define GZ_ECONNRESET   (-17)
#define GZ_ENOTCONN     (-18)
#define GZ_EHOSTUNREACH (-19)
#define GZ_EINUSE       (-20)   /* address or port in use */
#define GZ_EBUSY        (-21)   /* another call is pending on the socket */
#define GZ_EINPROGRESS  (-22)   /* non-blocking connect started */

/* IPC message: 64 bytes, copied by value. */
#define GZ_MSG_WORDS    16
struct gz_msg {
    uint32_t w[GZ_MSG_WORDS];
};

/* recv() returns a sender token (> 0), or GZ_RECV_NOTIFY with the pending
 * notification bits in msg.w[0], or a negative error. */
#define GZ_RECV_NOTIFY  0

/* Kernel-generated notification bits. */
#define GZ_NOTIFY_INPUT     0x80000000u
#define GZ_NOTIFY_CHILD     0x40000000u
#define GZ_NOTIFY_NET       0x20000000u     /* frames waiting (SYS_NET_RECV) */
#define GZ_NOTIFY_AUDIO     0x10000000u     /* PCM ring below half full */

/* Input events (SYS_INPUT_READ). */
enum {
    GZ_EV_TOUCH_DOWN = 1,
    GZ_EV_TOUCH_MOVE,
    GZ_EV_TOUCH_UP,
    GZ_EV_KEY_DOWN,
    GZ_EV_KEY_UP,
    GZ_EV_TEXT,             /* code = ASCII character; no touch coordinates */
};

enum {
    GZ_KEY_OK = 1,
    GZ_KEY_ESC,
    GZ_KEY_UP,
    GZ_KEY_DOWN,
    GZ_KEY_LEFT,
    GZ_KEY_RIGHT,
    GZ_KEY_HOME,        /* "Rocket" button */
    GZ_KEY_POWER,
    GZ_KEY_ENTER,
    GZ_KEY_TAB,
    GZ_KEY_BACKSPACE,
    GZ_KEY_DELETE,
    GZ_KEY_KBD_HOME,         /* text Home, distinct from Rocket */
    GZ_KEY_END,
    GZ_KEY_PAGEUP,
    GZ_KEY_PAGEDOWN,
};

#define GZ_INPUT_STOWAWAY   0x1u

struct gz_input_event {
    uint16_t type;
    uint16_t code;      /* key code for key events */
    int16_t x, y;       /* screen coordinates for touch events */
    uint32_t time_ms;
};

/* Calendar time (SYS_RTC_GET / SYS_RTC_SET), local time. */
struct gz_datetime {
    uint16_t year;
    uint8_t month, day;     /* 1-based */
    uint8_t hour, minute, second;
    uint8_t weekday;        /* 0 = Sunday */
};

struct gz_sysinfo {
    uint32_t total_pages;       /* all of SDRAM */
    uint32_t free_pages;        /* available to processes, with reclaimable cache */
    uint32_t nprocs;
    uint32_t uptime_ms;
    uint32_t cpu_prid;
    char build[32];
    uint32_t cache_pages;       /* romfs page cache (kernel/romfs.c) */
    uint32_t reclaimable_pages; /* of those, pages no process maps: counted in free_pages */
};

/* Version 1 task monitor ABI. Pass sizeof(struct gz_task_snapshot); older
 * kernels return GZ_EINVAL. CPU time includes kernel work on a task's behalf.
 * Mapped RAM includes shared pages in each process, excludes device/romfs
 * mappings, and must not be summed to measure physical memory usage. */
#define GZ_TASK_MAX 32
#define GZ_TASK_APP  1u
#define GZ_TASK_SELF 2u
#define GZ_TASK_BG   4u
#define GZ_TASK_SUSPENDED 8u

/* SYS_PROC_BG states. An app may suspend itself once in the background;
 * everything else is for privileged callers (SpringBoard). */
#define GZ_PROC_FG          0   /* PRIO_NORMAL */
#define GZ_PROC_BG          1   /* PRIO_LOW, running */
#define GZ_PROC_SUSPENDED   2   /* PRIO_LOW, no thread runs until another state */
struct gz_task_info {
    uint32_t pid, parent, flags;
    uint32_t threads, runnable, mapped_pages;
    uint64_t cpu_us;
    char name[32];
};
struct gz_task_snapshot {
    uint64_t time_us, busy_us;
    uint32_t total_pages, free_pages, count, page_size;
    struct gz_task_info tasks[GZ_TASK_MAX];
};

/* SYS_FLASH_*: the file server's NAND region (11 MiB from 0x500000), or in
 * a RAM boot (glaze/bootinfo.h) a RAM disk of info.blocks blocks with the
 * same page semantics. GZ_FLASH_BLOCKS is the NAND region's size and the
 * largest RAM disk. */
#define GZ_FLASH_PAGE_SIZE      512
#define GZ_FLASH_PAGES_PER_BLOCK 32
#define GZ_FLASH_BLOCKS         704

/* SYS_NAND_*: in a RAM boot, the file server may read every page of the
 * NAND, which still holds Windows CE, and program or erase it if the loader
 * allowed (GZ_FLASH_NAND_RW). Blocks below GZ_NAND_FIRST_WRITABLE (the
 * partition table and the boot loader the boot ROM runs) are never
 * written. */
#define GZ_NAND_BLOCKS          1024
#define GZ_NAND_FIRST_WRITABLE  2

#define GZ_FLASH_RAM            (1u << 0)   /* SYS_FLASH_* is a RAM disk */
#define GZ_FLASH_NAND           (1u << 1)   /* SYS_NAND_READ works */
#define GZ_FLASH_NAND_RW        (1u << 2)   /* SYS_NAND_PROG/ERASE too */

struct gz_flash_info {
    uint32_t blocks;            /* SYS_FLASH_* blocks */
    uint32_t flags;
    uint32_t nand_blocks;       /* SYS_NAND_* blocks, 0 without GZ_FLASH_NAND */
    uint32_t reserved;
};

/* SYS_NET_*: raw Ethernet frames for the network server (PROC_NET). */
#define GZ_ETH_FRAME_MAX    1514    /* without the CRC */

enum { GZ_NET_NONE, GZ_NET_NE2000, GZ_NET_XI825, GZ_NET_WL54CF };

struct gz_netif_info {
    uint32_t present;           /* a network card initialized successfully */
    uint8_t mac[6];
    uint16_t kind;              /* GZ_NET_* */
    uint32_t rx_frames, tx_frames;
    uint32_t rx_dropped;        /* kernel queue full or bad frames */
    uint32_t overruns;          /* card ring overflows */
};

/* Wave output (SYS_AUDIO_*, audio server only). Samples are signed 16-bit
 * little-endian, interleaved when stereo. The hardware plays 8000, 11025,
 * 16000, 22050, 32000 and 44100 Hz. */
#define GZ_AUDIO_RING   65536   /* bytes of PCM the kernel buffers */
/* A second, smaller ring of effects (UI sounds, alerts) in the same format,
 * added with saturation as each DMA buffer is refilled, so they play within
 * a buffer or two however full the main ring is. */
#define GZ_AUDIO_MIX_RING   16384
/* Or'd into SYS_AUDIO_START's channels: output only for the mix ring, so
 * silence from the empty main ring is expected and not an underrun. */
#define GZ_AUDIO_START_IDLE 0x100u

struct gz_audio_status {
    uint32_t running;
    uint32_t rate;
    uint32_t channels;
    uint32_t queued;            /* bytes waiting in the ring */
    uint32_t frames_played;     /* ring frames played since boot */
    uint32_t underruns;         /* buffers padded with silence while running */
    uint32_t mix_queued;        /* bytes waiting in the mix ring */
};

/* SYS_CF_*: a CompactFlash ATA card's 512-byte sectors, for the file
 * server (PROC_FLASH). generation changes whenever the card is inserted,
 * removed or swapped. */
#define GZ_CF_SECTOR    512
#define GZ_CF_IO_MAX    4096    /* bytes per SYS_CF_READ / SYS_CF_WRITE */

struct gz_cf_info {
    uint32_t present;           /* an ATA card answered IDENTIFY */
    uint32_t generation;
    uint32_t sectors;
    char model[44];
};

#define GZ_FB_WIDTH     240
#define GZ_FB_HEIGHT    320
#define GZ_FB_STRIDE    256     /* pixels */

#endif
