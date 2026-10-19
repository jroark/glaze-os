# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Glaze is a microkernel OS for the Casio BE-300 (NEC VR4131 MIPS, 240x320 RGB565 LCD, 16 MiB SDRAM, 16 MiB small-page NAND) with an early-iPhone-OS-style UI. It boots through the stock boot ROM from a NAND image on the [be300 emulator](https://github.com/jroark/be300).

## Building

```sh
./build.sh                        # Docker (mips-cross-dev image) -> out/glaze.bin
make -j8                          # same, natively, if mipsel-linux-gnu-gcc is installed
make -j8 INIT=bin/ktest           # boot the kernel self-test instead of SpringBoard
make -j8 INPUT_SCRIPT=tests/x.txt # replay scripted input (see Testing)
make -C host fstest               # native build of the file-system stack test
```

There is no linter; the build uses `-Wall -Wextra` and should stay warning-free. `compile_flags.txt` is for clangd.

`main.o`, `input.o` and `romfs.img` are always rebuilt because `INIT`, `BUILD_ID` and `INPUT_SCRIPT` are baked into them. Every directory under `apps/` and `servers/` becomes a program, `bin/<dirname>` in romfs, with no Makefile edit. Assets in `assets/gen/` are checked in; they are regenerated only on macOS with `swift tools/mkassets.swift assets/gen assets/fonts userdata/Pictures`.

Romfs is for the OS and built-in apps only. User files, settings and app data belong in the file system: `userdata/` is the factory volume, which `host/mkfsimg` (built with `HOSTCC`, default `cc`) formats into `out/fs.img` and `tools/mk_nand.py --fs-image` places at 0x500000. Don't name files in `userdata/` with spaces, since make lists them.

## Running and testing

```sh
be300 --nand out/glaze.bin --rtc-host-time > stdout.log 2> stderr.log
be300 --nand out/glaze.bin --rtc-host-time --ne2000   # with the PCMCIA Ethernet card
```

- The emulator writes NAND back into the image, so boot a copy when you need a clean flash. Kernel and `printf` output goes to the emulator's stdout. A screenshot (`screenshot_*.bmp`) is saved on orderly shutdown, including SIGTERM from `timeout`. With no display, `SDL_VIDEODRIVER=dummy` runs it headless and still saves the screenshot.
- **ktest** (`apps/ktest`): build with `INIT=bin/ktest`, boot, and grep stdout for `ktest: FAIL` / `ktest: ALL PASS`. It covers IPC, shm, sleep, faults, threads and semaphores, the file server and preferences, and with `--ne2000` the network: DHCP, ping, DNS through the NAT, TCP/UDP over lwIP's loopback through both `glaze/net.h` and BSD sockets. Boot it both with and without `--ne2000`; without a card the network cases print `SKIP`. It prints `INFO` for an internet fetch it can't require.
- **Scripted UI tests**: `tests/*.txt` replay timed events (`<ms> tap x y`, `drag ...`, `key ok|esc|...|home`) from inside the kernel, in screen coordinates. `tests/run.sh tests/t_x.txt` builds, boots and converts the final screenshot, but it uses the macOS tools `gtimeout` and `sips`. Scripted events bypass the touch panel and its interrupts.
- **Real touch path**: `tests/touch.sh tests/touch_unlock.act` drives the emulator window with xdotool on an X display (`Xvfb :99 &`, then `DISPLAY=:99`). Action coordinates are window/screen pixels; app surfaces start 16 px lower, under the status bar.
- **Flash stack**: `host/fstest [seed [wear|flips|bad|cuts|format]]` runs `servers/fs/ftl.c` plus littlefs and Dhara on a RAM NAND model with bit flips, failing blocks and power cuts. A full run takes several minutes; pass a phase name to run one.

## Toolchain constraints

- Everything is built `-march=mips2 -mabi=32 -mno-abicalls -fno-pic -G 0 -msoft-float`, freestanding and static. The VR4131 is MIPS III without CLZ or MIPS32 SPECIAL2 ops, and the emulator executes 32-bit code only.
- Nothing links libgcc. Compiler helpers live in `lib/libc` (`divdi3.c` for 64-bit division, `bitops.c` for `__popcountsi2`, `__ctzsi2`, `__clzsi2` and `__bswapsi2`). An "undefined reference to `__…si2`" means another helper is needed there.
- The libc is minimal (`lib/libc/include`). Check it has a function before using it, or add it to `lib/libc/string.c` and similar. The kernel compiles `lib/libc/string.c` and `divdi3.c` too.
- The VR4131 has no LL/SC (VR4131 UM: "does not support LL, LLD, SC, SCD"), so there are no userspace atomics: every lock is a kernel semaphore (`gz_mutex` in `glaze/thread.h`).

## Architecture

**Boot.** The boot ROM loads the SPL (`boot/`) from the B000FF container at NAND 0x4000. The SPL streams the flat kernel from 0x14000 to 0x80010000 and jumps to it. The SPL bakes in the kernel's load, entry and size from `tools/elf_consts.py`; `tools/mk_nand.py` packs the image.

**Flash layout.** 0x500000 to the end is the file server's region (`FLASH_FS_OFFSET`, `GZ_FLASH_BLOCKS`). There is no other persistent store. The romfs (apps and assets) is linked into the kernel image itself (`kernel/romfs_blob.S`, `tools/mkromfs.py`), and files in it are page-aligned so they can be mapped into user space.

**Kernel** (`kernel/`):
- Non-preemptive: kernel code runs with interrupts disabled, and user threads are preempted from the timer tick.
- Scheduling is strict priority (high, normal, low, idle) with round-robin within a level.
- Two-level page tables hold PTEs in VR41xx EntryLo format (`mips.h`). The VR41xx TLB layout differs from standard R4000.
- IPC is synchronous send/recv/reply on named ports with 64-byte messages (`struct gz_msg`), plus per-port notification bits. Bulk data goes through shared memory (`SYS_SHM_*`).
- Syscall numbers and the ABI are in `lib/include/glaze/abi.h` (shared with userspace); user wrappers are in `glaze/sys.h`.
- Device access is gated by process flags. `PROC_PRIVILEGED` (framebuffer, input) can be passed down by a privileged spawner. `PROC_FLASH` (raw NAND) and `PROC_NET` (Ethernet frames) are granted only by the kernel, to `bin/fs` and `bin/net`; `sys_spawn` masks other flags.
- The kernel spawns `bin/fs`, `bin/net` and then `INIT` (default `bin/springboard`) at boot.
- Processes may have extra threads (`SYS_THREAD_CREATE`, libc `gz_thread_create`). Each gets a 64 KiB stack in a 64 KiB-aligned slot below the main stack (with an unmapped guard); libc keeps a per-thread block at the stack's base (`gz_self()`: tid, `errno`, TLS slots). The last thread to exit ends the process. Synchronization is the kernel's process-private counting semaphores (`kernel/sem.c`, `SYS_SEM_*`, with timeouts); libc's recursive `gz_mutex` sits on them, and malloc and the fs/net clients lock only once a process has a second thread.

**Input.** Touch comes from the VRC4173 PIU, through GIRQ0 to GIU line 0 to IP2 (`kernel/drivers/irq.c`, `input.c`); buttons are polled and also arrive on GIRQ0. Events go into a kernel ring that only the bound compositor drains. SpringBoard forwards events to the foreground app through the shared ring in `struct sb_shared`. `ui_next_event` coalesces consecutive moves.

**SpringBoard ↔ app protocol** (`glaze/sb.h`, `lib/ui/app.c`):
- An app sends `SB_HELLO` and receives a shared-memory 240x304 surface; the top 16 rows of the screen are SpringBoard's status bar.
- The app draws into the surface and calls `ui_present()`, which blocks until the dirty rectangle is on screen.
- The Home button kills the app, so apps must save state eagerly (Notes debounces its saves).
- The home-screen app list is the `sb_apps[]` table in `servers/springboard/draw.c`; icons are `assets/gen/icon_*.gzi`.

**Networking** (`kernel/drivers/ne2000.c`, `servers/net`, `glaze/net.h`, client in `lib/libc/net.c`):
- The NE2000 sits in the PCMCIA card I/O window at 0x0A00C300 once bit 2 of the bridge register 0x0A00A01C is set; its interrupt is GIRQ0 bit 0, which also carries CF state changes. The driver queues received frames in the kernel and notifies the port bound with `SYS_NET_BIND` (`GZ_NOTIFY_NET`); the timer tick polls it too.
- The server runs lwIP (vendored unmodified in `third_party/lwip`, built into `libnetstack.a`) in its tcpip thread on the OS port in `servers/net/sys_arch.c` (semaphores, mailboxes, threads; `LWIP_NETCONN_SEM_PER_THREAD` keeps its semaphore in the thread block). Options are in `servers/net/lwipopts.h`.
- The main thread receives requests and frame notifications (frames go to `tcpip_input`). Calls that finish at once run there on lwIP's sockets; calls that wait (connect, accept, send, recv, resolve, ping, poll) go to worker threads, which make the blocking `lwip_*` call and reply to the sender's token. lwIP's poll bits differ from POSIX's, which the protocol uses.
- Sessions mirror the file server's (8 KiB shared buffer). A timeout of 0 means non-blocking. BSD sockets for apps (`sys/socket.h`, `poll.h`, `netdb.h`, ...) are `lib/libc/socket.c` on top of `glaze/net.h`: an fd is a net socket handle, IPv4 TCP/UDP only.
- lwIP won't route 127.0.0.0/8 out of the Ethernet interface, so the guest can't reach the host's loopback through the NAT; use the host's own address.

**UI libraries.** `libgfx` (`lib/gfx`) is a software RGB565 renderer for GZI images and GZF fonts; the formats are described in `lib/include/glaze/gfx.h`. `libui` (`lib/ui`) provides the nav bar, bar buttons, grouped cells, alerts, the keyboard, inertial scroll views and push/pop transitions. Apps follow the Settings/Notes pattern: draw the whole view, keep hit rects, `redraw()` after events, and use `ui_transition(draw_fn, forward)` for navigation. `host/` builds `libgfx` natively to render SpringBoard previews.

**File system** (`servers/fs`, `glaze/fs.h`, client in `lib/libc/fs.c`):
- littlefs runs on the Dhara NAND translation layer, both vendored unmodified in `third_party/` and built into `libfsstack.a`. Don't edit them; put changes in `servers/fs/ftl.c`.
- ECC is in-band: the spare area is synthesized by the emulator and absent from image files. Each page gets a 4-bit BCH code stored in the unused tail (bytes 480–511) of the checkpoint page Dhara writes after every 3 data pages.
- The rest of `ftl.c`'s design is documented in its header comment and depends on Dhara's journal layout (`log2_ppc == 2` for 512-byte pages): the ECC cache, the bad-block table in the region's first two blocks, read-back verification, and torn-checkpoint handling. Re-run `host/fstest` after any change to `ftl.c`.
- littlefs's block count is fixed from the geometry, not from `dhara_map_capacity`, which changes between mounts.
- Clients get a session with an 8 KiB shared buffer; paths and data travel in it.
- Layout: `/Documents`, `/Pictures` (Photos' Camera Roll), `/Library/Preferences` (settings, one file per key, via `glaze/prefs.h`), `/Library/Notes`, `/Library/Caches` (rebuildable). A blank or erased volume gets only the folders in `servers/fs/folders.h`.
- Reads are slow, about 1.5 s for a 150 KB picture in the emulator, so apps should cache derived data (Photos keeps thumbnails in `/Library/Caches/Photos`) and load in the background.

**Browser** (`apps/browser`, "Browser" on the home screen):
- The engine (`web.h`: `html.c`, `css.c`, `style.c`, `layout.c`, `paint.c`, `image.c`, `forms.c`, `page.c`) uses only libc and libgfx, so `make -C host browse` builds it natively (with ASan/UBSan): `host/browse <url | file.html> [out.bmp]` renders a page to a 240 px wide BMP, and `BROWSE_DUMP=1` prints the styled tree. `tests/web/basic.html` exercises it.
- Engine text is Latin-1 plus libgfx's extra glyphs (`charset.c` maps everything else); the fonts cover 0xA0-0xFF. CSS px are laid out at 3/4 device px, so the screen is 320 CSS px wide. Layout is integer-only; wide tables are linearized; floats and positioning are ignored.
- Network: `conn.c` (BSD sockets), `http.c` (HTTP/1.1, chunked, gzip via stb's zlib), `loader.c` (queue, redirects, cookies, cache), `load.c` (document, then CSS, then images). No TLS: https fails with a "not supported" page. `plat.h` is what differs between `main.c` (device) and `host/browse.c`.
- `res:` URLs are the bundled pages in `assets/web` (romfs `web/`); the start page is `res:start.html`. State persists in `/Browser/state` and `/Browser/bookmarks`.
- Images go through vendored `stb_image.h` (PNG, JPEG, GIF), downscaled at decode time; a page's decoded images are capped at 3 MiB.

## Hardware notes

Register addresses and bit meanings follow the VR4131 and VRC4173 user's manuals and the be300 emulator's models (the `be300` and `mipsel-cross-image` repos hold the manuals, hardware dumps and the Linux port). Cite the manual section in comments when touching hardware code. Some things are known not to match real hardware yet:
- **D-cache colouring:** the virtually indexed D-cache can give a page's kernel and user mappings different cache colours. The emulator doesn't model cache contents (TODO in `kernel/mm.c`).
- **Factory bad-block marks:** these sit in the spare area and are never read.
- **PCMCIA card setup:** the NE2000 driver assumes the card already decodes I/O at port 0x300, as the emulator's does. A real card needs its CIS read and its configuration option register written through attribute memory first.
