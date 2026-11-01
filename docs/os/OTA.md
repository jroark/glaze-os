# Over-the-air OS updates

A NAND-booted BE-300 updates Glaze itself: App Store → Updates offers a newer
OS from the update host, downloads it to the flash, has the kernel verify it,
and resets. The SPL installs it before the new kernel loads. An update
replaces the kernel and romfs, which are logical boot blocks 5–319. It never
replaces logical block 0 (the partition table and `GZNAND1` manifest), the SPL
(blocks 1–4, which the masked ROM loads), or the file system.

There's no room for a second copy of the OS: romfs nearly fills its 4.75 MiB
slot, and the 684-block file system holds about 6.4 MB through littlefs on
Dhara. So the update is staged as a compressed file in the file system, and
boot blocks are rewritten in place by the one program that never changes, the
SPL. Every step can be interrupted and resumed.

## The flow

```
App Store ──https──> /Library/Updates/glaze-V.gzu   (size and SHA-256 from os/catalog.txt)
   │ fs_ota_begin/step/arm (FS_OS_UPDATE)
bin/fs ── SYS_OTA_STAGE ──> kernel (kernel/ota_stage.c): Ed25519 signature, version,
   │                        SHA-256 of the table and blocks, the table's block CRCs
   │ syncs open files ─ ARM ─> kernel writes the journal to a reserve block, resets
SPL (boot/ota_apply.c): journal? → read-only littlefs → inflate and check every block,
     the new kernel header and the romfs table → mark started → write each block that
     differs (tags, RS parity, read back) → erase the journal → load the new kernel
```

- **Download.** The App Store's worker streams `os/glaze-V.gzu` into
  `/Library/Updates/.download`, checks its size and SHA-256 against the OS
  catalog, and renames it. The SPL reads only the flash volume, so the update
  never goes to `/Card`. Before asking, the App Store requires the adapter or a
  battery band of at least 50%.
- **Verification.** `fs_ota_begin` makes the file server send the header to
  the kernel, which checks the release key's signature (wolfCrypt's Ed25519,
  `ED25519_SMALL`, built into the kernel with `kernel/crypto/user_settings.h`),
  that the version is newer than the running one, and the SPL's version. Each
  `fs_ota_step` streams the next 64 KiB, and the server answers other clients
  between steps. The last step commits: the digest must match, and the table's
  entries must lie in the file. Until ARM or CANCEL, the file server refuses to
  remove, rename or open the staged file for writing.
- **Arming.** The file server syncs every open file, then the kernel writes
  the journal and resets through PMUCNT2.SOFTRST (`machine_reboot`). Nothing
  writes the file system after the journal, so the staged file stays where
  the SPL will find it.
- **Installing.** The SPL finds the journal during its usual tag scan. It
  mounts littlefs read-only (`servers/fs/ftl.c` with `FTL_READONLY`, littlefs
  `LFS_READONLY`, Dhara with `GF13_NO_TABLES`) and inflates every block
  ([puff](../../third_party/puff)). It checks each one against the journal, and
  the new kernel's header, image CRC and version and the romfs table besides.
  Any problem now rejects the update: the journal is erased, and the installed
  OS boots. Otherwise a marker page records that writing has started. Each
  target block whose contents differ is then erased and programmed with the
  stock tag and RS parity, and read back. A block that fails moves to a free
  reserve block, with the old one's tags cleared. The SPL draws a blue second
  bar under the boot splash while it works, and reloads the power controller's
  150 s watchdog.
- **Reporting.** The SPL leaves `struct gz_spl_info` at PA 0xE800 (its
  version, and `APPLIED` or `REJECTED` with a reason). The kernel logs it
  (`ota: the SPL installed 1.1 (N blocks written)`) and returns it from
  `SYS_OTA_STATUS`. The App Store shows it, and its boot job clears
  `/Library/Updates`.

A power cut repeats the SPL's work on the next boot. Blocks that already
match are skipped, and the staged file hasn't moved. Once writing has
started, a failure stops the boot with a red bar and five beeps. The next boot
tries again, and CF recovery (`docs/hardware/PHYSICAL_NAND.md`) is the last resort.

## Formats (`lib/include/glaze/ota.h`)

- **Kernel slot.** NAND 0x14000 holds a 512-byte `struct gz_kernel_header`
  (`GZK2`: load, entry, size, image CRC, version, build, header CRC). The
  flat kernel follows at 0x14200. The SPL reads these where it used to bake in
  `KERNEL_SIZE` and friends, so it isn't built against a kernel. The
  hibernate record (`kernel/resume.h`) now carries the kernel's text range and
  sum, which the kernel computes just before it hibernates.
- **Update file (`.gzu`).** A 512-byte header (`GZOTA1`: file size, block
  count, minimum SPL, version, build, SHA-256 of the rest, and an Ed25519
  signature over the first 448 bytes). Then a table of `struct gz_ota_block`
  (offset, size, CRC-32 of the 16 KiB block). Then the blocks from the kernel
  slot through romfs, each raw deflate on its own, or stored when that isn't
  smaller. Blocks past the table are erased. A release is about 2 MB, against
  4.9 MB raw.
- **Journal.** One reserve-pool block (1004–1023, not a boot replacement)
  whose first five pages carry the spare tag `GZOJ` in place of
  `AA 55 0F FF`, so neither the ROM's quorum nor Windows CE takes it for a
  logical block. `struct gz_ota_journal` takes pages 0–2 with RS parity: the
  staged path, file size, table offset, block count, version, and a CRC-32
  per target block. Page 5 is the SPL's "started" marker. `nand_boot_scan`
  records the journal's block, and a torn journal with fewer than three good
  tags is ignored. The next arming reuses that block.
- **OS catalog (`os/catalog.txt`).** `GZOS1`, then one `[os]` record:
  `version`, `build`, `package`, `size`, `sha256`, `spl_min` and `notes`
  (`store_os_parse` in `apps/appstore/catalog.c`).

Versions are dotted numbers compared as the App Store compares them. The
`VERSION` file sets the OS version (`GLAZE_VERSION` in the kernel, the kernel
header, and `SYS_OTA_STATUS`). Bump it for every release, since the kernel
installs only newer versions.

## Prerequisites on a device

OTA needs the physical NAND layout (`GZNAND1`) and an SPL with
`GZ_SPL_F_OTA`. `SYS_OTA_STATUS` reports both, and the App Store offers
updates only when they hold. The first OTA-capable SPL goes on once, through
the USB path in `docs/hardware/PHYSICAL_NAND.md`: `tools/nand_transaction.py
--keep-filesystem --installed …`, then `glaze-usb nand-apply …
--keep-filesystem`. After an OTA, the boot blocks no longer match the image
that USB installed, so a later USB update needs a fresh two-pass snapshot as
`--installed-image`. Legacy data-only emulator images (`out/glaze.bin`) have
no reserve pool and report OTA as unsupported.

## Release key

`tools/mkota.py keygen` makes the release key once:

- the private key at `~/.config/glaze/ota-ed25519.pem`, outside the
  repository; keep it backed up and private;
- the public key at `config/ota_pubkey.bin`, which is checked in.

The build embeds the public key in the kernel (`out/gen/ota_key.h`).
Replacing it means devices accept only updates signed with the new key once
they run a kernel that has it. `OTA_PUBKEY=<file>` builds an image that trusts
another key, which is how the tests run.

## Building and publishing a release

```sh
echo 1.1 > VERSION
./build.sh                                  # out/kernel.elf, out/romfs.img
make ota OTA_NOTES="Faster Notes."          # natively: OpenSSL 3 and the key -> out/ota/os/
(cd ../glaze-web && python3 tools/publish_appstore.py --profile linux4be --os ../glaze-os/out/ota)
```

`make ota` builds nothing. It signs the kernel and romfs that `./build.sh`
left, and refuses a ktest image (`--allow-test-image` overrides that).
`mkota.py` uses `openssl pkeyutl -rawin`, so set `OPENSSL` if the first
`openssl` on `PATH` is LibreSSL. The publisher validates the OS catalog and
file, uploads `os/glaze-V.gzu` as an immutable asset, and replaces
`os/catalog.txt` (no-cache) last, as it does for apps.

## Tests

- `make -C host ota-test` (`tests/native/ota.c`, `tests/ota_test.py`) runs the
  kernel's staging and the SPL's apply engine natively, against a NAND model
  and a 684-block file system built by `mkfsimg` that holds the staged file.
  It checks:
  - rejection of a wrong key, tampering, downgrade, reinstall, an old SPL, a
    card path or a wrong size;
  - an install, with the file system untouched;
  - a failing block moving to the reserve pool;
  - a damaged staged file rejected before any write;
  - torn journals;
  - about 120 power cuts across the install's 8000 flash operations, some
    with a second cut while resuming.
- `tests/run_ota.py` runs it in the emulator. It builds a ktest image that
  trusts a throwaway key, and an update to 1.1 with a different romfs, placed
  in the factory volume of a physical image. ktest verifies and arms it
  (`test_ota`). The emulator resets, the SPL installs, and ktest runs again on
  the new romfs. Then it stops the emulator at random points during the
  install and checks that each restart finishes it.
- `make -C host boot-init-test` covers the SPL's hibernate checks with the
  new record fields. `out/host/fstest` covers `ftl.c`, whose read-only build
  shares its code.

## Limits and next steps

- Updating the SPL or the manifest over the air isn't supported. A bad SPL
  write would need CF recovery, so those stay on the USB path.
- The staged file needs about 2 MB free on the flash. The App Store checks
  space before downloading.
- The SPL runs uncached, so an install takes minutes on hardware. Confirm
  the watchdog reload on a real BE-300 with an install longer than 150 s
  before relying on it (`BOOT_BEEP=1` places hangs).
- Kernel verification runs with interrupts off. The Ed25519 check takes a
  moment, and each 8 KiB data call takes a few milliseconds.
