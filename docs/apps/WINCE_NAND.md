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

The userspace PE32/MIPS compatibility layer executes the original
applications' own code. This launch method adds source discovery, compressed
assets, DLL lookup (including Casio's libraries in the NK ROM, below), the
device's registry, and original data-file access. It does not add arbitrary
Windows CE kernel services or hardware drivers. ARM/x86 executables,
unsupported imports and malformed binaries produce a launch or runtime error.

Every executable in the stock BE-300 image's `Program Files` (the local
`300.bin`) starts from Files in the emulator and shows its original UI.
`tools/wince_audit.py --image <nand>` lists, for each one, any import that
nothing provides. Only GetDisk's CE database calls remain; they back an alarm
mirror that the runtime bypasses (see "System services").

| Application | State |
| --- | --- |
| Calculator (`calc.exe`) | Original keypad and `LC.dll` engine. |
| Calendar, Contacts, Tasks, Notes | Original NK `pimdb.dll`. Creating an appointment, contact, task or note works (`tests/ui/t_wince_nand_*.txt`); records are the original files under `\My Documents`. |
| Mail | Original NK `NFC.dll`/`MailData.dll` and Program Files `SendRecv`/`MMLfor*` load; folders, menus and the account dialog work. Sending and receiving against a server is not validated. |
| Internet (WorldTALK) | Lays out its Welcome page (about 40 s in the emulator); Winsock fetches pages. |
| Clock | Home/Visiting displays; alarms become Glaze notifications. |
| Control Panel | The applet list from the device's registry, with the applets' icons; a tap starts the applet as its own Glaze app. |
| Contrast | Contrast and brightness are Glaze's display settings. |
| Sound | Volume and click/tap sounds are Glaze's sound settings. |
| Power | Battery status from Glaze's battery driver. |
| Color Scheme (`ViewDesign`) | System colours apply to every CE app and persist. |
| Owner Information, Memory | Run; Memory's figures are nominal. |
| Password | Glaze's passcode (four digits); other passwords are refused with an explanation. |
| Align Screen (`TapAdjust`) | Glaze's five-point touch calibration. |
| Connections, Dial-up | The device's RAS phonebook; dialing uses Glaze's CF modem and PPP. Without a modem, a dial reports Glaze's existing network connection. |
| Backup | Runs. Raw NAND access and safe-mode reboot are refused. |
| Help (`PegHelp`), myCasio introduction | Run (Help needs a help file argument, as on CE). |
| Installers and sync helpers (`CoInstall`, `CoSubInstall`, `GetInstallInfo`, `CoLineCheck`, `nowsynchro`, `COShellPWon`, `options_displayer`) | Run and end as they do without a desktop connection. `options_displayer` activates the Hygglo recognizer through COM. |

Original FAT timestamps aren't visible through Glaze's file API, so files that
were never written in Glaze show 1601. Physical hardware has not been rerun
for these changes; the guest integration test exercises the RAM-boot
trampoline and its NAND mount.

## Casio libraries from the NK ROM

Casio's application libraries (`pimdb`, `NFC`, `MailData`, `NotifyData`, `CS`,
`CSO`, `coshellapi`, `GetDisk`, `clblib`, `buzzer`, `NANDAccess`, `commdlg`,
`CAInst`, `cefobj`) exist only inside the NK partition. Metadata recovery keeps
their ROMHDR records (e32/o32 fields and section bytes, about 360 KiB for
`300.bin`) in `rom.cache`. The loader rebuilds each as an ordinary PE at its ROM
base (0x01870000–0x02000000, linked whole at its vbase). It decodes romimage's
compressed data sections with BinaryDecompress's format, which was derived from
`nk.exe`. Their import tables, pre-bound to the ROM's COREDLL, are bound again to
the runtime's adapters. A runtime adapter wins over a ROM library, so one loads
only when an app imports a name the adapters lack. The ROM's COREDLL, COMMCTRL
and drivers are never loaded.

## Registry

Direct launches share one CE registry, `/Library/WinCE/registry`, as CE apps
share theirs. It starts as the device's own `\Backup\System.reg`, Casio's binary
registry backup: HKCR, HKCU and HKLM, without `HKLM\Drivers` (317 keys and 731
values for `300.bin`). Each save bumps `registry.gen`, and running CE apps
reload when it changes. Control Panel's applet list, the RAS phonebook, the
colour scheme, the Tenik browser's home page and the file associations come
from it. Packaged apps keep their private registries.

## System services

- **Processes.** `CreateProcessW`, `ShellExecuteEx` and COShellApi's `CoshExecute`
  start another original executable as its own Glaze app through SpringBoard,
  with its command line for `GetCommandLineW`/WinMain. Documents and URLs open
  with the program the registry associates (`HKCR\<ext>` → `Shell\Open\Command`).
  A running app is brought forward instead. `CoshMainMenuExec`, an app's "main
  menu" command, returns to the home screen (`SB_GO_HOME`).
- **Display, touch, sound, passcode.** SpringBoard lets a foreground CE app make
  the display and touch-calibration requests that Settings makes. The applets
  change Glaze's own settings.
- **Notifications.** `CeSetUserNotification` (and GetDisk's `CGD*UserNotification`
  wrappers) post a SpringBoard notification for the app's context at that local
  time. Handles live in the shared registry.
- **Dial-up.** RAS and the TAPI calls the applets make: see the table.
- **COM.** In-process servers registered under `HKCR\CLSID` load through
  `DllGetClassObject`; there is no OLE32 marshalling.

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
`/NAND/Program Files`, including compressed variants, then the Casio libraries
from NK. Runtime built-in API adapters handle supported CE services first.

Reads of original CE paths fall back to `/NAND`; `\Documents` and
`\My Documents` use `/NAND/My Documents` when no Glaze copy exists. Directory
searches merge the private overlay and original entries, exposing compressed
files under their decompressed names. Compressed data files are decoded into
the private context as needed. Writes use Glaze's existing private drive or
Documents storage, including copying an original file before modifying it.
Source executables, libraries, NK metadata, and original data remain unchanged.
The original registry backup is read, never modified; see "Registry".

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
checksums, and the supplied dump has four. Of NK's code, only the Casio
application libraries above run; invalid metadata fails with a diagnostic
rather than using an ordinal map from another firmware.

Recovery temporarily holds the decompressed NK image (about 6 MiB for the
supplied dump, bounded at 8 MiB). The progress screen remains under SpringBoard's
normal Home/foreground control. Close other large applications if memory is
insufficient. The temporary NK buffer is released before loading the compressed
program. A checksummed metadata cache (version 2, with the library records) in `/Library/WinCE/rom.cache` serves
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
cover every CPK block kind, a ROM library with a compressed data section
(every BinaryDecompress match kind, rebuilt as a PE the loader's parser
accepts), and malformed ranges and truncated streams. The optional local NAND
checks compare Calculator, LC.dll, and Calendar against the independent Python
CCFS decoder and rebuild pimdb, NFC, MailData, CS and CSO. `make -C host
wince-test` with `WINCE_SYSTEM_REG=<a local System.reg>` also imports that
registry. No vendor bytes are required by the default tests or stored in Git.

```sh
python3 tools/wince_audit.py --image /external/300.bin [-v]
python3 tests/run_wince_nand.py --seconds 24
python3 tests/run_wince_nand.py --image /external/300.bin --relaunch --seconds 40
python3 tests/run_wince_nand.py --image /external/300.bin --app Tasks.exe \
  --input tests/ui/t_wince_nand_tasks.txt --seconds 40
```

The runner creates a disposable 16 MiB test NAND under ignored `out/`, with
separate Glaze, original NK, and FAT partitions. The FAT holds the image's own
folders (Program Files, My Documents, Backup). It uses GlazeBoot's real
trampoline handoff and default app selection, then drives Files to the
application (`--app`, Calculator by default), scrolling Program Files with slow
drags. `--input` events count from that launch tap and `--seconds` of
emulation follow it; `--expect` requires serial log text. The first command
uses a generated executable. It verifies entry/exit or app startup, no faults
or unresolved imports (`--allow-unresolved` relaxes that), and hashes the
source partitions before/after. `--relaunch` also checks metadata-cache reuse;
the Calculator input script enters `2 + 3 =` on both launches. It does not
modify the supplied image. Builds go through `./build.sh` when no native cross
compiler is installed. The kernel reads the input script from romfs, so after
one build with any `INPUT_SCRIPT`, `--no-build` repacks GlazeBoot's romfs with
each run's script, and one build serves every app (and parallel runs with
separate `--work` directories). Logs and screenshots are retained under the
work directory. The `t_wince_nand_{tasks,contacts,calendar,notes}.txt` scripts
create a record in each PIM application. Rebuild normally afterwards to remove
scripted input.
