# Kernel, SPL and drivers

Read with the root CLAUDE.md, which covers building, testing and toolchain rules.

**Boot.** The boot ROM loads the SPL (`boot/`) from the B000FF container at NAND 0x4000. The SPL reads the kernel slot's 512-byte header at 0x14000 (`struct gz_kernel_header`, `glaze/ota.h`: load, entry, size, CRC, version), streams the flat kernel from 0x14200, checks its CRC and jumps to it; it isn't built against a particular kernel. `tools/mk_nand.py` packs the image (`--version` from `VERSION`).

**Flash layout.** The SPL loads only the kernel (about 56 KB) from 0x14000. The romfs (`tools/mkromfs.py`: apps, libraries and assets, files page-aligned) sits raw at `ROMFS_OFFSET` 0x40000 up to 0x500000 (`tools/mk_nand.py --romfs` checks the 4.75 MiB slot), and 0x500000 to the end is the file server's region (`FLASH_FS_OFFSET`, `GZ_FLASH_BLOCKS`). There is no other persistent store on the device; a CompactFlash card mounts as `/Card`.

**romfs paging** (`kernel/romfs.c`). Only the archive's entry table is read at boot; its 4 KiB pages come from NAND into a shared page cache on first use, so RAM holds only what is used. `gz_romfs_map` makes a `REGION_ROMFS` mapping with no pages, and a romfs program's read-only shared pages are left unmapped (`image.c`); the first user access takes a TLB fault that `trap()` hands to `as_fault`, which reads the page in and maps it read-only (writes still kill the process), and `uva_to_kva` does the same when a syscall reads such memory first. The kernel reads ELF headers, private (writable or off-colour) pages and the input script through `romfs_read`. A cache frame's colour is its archive offset's bit 12; a per-frame map count pins mapped pages, and the page allocator takes back unmapped ones when it runs out (`romfs_reclaim`), so `gz_sysinfo` counts those as free (`cache_pages`, `reclaimable_pages`). romfs images are shared by entry without being read, and byte images by content.

**Kernel** (`kernel/`):
- Non-preemptive: kernel code runs with interrupts disabled, and user threads are preempted from the timer tick.
- Scheduling is strict priority (high, normal, low, idle) with round-robin within a level.
- Two-level page tables hold PTEs in VR41xx EntryLo format (`mips.h`). The VR41xx TLB layout differs from standard R4000.
- IPC is synchronous send/recv/reply on named ports with 64-byte messages (`struct gz_msg`), plus per-port notification bits. Bulk data goes through shared memory (`SYS_SHM_*`).
- Syscall numbers and the ABI are in `lib/include/glaze/abi.h` (shared with userspace); user wrappers are in `glaze/sys.h`.
- Device access is gated by process flags. `PROC_PRIVILEGED` (framebuffer, input) can be passed down by a privileged spawner. `PROC_FLASH` (raw NAND and the CF card's sectors), `PROC_NET` (Ethernet frames) and `PROC_AUDIO` (wave output) are granted only by the kernel, to `bin/fs`, `bin/net` and `bin/audio`; `sys_spawn` masks other flags.
- The trusted `config/services.json` catalog starts `bin/fs` and `bin/audio`; `bin/net` starts only when a network driver is up. A deferred kernel worker handles hardware dependencies and service restarts. `INIT` (default `bin/springboard`) remains the compositor/test entry point.
- Network backends are separate romfs `drivers/*.ko` ELF32/MIPS ET_REL modules, loaded by CIS identity and released on removal; they are excluded from the linked kernel. See `docs/os/DRIVERS_SERVICES.md`. Use `make -C host module-test net-driver-test service-test`, `python3 tests/services_catalog.py`, and `SERVICE_TESTS=1 INIT=bin/service_hotplug` for lifecycle validation. PC Cards in a PCMCIA jacket (3c589, Hermes, and modems through `kernel/modem.c`) are on a second socket controller (CE's socket 1, windows at 0x0B200000/0x0B600000); `cf.c` selects the socket and drivers address cards through `pcmcia_io_base`/`pcmcia_attr_base` (hw.h), never fixed 0x0A00C000/D000 addresses. Their modules are in romfs only with `PCCARDS=1` (GlazeBoot builds' default). See `docs/hardware/PCCARDS.md`; the emulator models them (`--3c589`, `--hermes`, `--hermes-firmware 6.4`) in the CF socket or, with `--jacket`, in socket 1, which exercises the socket 1 power-up.
- Managed apps have three `SYS_PROC_BG` states (`GZ_PROC_FG`, `GZ_PROC_BG` at `PRIO_LOW`, `GZ_PROC_SUSPENDED`): a suspended process's runnable threads stay off the run queue until another state thaws them (`sched_freeze_proc`). Only a privileged caller sets them, except that an app in the background may suspend itself. A parent that registers a port with `SYS_CHILD_NOTIFY` gets `GZ_NOTIFY_CHILD` when a child exits. `SYS_SHM_UNMAP` drops a mapping (the object is freed with its last one), and shm mappings reuse freed address ranges.
- `SYS_SPAWN_IMAGE` (`gz_spawn_image`) starts an installed app from an ELF in the caller's memory: `image_map_exec` copies it into shared image frames like a `GZ_IMAGE_BYTES` library, `elf_load` serves both it and romfs, and the result is always unprivileged and must use `/lib/ld-glaze.so.1`.
- `tests/bench.py LABEL` boots a launch-script image and ktest a few times headless and writes medians to `out/bench/LABEL.txt` (boot time, free memory, cold/warm app launch, ktest), from SpringBoard's `sb: ...` lines and ktest's `done` line; compare two labels around a change that affects memory or speed.
- Processes may have extra threads (`SYS_THREAD_CREATE`, libc `gz_thread_create`). Each gets a 64 KiB stack in a 64 KiB-aligned slot below the main stack (with an unmapped guard); libc keeps a per-thread block at the stack's base (`gz_self()`: tid, `errno`, TLS slots). The last thread to exit ends the process. Stacks are faulted in a page at a time (`REGION_STACK`); `SYS_MMAP_ANON` still commits at once, and malloc gives requests of 64 KiB or more their own mapping, which `free` returns with `SYS_MUNMAP_ANON`. Synchronization is the kernel's process-private counting semaphores (`kernel/sem.c`, `SYS_SEM_*`, with timeouts); libc's recursive `gz_mutex` sits on them, and malloc and the fs/net clients lock only once a process has a second thread.

**CompactFlash** (`kernel/drivers/cf.c`, `SYS_CF_*`, file server only):
- The card shares the PCMCIA socket with the NE2000; `cf_init()` runs after `ne2000_init()` and only probes when no NE2000 answered. Its CIS (attribute memory at 0x0A00D000, even bytes) must not have a FUNCID other than fixed disk.
- ATA in I/O mode at the secondary IDE ports: taskfile at 0x0A00C170, device control at 0x0A00C376 (`hw.h`). Polled PIO, LBA28, IDENTIFY / READ SECTORS / WRITE SECTORS only, at most 8 sectors per call; the emulator signals no command completion and implements no MULTIPLE commands.
- No hot-plug interrupt: `SYS_CF_INFO` resamples the socket status and re-identifies on a change, bumping `generation`. The emulator's `--cf` and `--ne2000` together put the card in an undecoded slot, so use one or the other.
- Serial cards (FUNCID 2) are never ATA-probed. `capture_card()` gives the exact SMART BCSP card to `kernel/bluetooth.c` and a card whose serial FUNCE names a modem to `kernel/modem.c` (`SYS_MODEM_IO`, `PROC_NET`). Both load the 16550 module `drivers/cfuart.ko` with their own `struct uart_config` (COR address and value, port, divisor, line format). See `docs/hardware/MODEM.md` and `BLUETOOTH.md`.

The built-in piezo (`docs/hardware/BUZZER.md`) is BLG channel `09A0/9A4/9A8`, not
backlight `0980/988/98C`. Its kernel driver is `kernel/drivers/buzzer.c`;
the audio server sequences patterns in `servers/audio/buzzer.c`. Preserve
shared BLG config `0984` and gate `1128` across display power changes.
Use `buzzer-driver-test audio-driver-test audio-volume-test display-test`.

Battery, charging and idle power (`docs/hardware/POWER.md`) follow Windows CE's
gwes.exe battery driver: GIUPIODL bits 12/2, a PIU A/D port sample at
`0x0A000390` from `kernel/drivers/battery.c`'s tick, EEPROM calibration and
the averaging/bands in `glaze/battery.h`. `gz_power_status()` is
size-checked; SpringBoard draws and warns (`servers/springboard/power.c`).
The idle thread runs VR4131 STANDBY (`IDLE_STANDBY=0` disables it). Use
`make -C host battery-test settings-test` and be300's `--battery`,
`--battery-state`, `--battery-drain` through `BE300_ARGS`. Network card power
(802.11 power save, radio off, the socket cut before a sleep, and the card's
time at each level in `gz_power_status` version 2) is `docs/hardware/POWER.md`
"Network cards": `net.c`'s `net_power_note`, `wl54cf.c`, `cf.c`'s
`pcmcia_suspend`; test with `make -C host wl54cf-driver-test net-driver-test`.

Power off (`docs/hardware/POWER.md` "Power off") follows Windows CE's OEMPowerOff:
holding Power (or the Settings → Battery → Sleep delay after the screen goes
off) shows Goodbye, then `SYS_POWER_SLEEP` (`kernel/drivers/power.c`)
suspends or hibernates until Power or the next notification's RTC alarm.
Hibernate resumes through the SPL (`kernel/resume.h`): the boot ROM
overwrites low RAM and probes powers of two, so the kernel saves those pages
and `mm.c` reserves the SPL's; keep this in mind when moving anything in low
RAM. Use `make -C host idle-test boot-init-test`, `tests/ui/t_poweroff.txt`,
`t_hibernate.txt`, `t_poweroff_alarm.txt` and `HIBERNATE_VERIFY=1`.

OTA updates (`docs/os/OTA.md`, `glaze/ota.h`) replace the kernel and romfs
(logical boot blocks 5-319) on a physical NAND image. App Store → Updates
downloads a signed `.gzu` (per-block deflate) to `/Library/Updates`; the
file server streams it to the kernel (`FS_OS_UPDATE`, `SYS_OTA_STAGE`,
`kernel/ota_stage.c`: Ed25519 with the key in `config/ota_pubkey.bin`,
SHA-256, block CRCs), which writes a `GZOJ` journal to a reserve block and
resets; the SPL (`boot/ota_apply.c`, read-only `ftl.c` with
`FTL_READONLY`, puff) installs it, resuming after any power cut. The SPL is
never updated over the air. `VERSION` is the OS version; bump it per
release. `make ota` signs `out/ota/os/` natively with
`~/.config/glaze/ota-ed25519.pem` (never commit it); `OTA_PUBKEY=` builds
an image trusting a test key. Use `make -C host ota-test boot-init-test`,
`out/host/fstest` after `ftl.c` changes, and `tests/run_ota.py` (emulator, power
cuts).
