# Third-party code

Vendored projects (local changes are listed below):

| Directory | Project | Version | License |
| --- | --- | --- | --- |
| `jitterentropy/` | [Jitterentropy](https://github.com/smuellerDD/jitterentropy-library) CPU timing-jitter seed collector | 3.6.3 | BSD-style or GPL-2.0-or-later; Glaze elects GPL; port and pin in `UPSTREAM.md` |
| `littlefs/` | [littlefs](https://github.com/littlefs-project/littlefs) | v2.11 (`6cb4e86540ec`) | BSD-3-Clause, `littlefs/LICENSE.md` |
| `pl_mpeg/` | [pl_mpeg](https://github.com/phoboslab/pl_mpeg) MPEG-1 video decoder and MPEG-PS demuxer, for Videos (`apps/video/mpeg.c`) | `c871f2be022e` (2025-12-30) | MIT, `SPDX-License-Identifier` at the top of `pl_mpeg/pl_mpeg.h` |
| `stb/` | [stb_image.h](https://github.com/nothings/stb) image decoder (the browser's PNG, JPEG and GIF) | v2.30 (`2c980bb59875`) | public domain / MIT, end of `stb/stb_image.h` |
| `dhara/` | [Dhara](https://github.com/dlbeer/dhara) NAND translation layer, plus its BCH ECC | `1b166e41b74b` | ISC-style, `dhara/LICENSE` |
| `sbc/` | Broadcom Bluedroid fixed-point SBC encoder (BTstack third-party mirror) | `9edb5d82ea6e`; pin in `sbc/UPSTREAM.md` | Apache-2.0, `sbc/LICENSE` |
| `helix-mp3/` | RealNetworks [Helix](https://github.com/ultraembedded/libhelix-mp3) fixed-point MP3 decoder: `pub/` headers, `real/` C sources, `mp3dec.c`, `mp3tabs.c` | `0a0e0673f82b` (2019 mirror of the 2003 release) | RPSL 1.0 / RCSL, `helix-mp3/LICENSE.txt` |
| `fatfs/` | ChaN's [FatFs](http://elm-chan.org/fsw/ff/) FAT12/16/32 module: `ff.c`, `ff.h`, `diskio.h`, `ffunicode.c` (via the [abbrev/fatfs](https://github.com/abbrev/fatfs) mirror) | R0.16 (`ab992fcf2926`) | 1-clause BSD-style, `fatfs/LICENSE.txt` |
| `wolfssl/` | [wolfSSL](https://github.com/wolfSSL/wolfssl) TLS 1.2/1.3 client: `src/`, `wolfcrypt/src/` and the `wolfssl/` headers they include; also `ed25519.c` and `ge_low_mem.c` for the kernel's OTA signature check (`kernel/crypto/user_settings.h`) | v5.9.4-stable (`3c5eead44904`) | GPLv3, `wolfssl/COPYING` and `wolfssl/LICENSING` |
| `puff/` | [puff](https://github.com/madler/zlib/tree/develop/contrib/puff), zlib's small inflater, for the SPL's OTA install (`boot/ota_apply.c`) | 2.3, from zlib v1.3.1 | zlib license, top of `puff/puff.h` |
| `hostap/` | [wpa_supplicant](https://w1.fi/wpa_supplicant/) PSK-only WPA/RSN core and internal crypto; port details in `hostap/UPSTREAM.md` | 2.11 | BSD-3-Clause; `hostap/COPYING` |
| `lwip/` | [lwIP](https://github.com/lwip-tcpip/lwip) TCP/IP stack: IPv4 core, Ethernet, and the netconn and sockets APIs (`src/api`) | 2.2.1 (`STABLE-2_2_1_RELEASE`, `77dcd25a7250`) | BSD-3-Clause, `lwip/COPYING` |

The emulator cores moved to glaze-emulators with their packages. One exception
to "unmodified": Helix's `real/assembly.h` has no portable C
path, so its final `#error Unsupported platform` branch is replaced with C
versions of `MULSHIFT32`, `MADD64`, `CLZ` and friends (marked "Glaze").

`pl_mpeg/pl_mpeg.h` is unmodified. Videos uses only its demuxer and video
decoder, with symbols hidden so the linker drops the rest; its MP2 decoder
is float, so libmedia's `mp2.c` is a fixed-point port of it instead (with
the standard's dequantization steps, which pl_mpeg approximates). Around
pl_mpeg's own behaviour `mpeg.c` passes over an open GOP's leading B
pictures after a seek, which `plm_seek` decodes against a missing reference
and mislabels, and shows the last reference frame of a stream that ends
with B pictures, which `plm_video_decode` drops. Its timestamps are doubles,
so Videos links the soft-float builtins in `compiler-rt/` (`libsoftfp.a`).
Its demuxer's seek can convert an infinite byte rate to `long`; compiler-rt
saturates, and the host test disables UBSan's float-cast-overflow check.

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
