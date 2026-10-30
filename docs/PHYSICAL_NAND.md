# Physical NAND artifacts and recovery

Status: 2026-10-30. The physical encoder and an isolated raw repair writer are
implemented. The writer requires a separate `nand=repair` RAM boot and explicit
execution; ordinary read-only boots refuse it. `out/glaze.bin` remains the legacy
16 MiB data-only emulator image and must not be sent to a raw programmer.

## Device evidence

The connected BE-300 was RAM-booted with `nand=ro`. The new `nand-backup`
command read all 32,768 physical pages twice, including every spare byte, and
required identical results before publishing the backup. The completed private
backup is `out/device-backups/be300-20261030.raw` (17,301,504 bytes), SHA-256
`58675491d2e63628b36903ebf878787a7a14c272b5809e824f259ef229c11fd4`.
Its conservative CE tag/ECC inventory quarantines no blocks. This records
current readable allocation metadata; it does not reconstruct factory history
that CE may have overwritten. Keep the original snapshot unchanged and private.

The device accepted the shared protected boot reader's inventory scan. Earlier
forced cold-socket testing mounted its CF card, and the user confirmed touch and
button input. A later boot from CE with no card exposed two additional CF gaps:
hot insertion needed the socket clock/timings/GPIO baseline, and an inherited
COR of zero disabled ATA configuration. Both are fixed. With the card inserted,
the normal RAM build recovers CE's unconfigured socket, mounts the SanDisk
SDCFJ-1024 (977 MB), and reads an existing file via USB.

The initial CE debugger staging fell back to `\Nand Disk\GlazeUSB` while no
CF was present. Those were ordinary CE filesystem files, created before the
snapshot above. No raw NAND programming/erase or boot-block replacement was
performed. `glaze-usb --cf-only boot ...` now refuses that staging fallback.

## Physical v1 layout

The raw image stores 32 pages per physical block, each 512 data + 16 spare bytes.

| Physical blocks | Purpose |
| --- | --- |
| 0–319 | Immutable logical boot blocks; quarantined blocks are preserved |
| 320–1003 | 684-block filesystem arena, including its two BBT copies |
| 1004–1023 | Replacement pool for immutable boot blocks; excluded from the FTL |

Physical block zero must be trusted. Boot blocks that need replacement consume
good blocks from the reserve pool. Every immutable page receives the stock
`AA 55 0F FF` tag, a little-endian logical block ID and ROM-compatible RS parity.
The encoder validates the result by reading every boot page through the shared
strict boot reader before publication. Quarantined blocks retain their original
data and spare bytes exactly; they are excluded from the plan's erase list.

Logical page one contains a CRC-protected `GZNAND1` manifest with fixed geometry,
the absolute physical quarantine bitmap and the inventory digest. The kernel
requires a protected map for this format, rejects invalid manifests, bounds
flash access below the reserve pool, and exposes the bad bitmap to the FTL.
The FTL imports it before writing or erasing anything. One bad BBT copy is
supported; both bad copies fail mount. Legacy data-only images retain 704 blocks.

The v1 source inventory accepts CE-format backups only. Updating an installed
Glaze image also requires importing its runtime BBT; that workflow is not yet
implemented. A fresh physical filesystem is generated for 684 blocks, with the
quarantine bitmap and fresh entropy seed; a legacy filesystem is never truncated.

## Reproduction

Run from the Glaze checkout. Build commands require the existing toolchain
container. Choose new output paths: backup/candidate/plan publication refuses
to overwrite existing files.

```sh
./build.sh all glazeboot recovery
make -C host glaze-usb nand-image-tools

# RAM boot with NAND read-only, then take an exact two-pass backup.
out/host/glaze-usb --cf-only boot out/glazeboot
out/host/glaze-usb nand-backup out/device-backups/device.raw

python3 tools/physical_nand.py inventory \
  --backup out/device-backups/device.raw --out out/device-inventory.json
python3 tools/physical_nand.py pack --logical out/glaze.bin \
  --backup out/device-backups/device.raw --inventory out/device-inventory.json \
  --out out/device-candidate.raw --plan out/device-plan.json
```

The backup syscall is privileged and RAM-boot-only. It returns raw bytes without
correcting them, and is available in read-only and explicit repair boots. The
raw repair protocol is refused in ordinary read-only boots.
A failed/mismatched backup leaves a private `.partial`
file and does not publish a complete backup. The packer binds inventory and
plan to the exact source SHA-256 and writes private files without replacement.

The candidate derived from this device's backup boots through the real ROM in
the sibling emulator, reaches SpringBoard and mounts the 684-block filesystem.
The independent synthetic suite includes bad physical SPL, BBT and data blocks;
one/two symbol correction in the ROM's SPL reader; two-symbol correction in
kernel/romfs; uncorrectable rejection; a valid-ECC/invalid-CRC manifest; and
raw save/restart. Quarantined blocks remain unchanged after guest execution.

```sh
make -C host boot-init-test cf-boot-test nand-ecc-test nand-boot-test \
  ftl-factory-test nand-policy-test nand-backup-test nand-program-test \
  nand-writer-test cf-read-test cf-recovery-tools
python3 tests/physical_nand.py
python3 tests/recovery.py
python3 tests/cf_recovery.py
python3 tests/nand_transaction.py
python3 tests/run_physical_nand.py --artifacts out/physical-suite-new \
  --recovery out/recovery/RECOVER.BIN
ctest --test-dir ../be300/build-host -R 'nand_boot|pmu|piu|lcd' --output-on-failure
```

## CF recovery and remaining gates

`make recovery` produces `out/recovery/RECOVER.BIN`, a stock-ROM `B000FF`
container. The ROM loads a small stub at `0x80200000`, the staged kernel at
`0x80300000` and romfs in high RAM. The stub runs uncached, clears caches, copies
the kernel to its link address and supplies a RAM-root/NAND-read-only bootinfo.
Staging avoids the ROM's live low-memory globals. No NAND I/O occurs in the stub.

A fresh 16 MiB FAT16 superfloppy containing only this file boots in the emulator
with entirely blank NAND, without `--restore`. The kernel reaches SpringBoard,
mounts CF and exposes the USB backup service; NAND remains all `FF` after shutdown.

```sh
tools/mkcfimg.sh out/recovery/recovery-cf.img 16 16 out/recovery/RECOVER.BIN
```

This command creates a local image, not a physical card. The production CF-only
formatter is also available as `glaze-usb cf-format-recovery SECTORS GENERATION`.
It verifies the card identity/geometry, closes its old handles, makes a full-size
FAT16 superfloppy with two FATs and a fixed root, then corrects FatFs's generic
`FAT` label to the literal `FAT16` the ROM requires. It never formats drive 1
(NAND). After this explicit format, one dedicated upload to `/Card/RECOVER.BIN`
is allowed. General USB uploads still reject external storage paths.

The user authorized formatting the inserted 977 MB SanDisk CF card. The card
was formatted and its first recovery payload verified with two identical raw
read-back passes. Both FAT copies matched, the full payload matched, and its
291 clusters were contiguous at data LBA 523. The ROM does not look up the
filename: `9FC01004` scans entries 2..9 of the second FAT's first sector;
`9FC00FE8` returns the stored next-cluster value minus three, rejecting values
at or above `0FF8`. `9FC00E8C` converts that to a starting sector and streams
successive sectors, without following the FAT chain. The previous card had
EOC in all eight scanned entries and could not boot a newly appended file.

The first physical restart left an unresponsive Glaze frame and no USB. This
is not proof of a successful recovery boot: display RAM can retain the old UI.
Removing CF and using Power+Reset returned the device to CE. The revised
recovery payload has a visible green entry marker and `GZ_BOOT_FROM_ROM`:
RAM-root storage no longer implies a warm CE peripheral handoff, so display
and socket cold setup run on CF recovery. Physical verification is ongoing.

The exact captured physical-card prefix also boots in the emulator. Leave
enough trailing sectors in a prefix fixture for the ROM's 64-sector prefetch;
truncating at the last payload byte causes an artificial ATA boundary failure.

Two USB gaps were found during cold testing: emulator cable presence depended
on CE's dock-detection driver, and the Glaze service could accept HELLO before
DTR's control status stage completed, then discard the session at the next
state transition. Cable presence now represents the configured host connection,
and debug input waits for configured+DTR before accepting HELLO.

## Isolated raw install and restore transactions

`kernel/nand_repair.c` accepts only `PROC_USB` in a RAM boot explicitly marked
`GZ_BOOT_NAND_REPAIR`. Neither ordinary read-only boots nor legacy `nand=rw`
enable it. The repair mode disables the `/NAND` filesystem and its logical write
APIs. `glaze-usb` advertises the repair capability only in this mode, refuses
network execution, and requires `nand-apply SOURCE TARGET PLAN --execute`.
No physical repair-mode boot or NAND transaction has been executed.

The writer verifies the source/target SHA-256 and plan CRC, refuses changes to
quarantined blocks, and compares every physical NAND byte to the source before
starting. The kernel stages all 32 raw pages of a block, checks the staged CRC
and rereads the source block CRC before erase. It checks chip status via 70h at
A420 (stock SPL `80F0AAFC/80F0AC3C`, status test `80F0A7E0`), verifies erased
bytes, and programs/read-verifies every data and spare byte. Errors stop the
session; no replacement mapping is guessed. Blocks run in descending physical
order, with block zero last; a final whole-device comparison is required.

The earlier filesystem program/erase paths also now report the chip's failure
bit instead of returning unconditional success. The physical read-only status
diagnostic returned `C0` (ready, no failure, write-enabled); no write was needed
to validate that status interface. Controller completion and NAND chip success
are distinct conditions.

`tools/nand_transaction.py` prepares private, non-overwriting binary plans and
JSON reviews without device access. Installation requires the physical packer's
placement plan and the exact original source. Restoration requires a fresh
two-pass raw snapshot of the current device and targets the original CE backup
byte-for-byte. Interrupted transactions are not resumed blindly: take a fresh
snapshot and prepare a new restore plan. Preserve the original backup separately.
CE may update filesystem metadata when it boots, so regenerate the source-bound
plan after returning to CE; preflight refuses a stale snapshot.

```sh
# Offline preparation only:
python3 tools/nand_transaction.py --source SOURCE.raw --target CANDIDATE.raw \
  --factory-backup SOURCE.raw --inventory inventory.json \
  --placement flash-plan.json --out install.bin
python3 tools/nand_transaction.py --source CURRENT-SNAPSHOT.raw --target SOURCE.raw \
  --factory-backup SOURCE.raw --inventory inventory.json --restore --out restore.bin

# Build a separately labelled repair image; this does not flash or deploy it.
./build.sh recovery-repair
```

`nand-program-test` executes production low-level code against the emulator's
independent NAND model. `nand-writer-test` exercises the native host writer and
production kernel transaction through a simulated wire peer, including exact
restore, source drift, invalid image binding, erase/read-back/final-verify faults,
quarantine preservation and block-zero ordering. Full USB/emulator integration
uses `tests/usb_native.py --nand-transaction-test TARGET INSTALL_PLAN RESTORE_PLAN`.

Before physical programming, prove recovery boot and input/USB on the hardware,
complete a recovery/restore rehearsal, then review the fresh backup-bound plan.
No physical NAND erase or program operation has been performed.
