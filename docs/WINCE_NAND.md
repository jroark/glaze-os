# Launch Windows CE apps directly from NAND

After starting Glaze through USB or GlazeBoot, open **Files → NAND → Program
Files**, then tap a supported `.exe` or `.exe.cpk` application. The built-in CE
runner loads the selected executable and its DLLs from the original volume;
there is no host-side package conversion or installation step.

Use the normal `nand=ro` configuration. `nand=off` hides the original volume and
prevents metadata reads. GlazeBoot's default app selection now includes `files`
and `wince`, alongside App Store, Browser, and Settings. Custom `GB_APPS` builds
must include both. Rebuild `glazeboot` and transfer the updated kernel and ROMFS
for an existing USB/GlazeBoot installation. A normal Glaze NAND installation
replaces Windows CE and does not provide this original-volume launch path.

## What runs

The existing userspace PE32/MIPS compatibility layer executes applications that
use its supported CE API subset. This launch method adds source discovery,
compressed assets, DLL lookup, and original data-file access; it does not add
arbitrary Windows CE kernel services or hardware drivers. ARM/x86 executables,
unsupported imports, and malformed binaries produce a launch/runtime error.

In the supplied local `300.bin`, the original compressed Calculator and its
`LC.dll` load through this path and render the original UI. The synthetic
unpacked PE test also launches from Files and returns its expected status.
Other applications retain the compatibility limits in [WINCE.md](WINCE.md).
Physical hardware and the full stock-CE-to-Glaze USB/GlazeBoot transport were
not rerun for this change; the guest integration test exercises the existing
RAM-boot trampoline and its NAND mount.

## Sources and writable state

Each source path gets a stable, collision-checked context beneath
`/Library/WinCE/<hash>-<collision>.app`. SpringBoard starts `bin/wince` with that
context, tracks the process in the normal app/switcher lifecycle, and reuses an
already running instance. The context stores a source-path record and private
registry/file state. Removing its Glaze icon removes this context, not the
original NAND application. Direct-launch icons last for the current Glaze
session; Files can recreate them from the source after a restart.

The CE executable sees its original `\Nand Disk\...` module path, with `.cpk`
removed. DLL lookup checks the executable's folder, `/NAND/Windows`, then
`/NAND/Program Files`, including compressed variants. Runtime built-in API
adapters continue to handle supported CE services.

Reads of original CE paths fall back to `/NAND`; `\Documents` and
`\My Documents` use `/NAND/My Documents` when no Glaze copy exists. Directory
searches merge the private overlay and original entries, exposing compressed
files under their decompressed names. Compressed data files are decoded into
the private context as needed. Writes use Glaze's existing private drive or
Documents storage, including copying an original file before modifying it.
Source executables, libraries, NK metadata, and original data remain unchanged.
The original CE registry is not imported or modified.

The default RAM-boot `/` is a 2 MiB RAM disk. Contexts, settings, decoded data,
and saved Documents on that disk disappear on reset; sufficiently large decoded
data files can exceed its capacity. Copy documents to CF before resetting when
you need to retain them. Existing packaged CE apps keep their package resources
and state behavior.

## Compression and ROM metadata

`FS_NAND_READ` provides bounded read-only access to the original logical NAND
through the file server. The existing kernel path still enforces RAM boot and
`nand=off`, maps CE logical blocks, and corrects spare-area ECC. No application
receives raw NAND program/erase access through this interface.

The first compressed application or ordinal import recovers the matching
COREDLL/COMMCTRL export names and CCFS Huffman dictionary from that device's
Casio LZSS/B000FF NK partition. The reader validates partition/record ranges,
ROMHDR/TOC entries, section mappings, exports, and dictionary references. It
records NK record checksum mismatches: captured restore streams can have stale
checksums, and the supplied dump has four. NK code is not executed. Invalid
metadata fails with a diagnostic rather than using an ordinal map from another
firmware.

Recovery temporarily holds the decompressed NK image (about 6 MiB for the
supplied dump, bounded at 8 MiB). The progress screen remains under SpringBoard's
normal Home/foreground control. Close other large applications if memory is
insufficient. The temporary NK buffer is released before loading the compressed
program. A small checksummed metadata cache in `/Library/WinCE/rom.cache` serves
subsequent apps in the same RAM boot; it is generated locally and is not shipped
in ROMFS. Its key includes the partition table and boundary pages. The cache
assumes the original read-only NAND stays unchanged during that RAM boot.

The native CCFS decoder implements block kinds 0–16 with bounded inputs,
outputs, bit references, and Huffman lookups. Decompressed programs/data are
limited to 4 MiB. Registry defaults and application-specific data requirements
still depend on the existing compatibility layer.

## Validation

```sh
make -C host wince-test
make -C host wince-nand-test
WINCE_NAND=/external/300.bin make -C host wince-nand-test
make -j4 out/glaze.bin glazeboot
```

The native ASan/UBSan tests use a generated NK/export/dictionary fixture and
cover every CPK block kind plus malformed ranges and truncated streams. The
optional local NAND checks compare Calculator, LC.dll, and Calendar against the
independent Python CCFS decoder. No vendor bytes are required by the default
tests or stored in Git.

Inside the cross-toolchain environment:

```sh
python3 tests/run_wince_nand.py --seconds 24
python3 tests/run_wince_nand.py --image /external/300.bin --relaunch --seconds 75
```

The runner creates a disposable 16 MiB test NAND under ignored `out/`, with
separate Glaze, original NK, and FAT partitions. It uses GlazeBoot's real
trampoline handoff and default app selection, then drives Files to the NAND
application. The first command uses a generated executable; the second copies
only the supplied Calculator/LC compressed files and matching NK partition into
the fixture. It verifies entry/exit or app startup and hashes the source
partitions before/after. `--relaunch` also checks metadata-cache reuse; the
Calculator input script enters `2 + 3 =` on both launches. It does not modify the supplied image. Logs and
screenshots are retained under `out/wince-nand-synthetic` or
`out/wince-nand-original`. Rebuild normally afterwards to remove scripted input.
