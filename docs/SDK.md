# Glaze application SDK

The SDK builds installable native C apps for Glaze on the Casio BE-300.
It is relocatable and works outside the OS checkout. It contains Glaze's
headers, shared libraries, startup objects, project generator, GNU make
rules, package checks, resource tools, an example and USB client source.
The cross compiler, emulator and OS images are separate dependencies.

## Build the SDK

From the Glaze source tree, using the existing Docker toolchain:

```sh
./build.sh sdk-archive
# Or, with mipsel-linux-gnu-gcc and binutils installed:
make -j4 sdk-archive
```

This produces `out/sdk/glaze-sdk/`, `out/sdk/glaze-sdk.tar.gz`, and the
archive's `.sha256` checksum. `make sdk` exports just the directory;
`make sdk-test` runs the standalone build and validation tests. Building
the SDK builds only its libraries and startup objects, not a NAND image.
Use `OUT` to select another build directory. `DYNAMIC=1` is required.

Extract the archive anywhere, using paths without whitespace (a GNU make
constraint in these rules). `sdk.json` records the source revision and
compiler, and `SHA256SUMS` records the exported files. Use an OS image built
from the same source revision. Glaze does not yet promise a stable app ABI
between releases; a matching SONAME alone does not establish compatibility.

## Host setup

Linux is the reference build host. On Debian/Ubuntu:

```sh
sudo apt-get install gcc-mipsel-linux-gnu binutils-mipsel-linux-gnu make python3
# Optional PNG conversion, CF images, and native USB client:
sudo apt-get install python3-pil mtools dosfstools build-essential pkg-config libusb-1.0-0-dev
```

On macOS, use the repository's `./build.sh sdk-archive` to export the SDK,
then build app projects in the same `mips-cross-dev:latest` container. For
example, if the extracted SDK and a project are in your current directory:

```sh
docker run --rm -v "$PWD":/work -w /work -e GLAZE_SDK=/work/glaze-sdk \
  mips-cross-dev:latest make -C my-app
```

Install native USB prerequisites with `brew install libusb pkg-config`.
The USB client runs on the host, not inside the cross compiler container.
Windows developers can use WSL2 for app builds and the emulator; USB device
access requires forwarding the device into WSL. Native Windows builds have
not been validated.

## First app

After extracting the archive:

```sh
export GLAZE_SDK="$PWD/glaze-sdk"
"$GLAZE_SDK/bin/glaze-sdk" doctor
"$GLAZE_SDK/bin/glaze-sdk" new my-app --id my-app --name "My App"
cd my-app
make -j4
make check
```

Or copy `examples/hello` from the SDK. The generated app has a touch
counter, a procedural icon, and saved state under its installed bundle.
`Info` holds the id, name, version and executable name. `app.h` holds the
title displayed inside the app. `main()` takes no arguments.

Build output:

| File | Purpose |
| --- | --- |
| `build/app.elf` | Executable with debug symbols for disassembly |
| `build/app` | Stripped executable included in the package |
| `build/app.gzp` | Installable package |
| `build/card.img` | Disposable CF image, created by `make cf` |
| `build/run-nand.img` | Disposable NAND copy, created by `make run` |

Add C source files to `SOURCES` in the project's Makefile; subdirectories
are supported. `CPPFLAGS`, `CFLAGS`, `LDFLAGS` and `LDLIBS` accept additions.
For example, `LDLIBS += -l:libmedia.so.1` adds the integer media parsers.
Use `CROSS=...` to select another GNU MIPS tool prefix. Header dependencies
are generated automatically. Run `make clean` after changing compiler flags
or SDK revisions; clean preserves the card and NAND copies because they
can contain saved app data. `make run` recreates these copies on each run.

The compiler uses SDK and compiler freestanding headers, excluding the
host Linux libc headers. All apps use MIPS II, O32, little endian,
soft float, `-G0`, PIC and the Glaze interpreter `/lib/ld-glaze.so.1`.
Do not override those ABI flags or link Linux libraries/libgcc.

## Resources and packages

Files under `res/` are packed recursively, retaining their paths. Load
them with `res_map()` or `res_image()`. SpringBoard supplies the bundle
path before `ui_init()` returns; `res_bundle()` exposes it. The home-screen
icon is `res/icon.gzi`, a 44x44 RGB565 image with optional A8 alpha.
To convert a PNG (requires Pillow):

```sh
python3 "$GLAZE_SDK/tools/mkgzi.py" icon.png res/icon.gzi
make
"$GLAZE_SDK/bin/glaze-sdk" check build/app.gzp
```

Package ids are 1–24 characters, start with a lowercase letter or digit,
and contain lowercase letters, digits, dots or hyphens without `..`.
Names are 1–31 printable ASCII characters, versions 1–15, and executable
names 1–15. SDK packages require all four fields. Up to 64 files fit in
the package table; entry paths are at most 47 ASCII bytes. Symlink
resources, hidden path components and `data/` package entries are rejected.
Use ordinary resource files. `data/` is reserved for the installed app.

The preflight checks cover container ranges, metadata, icon format, ELF
architecture, interpreter, segment layout, SysV hashes and startup library
names. They catch common packaging/build mistakes; they do not replace
the kernel/dynamic loader's full validation or a runtime test. Initial
`DT_NEEDED` dependencies must be supplied by the OS's romfs. Bundling a
new startup library in a package does not make it available to the loader.
For optional libraries/plugins loaded after startup, see
[dynamic libraries](DYNAMIC_LIBRARIES.md).

## Run in the emulator

Build a matching Glaze NAND separately (`./build.sh` in the OS checkout),
and build or obtain the [BE-300 emulator](https://github.com/jroark/be300).
With the app project as the current directory:

```sh
make run GLAZE_NAND=/path/to/glaze.bin BE300=/path/to/be300
```

This creates a 16 MiB FAT16 card containing the package and boots a copy
of the supplied NAND. Unlock SpringBoard, open **Files → Card**, tap
`app.gzp`, install it on flash, then return Home to launch its icon.
`make cf` creates only the card image; it can also be written to a real CF
card using your normal disk-image tools. Hardware buttons and touch are
delivered through the same app event API.

For an unattended boot, set `SDL_VIDEODRIVER=dummy` and
`SDL_AUDIODRIVER=dummy`, use a copied NAND and an orderly SIGTERM shutdown.
`printf`/`puts` go to the emulator console. Input scripts are documented in
the OS source's `CLAUDE.md`; USB scripts are described in [USB](USB.md).

## Deploy to a BE-300 over USB

Build the native client once, using your host C compiler and libusb:

```sh
make -C "$GLAZE_SDK/host"
```

Boot a matching Glaze image with its USB development service enabled,
connect the sync cable and unlock SpringBoard. From the app project:

```sh
make deploy
"$GLAZE_SDK/bin/glaze-usb" logs --once
"$GLAZE_SDK/bin/glaze-usb" screenshot build/screen.png
```

`deploy` verifies the package, uploads to `/Downloads/sdk-app.gzp`, and
asks SpringBoard to install and launch it. Set `GLAZE_USB` to another
client path if needed. Reinstalling the same id preserves its `data/` and
replaces any running copy. See [USB](USB.md) for device selection, input
scripts, RAM boot and reload. RAM boot needs GlazeBoot/OS artifacts and the
separate Windows CE toolchain; those are not part of this app SDK. The
USB guide's `out/host/glaze-usb` is `bin/glaze-usb` in an extracted SDK.

## App APIs and lifecycle

The headers under `sysroot/usr/include/glaze` are the API reference:

| Header | Use |
| --- | --- |
| `ui.h`, `gfx.h`, `res.h` | SpringBoard surface, widgets, fonts, drawing, bundled assets |
| `fs.h` | Files, directories and whole-file helpers |
| `prefs.h` | Small preferences (choose app-specific keys) |
| `audio.h` | Audio playback, streams and sound effects |
| `net.h`, `sys/socket.h`, `netdb.h` | Network status, BSD TCP/UDP sockets and DNS |
| `thread.h` | Threads and kernel-backed mutexes |
| `sb.h` | Badges, notifications and app events |
| `sys.h`, `service.h` | System information, IPC and service snapshots |
| `media.h` | WAV/AVI/MP3 metadata and integer sample conversion |

Call `ui_init(SB_STATUS_GRAY)`, draw into `ui.surf`, invalidate changed
rectangles and call `ui_present()`. The drawable area is 240×304 pixels;
the top 16 screen rows belong to SpringBoard. Touch coordinates received
by apps are already relative to the app surface. Pass RGB888 colors to
drawing functions; the surface stores RGB565. Fonts come from OS resources.

Wait in `ui_next_event()` rather than busy-polling. Process
`GZ_EV_TOUCH_CANCEL` by clearing pressed/drag state without activating the
control. On `GZ_EV_APP_DEACTIVATE`, save state and release shared resources
such as audio streams. The following `ui_next_event()` normally suspends
the process; it resumes with `GZ_EV_APP_ACTIVATE`. A suspended app may be
killed without another callback. Background networking apps can explicitly
opt into `ui_set_background_running(true)` and should block while idle.

Write saves under `<res_bundle()>/data/`, not in `res/` or romfs.
Installation creates a bundle under `/Applications/<id>.app` or
`/Card/Applications/<id>.app`. Updates preserve `data/`; uninstall removes
it. Handle errors (full flash, absent card, no network) in the UI. Network
drivers and the network service load only when supported hardware is
present; socket calls can return `ENETDOWN`.

The device has 16 MiB RAM shared with the OS and all running apps. Bound
caches, release allocations, and avoid large stack objects; each thread's
stack is 64 KiB. Apps run unprivileged. Public headers also describe some
privileged operations for OS services; including them does not grant
framebuffer, raw storage, network device or service-control privileges.

This SDK targets C. Glaze's libc is intentionally small: no POSIX process
model, `fopen`/stdio streams, pthreads, compiler TLS, hardware atomics,
floating-point runtime or C++ runtime. Use `fs_*`, `gz_thread_*` and
`gz_mutex_*`. Do not assume a declaration from a desktop library is available.
The VR4131 has no LL/SC; locks must use kernel semaphores. Installed ELF
files are limited to 2 MiB, with at most 32 loaded objects per process.

## Validation

`make sdk-test` checks an SDK copied to a different location, builds an app
outside the OS tree, rebuilds the included library sources, exercises
header/resource changes, excludes host libc headers, and rejects invalid
packages and incompatible executables. The OS checkout's `tests/t_sdk.txt`
installs and exercises a generated app from a CF image. Build the OS with
`INPUT_SCRIPT=tests/t_sdk.txt`, boot a NAND copy with the app's card image,
and check for `sdk-app: ready count=0`, `count=1` and `count=2` in stdout.
Rebooting the saved copy and repeating the install must show `ready count=2`,
then `count=3` and `count=4`.

On 2026-10-28 these checks passed on the BE-300 emulator, including saved
data across reboot/reinstall and Home/resume. The exported USB client built
natively on Linux. Physical-device deployment and macOS builds were not
retested as part of the SDK work.

## Source and licensing

Glaze code is GPLv3; see the included `LICENSE`. The SDK includes the
corresponding source for its Glaze libraries, startup objects and loader
under `src/`, plus the host client and Python tools' source. Rebuild those
target binaries independently with `make -C src -j4`; output is in
`src/rebuilt/` and does not alter the packaged sysroot. The external GNU
toolchain is not redistributed here. No NAND, firmware or game ROMs are
included in the SDK. Keep ROMs outside the repository.
