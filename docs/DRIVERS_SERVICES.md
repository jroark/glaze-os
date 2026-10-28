# Loadable drivers and hardware-dependent services

Glaze keeps optional card drivers and the network service out of active memory
when the socket has no supported network card. A storage CF card does not load
network components. The kernel retains the small socket/CIS enumerator and
network syscall dispatcher, so insertion can be detected without a network
process polling for it.

| Socket | Driver image | `net` service |
|---|---|---|
| Empty or storage card | None | Not started |
| NE2000-compatible network card | `drivers/ne2000.ko` | Started after successful initialization |
| XI-825 | `drivers/xi825.ko` | Started after successful initialization |
| WL54-CF | `drivers/wl54cf.ko` | Started after successful initialization |
| Driver missing, invalid or initialization failed | None retained; socket remains reserved from ATA | Not started |

Only the selected driver is loaded. NE2000 and WL54-CF allocate their shared
packet queue at that time; XI-825 uses its module-private queue. Removal clears
callbacks, masks the card source, discards queued frames, scrubs and frees the
module and packet pages, and stops the network service on the next manager
pass. Interface IRQ handlers do not load modules or create/kill processes.
The kernel lifecycle worker checks socket state and service dependencies every
100 ms of guest time. A replacement gets a new generation and a fresh service.

Display, input, timer, interrupt, raw flash and the storage/socket bootstrap
remain built into the kernel. Audio hardware is always present on a BE-300;
its service defaults to running, but uses the same service registry. This
change converts the three optional network backends to modules.

## Driver format and build

A normal `make` builds `out/drivers/{ne2000,xi825,wl54cf}.ko` and puts them in
romfs. Their implementation files remain in `kernel/drivers/`; wrappers in
`modules/` supply a versioned `glaze_driver` operation table. They are excluded
from the linked kernel. Shipping the files in romfs does not instantiate their
code, BSS or packet buffers; romfs cache pages are reclaimable. GlazeBoot RAM
boots still reserve the whole ROM archive, as they do for unloaded apps.

`kernel/module.c` loads trusted ELF32 little-endian MIPS O32 `ET_REL` objects.
It supports `R_MIPS_32`, `R_MIPS_26` and paired `R_MIPS_HI16`/`R_MIPS_LO16`,
including several HI16 records sharing a LO16. It resolves a small explicit
list of kernel imports, zeros BSS, validates relocations/symbols/ranges, then
publishes code with the existing VR4131 D/I-cache synchronization. Callbacks
must point to aligned instructions inside module text. Invalid images and
failed initialization release all allocations before returning.

Limits are 64 sections, 4,096 symbols, 8,192 relocations per section, 256 KiB
per file and 128 KiB of loaded sections. PIC/GOT, TLS, COMMON, RELA and other
relocation types are unsupported. Modules use the kernel's freestanding
MIPS2/soft-float flags, with `-G0 -mno-abicalls -fno-pic`; they never link libc
or libgcc. Rebuild modules with their matching kernel and bump the private
driver ABI if its structures or contracts change. These are kernel-privileged
images from the factory ROM archive; there is no syscall to load an arbitrary
app or writable filesystem image into the kernel.

To add another card backend, build its object and `glaze_driver` wrapper,
include its `.ko` in `DRIVER_NAMES`, and add its CIS match to the dispatcher.
Storage cards must never fall through to NIC register probing. Required
kernel imports must be explicitly exported by the loader.

## Service catalog

`config/services.json` is compiled by `tools/mkservices.py` into romfs
`etc/services`. The default catalog contains:

```json
[
  {"name":"fs", "path":"bin/fs", "privileges":["flash"], "required":true},
  {"name":"net", "path":"bin/net", "privileges":["network"],
   "requires":["network"], "after":["fs"]},
  {"name":"audio", "path":"bin/audio", "privileges":["audio"], "after":["fs"]}
]
```

Services are ordinary separate ELF programs; apps and services can continue to
use native shared libraries. `fs` and `net` retain their existing static links.
Add a program under `servers/` and a catalog entry to register another service;
the normal build includes server programs automatically.

The catalog supports up to eight services. Names are limited to 15 ASCII
letters/digits/underscores/hyphens, and paths to `bin/<program>` (47 bytes).
`privileges` selects `flash`, `network`, `audio` or `privileged` capabilities.
`requires:["network"]` waits for a successfully initialized network driver.
`after` names live service dependencies: they start first, and their dependents
stop first if they go away. Missing dependencies, cycles, duplicate names,
unknown capabilities and invalid paths fail catalog generation. The kernel
also validates the binary catalog before granting any capabilities.

The worker restarts exited services and retries failed starts, with a minimum
one-second interval between automatic attempts. A required service must start
at boot and cannot be administratively disabled. Runtime policy changes do
not write to persistent preferences; they last for this boot. Hardware driver
loading follows socket presence; administratively stopping `net` leaves an
installed card's driver available for a later service restart.

The catalog is trusted boot policy, not user-writable configuration. Ordinary
app spawning still cannot grant flash, network or audio capabilities.

## Discovery and control

`glaze/sys.h` exposes:

```c
struct gz_service_snapshot snapshot;
gz_services(&snapshot);              /* read-only, available to apps */
gz_service_set("net", 0);            /* privileged: request stop */
gz_service_set("net", 1);            /* privileged: allow start */
```

The snapshot reports service name, process ID, state, start count and last
startup error, plus selected network kind, card generation and allocated
driver/queue pages. An empty socket reports no driver pages and no network
process. Control is deferred to the worker, so it does not destroy a caller's
stack during its syscall. File-service disable requests return `GZ_EPERM`.

Without a card, `net_status()` immediately returns `NET_NO_CARD`, and socket
operations return `GZ_ENETDOWN`; clients do not wait five seconds for an absent
server. After teardown the client releases its stale shared-buffer mapping
and aborts the affected request with `GZ_ENETDOWN`; a later call reconnects.
Network capabilities are tied to the card generation: a still-exiting server
cannot operate a replacement card. Socket handles are opaque, kernel-issued
identities that are never reused
during a boot, so an old handle cannot alias a socket in a replacement server.
Applications still need to recreate connections after a service/card change.
This does not make arbitrary service IPC sessions transparently restartable.

## Validation

Native Linux checks (the module test needs cross-built driver files):

```sh
make -j4 out/drivers/ne2000.ko out/drivers/xi825.ko out/drivers/wl54cf.ko
make -C host module-test net-driver-test service-test
python3 tests/services_catalog.py
```

ASan/UBSan checks the actual driver ELF files, relocation carry, malformed
headers/symbols/relocations, allocation rollback, card selection, failed
initialization, service capabilities, dependency teardown and restart backoff.

The guest lifecycle tests are excluded from factory images unless requested:

```sh
make -j4 SERVICE_TESTS=1 INIT=bin/service_hotplug
# For WL54-CF supply WL54CF=1 and an appropriately licensed firmware pair.
```

Boot a copy of the NAND with the native emulator. Repeat for each card:

```sh
be300 --nand nand.bin --rtc-host-time --speed 0 --ne2000 \
    --net-insert-after 8 --cf-eject-after 25
# Replace --ne2000 with --xi825 or --wl54cf.
```

Require `service-test: offline PASS`, `inserted PASS`, `restart PASS`,
`removed PASS` and `ALL PASS`. The probe checks no resident driver/server while
empty, immediate offline calls, driver selection on insertion, service stop/
restart, unique socket handles, and zero module pages/no process after removal.
`INIT=bin/service_guest SERVICE_TESTS=1` covers an empty or initially populated
socket without requiring hotplug events. Artifacts stay outside Git under
`/workspace/setup/loadable-services/`. Emulator WL54-CF tests use only private
synthetic structural firmware, never vendor firmware.

Full `INIT=bin/ktest` also exercises IPC, memory, filesystem, audio, TCP/UDP/BSD
sockets, Wi-Fi controls and reconnects. Run audio clock checks sequentially on
an idle host with the default `--speed 166` (not `--speed 0`); the DMA follows
host time while the guest uses emulated Count.
Physical VR4131 cache behavior, card insertion/removal and firmware remain
hardware validation work.

Observed emulator results for this change:

- `hot-data-ne2000`, `hot-data-xi825` and `hot-data-wl54cf` pass all lifecycle
  phases, including wireless association where applicable, DHCP before/after
  service restart, stale socket rejection, and zero driver pages/no service
  after removal.
- Full `ktest` reports zero failures with no card, NE2000, XI-825 and WL54-CF
  using the default speed. Logs include `ktest-empty-current`,
  `ktest-ne-current`, `ktest-xi-realtime` and `ktest-wl-realtime`.
- The normal SpringBoard image contains all three module files and the catalog,
  but no `ktest`, `service_guest` or `service_hotplug` program. Its kernel ELF
  defines none of the three backend implementations.
- At SpringBoard readiness (1,205 guest ms), the no-card run reports 14,456 KiB
  available, versus 13,828 KiB with NE2000: a 628 KiB difference. These values
  include reclaimable ROM cache and compare the two hardware configurations,
  rather than a before/after build benchmark (`gui-empty`, `gui-ne2000`).
