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

/* Input events (SYS_INPUT_READ). */
enum {
    GZ_EV_TOUCH_DOWN = 1,
    GZ_EV_TOUCH_MOVE,
    GZ_EV_TOUCH_UP,
    GZ_EV_KEY_DOWN,
    GZ_EV_KEY_UP,
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
};

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
    uint32_t free_pages;        /* available to processes */
    uint32_t nprocs;
    uint32_t uptime_ms;
    uint32_t cpu_prid;
    char build[32];
};

/* SYS_FLASH_*: the file server's NAND region (11 MiB from 0x500000). */
#define GZ_FLASH_PAGE_SIZE      512
#define GZ_FLASH_PAGES_PER_BLOCK 32
#define GZ_FLASH_BLOCKS         704

/* SYS_NET_*: raw Ethernet frames for the network server (PROC_NET). */
#define GZ_ETH_FRAME_MAX    1514    /* without the CRC */

struct gz_netif_info {
    uint32_t present;           /* an NE2000 card answered the probe */
    uint8_t mac[6];
    uint16_t reserved;
    uint32_t rx_frames, tx_frames;
    uint32_t rx_dropped;        /* kernel queue full or bad frames */
    uint32_t overruns;          /* card ring overflows */
};

#define GZ_FB_WIDTH     240
#define GZ_FB_HEIGHT    320
#define GZ_FB_STRIDE    256     /* pixels */

#endif
