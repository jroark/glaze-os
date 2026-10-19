# Third-party code

Unmodified copies of:

| Directory | Project | Version | License |
| --- | --- | --- | --- |
| `littlefs/` | [littlefs](https://github.com/littlefs-project/littlefs) | v2.11 (`6cb4e86540ec`) | BSD-3-Clause, `littlefs/LICENSE.md` |
| `dhara/` | [Dhara](https://github.com/dlbeer/dhara) NAND translation layer, plus its BCH ECC | `1b166e41b74b` | ISC-style, `dhara/LICENSE` |
| `lwip/` | [lwIP](https://github.com/lwip-tcpip/lwip) TCP/IP stack: IPv4 core, Ethernet, and the netconn and sockets APIs (`src/api`) | 2.2.1 (`STABLE-2_2_1_RELEASE`, `77dcd25a7250`) | BSD-3-Clause, `lwip/COPYING` |

Only the files the servers build are copied (for lwIP, also the headers its
core includes unconditionally, such as `netif/ppp/ppp_opts.h`). Glaze's glue
lives in `servers/fs/` and `servers/net/` (lwIP's `lwipopts.h`,
`arch/cc.h`, and the OS port in `arch/sys_arch.h` and `sys_arch.c`).
