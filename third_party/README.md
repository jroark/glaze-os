# Third-party code

Unmodified copies of:

| Directory | Project | Version | License |
| --- | --- | --- | --- |
| `littlefs/` | [littlefs](https://github.com/littlefs-project/littlefs) | v2.11 (`6cb4e86540ec`) | BSD-3-Clause, `littlefs/LICENSE.md` |
| `stb/` | [stb_image.h](https://github.com/nothings/stb) image decoder (the browser's PNG, JPEG and GIF) | v2.30 (`2c980bb59875`) | public domain / MIT, end of `stb/stb_image.h` |
| `dhara/` | [Dhara](https://github.com/dlbeer/dhara) NAND translation layer, plus its BCH ECC | `1b166e41b74b` | ISC-style, `dhara/LICENSE` |
| `helix-mp3/` | RealNetworks [Helix](https://github.com/ultraembedded/libhelix-mp3) fixed-point MP3 decoder: `pub/` headers, `real/` C sources, `mp3dec.c`, `mp3tabs.c` | `0a0e0673f82b` (2019 mirror of the 2003 release) | RPSL 1.0 / RCSL, `helix-mp3/LICENSE.txt` |
| `lwip/` | [lwIP](https://github.com/lwip-tcpip/lwip) TCP/IP stack: IPv4 core, Ethernet, and the netconn and sockets APIs (`src/api`) | 2.2.1 (`STABLE-2_2_1_RELEASE`, `77dcd25a7250`) | BSD-3-Clause, `lwip/COPYING` |

One exception to "unmodified": Helix's `real/assembly.h` has no portable C
path, so its final `#error Unsupported platform` branch is replaced with C
versions of `MULSHIFT32`, `MADD64`, `CLZ` and friends (marked "Glaze").

Only the files Glaze builds are copied (for lwIP, also the headers its core
includes unconditionally, such as `netif/ppp/ppp_opts.h`). Glaze's glue
lives in `servers/fs/`, `servers/net/` (lwIP's `lwipopts.h`, `arch/cc.h`,
and the OS port in `arch/sys_arch.h` and `sys_arch.c`), `servers/audio/`,
`apps/browser/image.c` and `apps/video/`.
