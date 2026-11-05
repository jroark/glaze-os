# Network server

Read with the root CLAUDE.md, which covers building, testing and toolchain rules.

**Networking** (`kernel/drivers/ne2000.c`, `servers/net`, `glaze/net.h`, client in `lib/libc/net.c`):
- The NE2000 sits in the PCMCIA card I/O window at 0x0A00C300 once bit 2 of the bridge register 0x0A00A01C is set; its interrupt is GIRQ0 bit 0, which also carries CF state changes. The driver queues received frames in the kernel and notifies the port bound with `SYS_NET_BIND` (`GZ_NOTIFY_NET`); the timer tick polls it too.
- The server runs lwIP (vendored unmodified in `third_party/lwip`, built into `libnetstack.a`) in its tcpip thread on the OS port in `servers/net/sys_arch.c` (semaphores, mailboxes, threads; `LWIP_NETCONN_SEM_PER_THREAD` keeps its semaphore in the thread block). Options are in `servers/net/lwipopts.h`.
- The main thread receives requests and frame notifications (frames go to `tcpip_input`). Calls that finish at once run there on lwIP's sockets; calls that wait (connect, accept, send, recv, resolve, ping, poll) go to worker threads, which make the blocking `lwip_*` call and reply to the sender's token. lwIP's poll bits differ from POSIX's, which the protocol uses.
- Sessions mirror the file server's (8 KiB shared buffer). A timeout of 0 means non-blocking. BSD sockets for apps (`sys/socket.h`, `poll.h`, `netdb.h`, ...) are `lib/libc/socket.c` on top of `glaze/net.h`: an fd is a net socket handle, IPv4 TCP/UDP only.
- lwIP won't route 127.0.0.0/8 out of the Ethernet interface, so the guest can't reach the host's loopback through the NAT; use the host's own address.
- A modem card (`GZ_NET_MODEM`) has no Ethernet interface: `modem.c` dials with AT commands and runs lwIP's PPPoS (PAP/CHAP-MD5, peer DNS) on the kernel's serial byte stream, driven by `NET_DIALUP_*` requests (`glaze/dialup.h`). Test with `tests/ppp_peer.py` and be300 `--modem 127.0.0.1:5555` (`docs/hardware/PCCARDS.md`).
