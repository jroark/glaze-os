/*
 * lwIP configuration for the network server: lwIP's tcpip thread and its
 * sockets API on the Glaze OS port (arch/sys_arch.h), IPv4 with ARP, DHCP,
 * DNS, ICMP, UDP and TCP. Options left out keep lwIP's defaults
 * (third_party/lwip/src/include/lwip/opt.h).
 */
#ifndef GLAZE_LWIPOPTS_H
#define GLAZE_LWIPOPTS_H

#define NO_SYS                      0
#define SYS_LIGHTWEIGHT_PROT        1
#define LWIP_SOCKET                 1
#define LWIP_NETCONN                1
#define LWIP_TCPIP_CORE_LOCKING     1
#define LWIP_NETCONN_FULLDUPLEX     1   /* close while another thread waits */
#define LWIP_NETCONN_SEM_PER_THREAD 1
#define LWIP_COMPAT_SOCKETS         0   /* lwip_* names only */
#define LWIP_POSIX_SOCKETS_IO_NAMES 0
#define LWIP_SO_RCVTIMEO            1
#define LWIP_SO_SNDTIMEO            1
#define LWIP_SOCKET_POLL            1
#define LWIP_SOCKET_SELECT          1
#define LWIP_TCP_KEEPALIVE          1
#define SO_REUSE                    1
#define LWIP_ERRNO_STDINCLUDE       1   /* libc's per-thread errno */
#define LWIP_NETIF_API              0

#define TCPIP_MBOX_SIZE             32
#define DEFAULT_RAW_RECVMBOX_SIZE   8
#define DEFAULT_UDP_RECVMBOX_SIZE   8
#define DEFAULT_TCP_RECVMBOX_SIZE   16
#define DEFAULT_ACCEPTMBOX_SIZE     4
#define TCPIP_THREAD_NAME           "tcpip"
#define LWIP_SINGLE_NETIF           1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LOOPBACK         1   /* traffic to our own address */

#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define LWIP_DNS                    1
#define LWIP_IGMP                   0
#define LWIP_AUTOIP                 0
#define LWIP_DHCP_DOES_ACD_CHECK    0   /* no address conflict probing */
#define IP_FRAG                     0
#define IP_REASSEMBLY               1

#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (48 * 1024)
#define MEMP_NUM_PBUF               32
#define MEMP_NUM_RAW_PCB            4
#define MEMP_NUM_NETCONN            24
#define MEMP_NUM_NETBUF             16
#define MEMP_NUM_TCPIP_MSG_API      16
#define MEMP_NUM_TCPIP_MSG_INPKT    32
#define MEMP_NUM_UDP_PCB            8
#define MEMP_NUM_TCP_PCB            16
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_TCP_SEG            64
#define PBUF_POOL_SIZE              24

#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_LISTEN_BACKLOG          1

#define DNS_TABLE_SIZE              4
#define DNS_MAX_SERVERS             2

#define LWIP_STATS                  0
#define LWIP_DEBUG                  0

#endif
