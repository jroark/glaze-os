# Third-party code

Vendored projects (local changes are listed below):

| Directory | Project | Version | License |
| --- | --- | --- | --- |
| `beetle-pce-fast/` | [Beetle PCE Fast](https://github.com/libretro/beetle-pce-fast-libretro), C HuC6280 CPU, VDC/VCE and PSG for HuCards | `4eee99c451f8` | GPL-2.0-or-later, elected GPLv3 for the app; `COPYING` and `UPSTREAM.md` |
| `totalsms/` | [TotalSMS](https://github.com/ITotalJustice/TotalSMS), Master System / Game Gear CPU/VDP and cartridge mappers; scheduler dependency | `05bbb8bbd931` | MIT; pins and port changes in `totalsms/UPSTREAM.md`; original integer PSG in `packages/mastersystem/psg.c` |
| `clownmdemu/` | [ClownMDEmu](https://github.com/Clownacy/clownmdemu), Genesis CPU/VDP/FM/PSG, Clown68000 and ClownZ80 | `88ef45a65855` | AGPL-3.0-or-later; clowncommon ISC; dependency pins and port changes in `clownmdemu/UPSTREAM.md` |
| `gnuboy/` | [gnuboy](https://github.com/rofl0r/gnuboy), DMG/CGB CPU, graphics and integer APU | `4c2f6c441eb3` | GPL-2.0-or-later, `gnuboy/COPYING`; election in `NOTICE`, local changes in `UPSTREAM.md` |
| `infones/` | [InfoNES](https://github.com/jay-kumogata/InfoNES), CPU/PPU/integer APU and mappers 0–4 | `fe3295c0a86b` | Apache-2.0, `infones/LICENSE`; port changes in `infones/UPSTREAM.md` |
| `littlefs/` | [littlefs](https://github.com/littlefs-project/littlefs) | v2.11 (`6cb4e86540ec`) | BSD-3-Clause, `littlefs/LICENSE.md` |
| `stb/` | [stb_image.h](https://github.com/nothings/stb) image decoder (the browser's PNG, JPEG and GIF) | v2.30 (`2c980bb59875`) | public domain / MIT, end of `stb/stb_image.h` |
| `dhara/` | [Dhara](https://github.com/dlbeer/dhara) NAND translation layer, plus its BCH ECC | `1b166e41b74b` | ISC-style, `dhara/LICENSE` |
| `helix-mp3/` | RealNetworks [Helix](https://github.com/ultraembedded/libhelix-mp3) fixed-point MP3 decoder: `pub/` headers, `real/` C sources, `mp3dec.c`, `mp3tabs.c` | `0a0e0673f82b` (2019 mirror of the 2003 release) | RPSL 1.0 / RCSL, `helix-mp3/LICENSE.txt` |
| `fatfs/` | ChaN's [FatFs](http://elm-chan.org/fsw/ff/) FAT12/16/32 module: `ff.c`, `ff.h`, `diskio.h`, `ffunicode.c` (via the [abbrev/fatfs](https://github.com/abbrev/fatfs) mirror) | R0.16 (`ab992fcf2926`) | 1-clause BSD-style, `fatfs/LICENSE.txt` |
| `wolfssl/` | [wolfSSL](https://github.com/wolfSSL/wolfssl) TLS 1.2/1.3 client: `src/`, `wolfcrypt/src/` and the `wolfssl/` headers they include | v5.9.4-stable (`3c5eead44904`) | GPLv3, `wolfssl/COPYING` and `wolfssl/LICENSING` |
| `hostap/` | [wpa_supplicant](https://w1.fi/wpa_supplicant/) PSK-only WPA/RSN core and internal crypto; port details in `hostap/UPSTREAM.md` | 2.11 | BSD-3-Clause; `hostap/COPYING` |
| `lwip/` | [lwIP](https://github.com/lwip-tcpip/lwip) TCP/IP stack: IPv4 core, Ethernet, and the netconn and sockets APIs (`src/api`) | 2.2.1 (`STABLE-2_2_1_RELEASE`, `77dcd25a7250`) | BSD-3-Clause, `lwip/COPYING` |

InfoNES has the C portability, bounds and CPU correctness changes documented
in `infones/UPSTREAM.md`. Another exception to "unmodified": Helix's `real/assembly.h` has no portable C
path, so its final `#error Unsupported platform` branch is replaced with C
versions of `MULSHIFT32`, `MADD64`, `CLZ` and friends (marked "Glaze").

`fatfs/ffconf.h` is FatFs's configuration template with Glaze's settings,
listed at its top. wolfSSL's configuration is Glaze's
`apps/browser/tls/user_settings.h` (`-DWOLFSSL_USER_SETTINGS`). wolfSSL is
GPLv3, so the browser binary that links it is too.

Only the files Glaze builds are copied (for lwIP, also the headers its core
includes unconditionally, such as `netif/ppp/ppp_opts.h`; for wolfSSL, the
`src/*.c` files that `ssl.c` and `tls.c` `#include`). Glaze's glue
lives in `servers/fs/` (FatFs's disk interface is `card.c`), `servers/net/` (lwIP's `lwipopts.h`, `arch/cc.h`,
and the OS port in `arch/sys_arch.h` and `sys_arch.c`), `servers/audio/`,
`apps/browser/image.c`, `apps/browser/conn.c` and `tls.c`, and `apps/video/`.
