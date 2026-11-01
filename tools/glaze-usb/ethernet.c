/* macOS utun / Linux TAP adapter for the proven framed bulk protocol. */
#include "client.h"
#include "ethernet_packet.h"
#include <glaze/usb_net.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <stdlib.h>
#ifdef __linux__
#include <linux/if_tun.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>

static int address(int ctl, struct ifreq *req, unsigned long op, const char *text)
{
    struct sockaddr_in *a = (struct sockaddr_in *)&req->ifr_addr;
    memset(a, 0, sizeof(*a)); a->sin_family = AF_INET;
    if (inet_pton(AF_INET, text, &a->sin_addr) != 1) return -1;
    return ioctl(ctl, op, req);
}
static int tap_open(const char *name)
{
    if (!*name || strlen(name) >= IFNAMSIZ || strspn(name,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != strlen(name)) {
        fputs("Invalid TAP interface name\n", stderr); return -1;
    }
    if (if_nametoindex(name)) {
        fputs("TAP interface already exists; choose an unused name\n", stderr); return -1;
    }
    int tap = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (tap < 0) { perror("/dev/net/tun"); return -1; }
    struct ifreq req = {0};
    strcpy(req.ifr_name, name);
    req.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_TUN_EXCL;
    if (ioctl(tap, TUNSETIFF, &req) < 0) { perror("TUNSETIFF (needs CAP_NET_ADMIN)"); close(tap); return -1; }
    int ctl = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), error = ctl < 0;
    static const uint8_t mac[6] = GUN_HOST_MAC;
    if (!error) {
        req.ifr_hwaddr.sa_family = ARPHRD_ETHER;
        memcpy(req.ifr_hwaddr.sa_data, mac, sizeof(mac));
        error = ioctl(ctl, SIOCSIFHWADDR, &req) < 0;
    }
    if (!error) { req.ifr_mtu = GUN_MTU; error = ioctl(ctl, SIOCSIFMTU, &req) < 0; }
    if (!error) error = address(ctl, &req, SIOCSIFADDR, GUN_HOST_IP) < 0;
    if (!error) error = address(ctl, &req, SIOCSIFNETMASK, GUN_MASK) < 0;
    if (!error) error = ioctl(ctl, SIOCGIFFLAGS, &req) < 0;
    if (!error) { req.ifr_flags |= IFF_UP; error = ioctl(ctl, SIOCSIFFLAGS, &req) < 0; }
    if (error) perror("Configure USB TAP interface");
    if (ctl >= 0) close(ctl);
    if (error) { close(tap); return -1; }
    return tap;
}
#endif
#ifdef __APPLE__
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#include <sys/wait.h>
#include <net/if.h>
/* Stable Darwin kernel-control ABI (XNU bsd/net/if_utun.h). That kernel
 * header is not shipped in every public macOS SDK. */
#define GUN_UTUN_CONTROL_NAME "com.apple.net.utun_control"
#define GUN_UTUN_OPT_IFNAME 2

static int utun_open(const char *name)
{
    unsigned unit = 0;
    if (name) {
        char *end; unsigned long n;
        if (strncmp(name,"utun",4) || !name[4] || strspn(name+4,"0123456789")!=strlen(name+4)) {
            fputs("macOS interface must be utunN, or omit it for automatic allocation\n",stderr); return -1;
        }
        errno=0; n=strtoul(name+4,&end,10);
        if (errno || *end || n>65534 || if_nametoindex(name)) {
            fputs("Requested utun interface is invalid or already in use\n",stderr); return -1;
        }
        unit=(unsigned)n+1;
    }
    int fd=socket(PF_SYSTEM,SOCK_DGRAM,SYSPROTO_CONTROL);
    if(fd<0){perror("utun socket");return -1;}
    struct ctl_info info={0};strcpy(info.ctl_name,GUN_UTUN_CONTROL_NAME);
    struct sockaddr_ctl addr={0};
    addr.sc_len=sizeof(addr);addr.sc_family=AF_SYSTEM;addr.ss_sysaddr=AF_SYS_CONTROL;addr.sc_unit=unit;
    if(ioctl(fd,CTLIOCGINFO,&info)<0){perror("utun control");close(fd);return -1;}
    addr.sc_id=info.ctl_id;
    if(connect(fd,(struct sockaddr *)&addr,sizeof(addr))<0){perror("utun connect (run with sudo)");close(fd);return -1;}
    char actual[IFNAMSIZ]={0};socklen_t len=sizeof(actual);
    if(getsockopt(fd,SYSPROTO_CONTROL,GUN_UTUN_OPT_IFNAME,actual,&len)<0 || !memchr(actual,0,sizeof(actual))) {
        perror("utun name");close(fd);return -1;
    }
    /* Use fixed argv and an absolute executable: no shell or interpolated
     * commands run with the network administrator's privileges. */
    pid_t pid=fork();
    if(pid==0){execl("/sbin/ifconfig","ifconfig",actual,"inet",GUN_HOST_IP,GUN_DEVICE_IP,
                     "netmask",GUN_MASK,"mtu","1500","up",(char *)NULL);_exit(127);}
    int status=0;pid_t waited=-1;
    if(pid>0)do{waited=waitpid(pid,&status,0);}while(waited<0 && errno==EINTR);
    if(pid<0 || waited<0 || !WIFEXITED(status) || WEXITSTATUS(status)) {
        fputs("Could not configure utun interface\n",stderr);close(fd);return -1;
    }
    int flags=fcntl(fd,F_GETFL);
    if(flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0 || fcntl(fd,F_SETFD,FD_CLOEXEC)<0) {
        perror("utun flags");close(fd);return -1;
    }
    fprintf(stderr,"Created macOS interface %s\n",actual);
    return fd;
}
#endif

/* Also exercised with a nonblocking datagram socket in native tests. */
int gu_usb_net_pump(struct gu_client *c, int tap, int layer3)
{
    uint8_t pending[GUN_FRAME_MAX + 1], ip[GUN_MTU + 5], arp[60];
    unsigned size = 0, arp_size = 0;
    int result = gu_call(c, GU_USB_NET_OPEN, NULL, 0);
    if (result) return -1;
    result = 0;
    while (!usb_cancelled) {
        if (!size) {
            if (arp_size) { memcpy(pending,arp,arp_size);size=arp_size;arp_size=0; }
        }
        if (!size) {
            ssize_t n = read(tap, layer3 ? ip : pending, layer3 ? sizeof(ip) : sizeof(pending));
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { perror("Network interface read"); result = -1; break; }
            if (n == 0) { result = -1; break; }
            if (n > 0) {
                if(layer3) size=gu_eth_from_ip(pending,ip,(unsigned)n);
                else if(n>=14 && n<=(ssize_t)GUN_FRAME_MAX) size=(unsigned)n;
            }
        }
        /* Leave an ID for CLOSE. Never wrap and alias an old exchange. */
        if (c->id >= UINT32_MAX - 1) { result = -1; break; }
        int n = gu_call(c, GU_USB_NET_EXCHANGE, pending, size);
        const uint8_t *reply = c->frame + GU_HEADER;
        if (n < 4 || n > 4 + (int)GUN_FRAME_MAX || (n > 4 && n < 18) ||
            gu_get32(reply) > 1 || (!size && gu_get32(reply))) { result = -1; break; }
        unsigned accepted = gu_get32(reply);
        if (accepted) size = 0;
        if (n > 4) {
            const uint8_t *packet=reply+4;unsigned packet_size=(unsigned)n-4;
            if(layer3) {
                uint8_t next_arp[60];unsigned next_size=0;
                packet_size=gu_eth_to_ip(ip,packet,packet_size,next_arp,&next_size);packet=ip;
                if(next_size){memcpy(arp,next_arp,next_size);arp_size=next_size;}
            }
            int written = 0;
            for (unsigned retry = 0; packet_size && retry < 100 && !usb_cancelled; retry++) {
                ssize_t w = write(tap, packet, packet_size);
                if (w == (ssize_t)packet_size) { written = 1; break; }
                if (w >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
                struct pollfd wait = {.fd = tap, .events = POLLOUT}; poll(&wait, 1, 10);
            }
            if (packet_size && !written && !usb_cancelled) { fputs("Network interface write failed\n", stderr); result = -1; break; }
        } else if (!accepted) {
            /* Bounded idle polling, also lets the device drain RX on full. */
            struct pollfd wait = {.fd = tap, .events = size ? 0 : POLLIN}; poll(&wait, 1, 4);
        }
    }
    /* On Ctrl-C usb_close clears DTR, which tears down the link in-kernel. */
    if (!usb_cancelled && gu_call(c, GU_USB_NET_CLOSE, NULL, 0)) result = -1;
    return result;
}
int gu_usb_net(struct gu_client *c, const char *name)
{
    if (c->usb->network || !(c->caps & GU_CAP_USB_NET)) {
        fputs("USB networking requires a physical USB connection and updated Glaze\n", stderr); return -1;
    }
#ifdef __linux__
    if (!name) name="glzusb0";
    int fd = tap_open(name);
    if (fd < 0) return -1;
    fprintf(stderr, "USB network %s: host " GUN_HOST_IP "/30, Glaze " GUN_DEVICE_IP "; Ctrl-C disconnects\n", name);
    int layer3=0;
#elif defined(__APPLE__)
    int fd=utun_open(name);
    if(fd<0)return -1;
    fputs("USB network: host " GUN_HOST_IP ", Glaze " GUN_DEVICE_IP "; Ctrl-C disconnects\n",stderr);
    int layer3=1;
#else
    (void)name;
    fputs("USB networking currently supports macOS and Linux\n", stderr); return -1;
#endif
#if defined(__linux__) || defined(__APPLE__)
    /* Captured IP packets can contain application credentials. --trace
     * must not turn a network interface into an implicit packet capture. */
    int trace=c->usb->trace;c->usb->trace=0;
    if(trace)fputs("USB network packet payload tracing suppressed\n",stderr);
    int result=gu_usb_net_pump(c,fd,layer3);
    c->usb->trace=trace;close(fd);return result;
#endif
}
