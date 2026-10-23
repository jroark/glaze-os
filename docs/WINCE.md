# Original BE-300 applications

Glaze has an initial userspace WinCE compatibility runtime for locally supplied
Casio Calculator (`calc.exe`, with its original `LC.dll` engine), PocketNotepad
(`notepad.exe`), Owner (`Owner.exe`) and Owner Information (`OwnerInfo.exe`)
from the BeShell NAND image. An opt-in Clock (`clock.exe`) package supports its
initial Home/Visiting time-display view.
An opt-in RegEdit (`RegEdit.exe`) package supports browsing and editing its
own private registry, including creating keys and string values with the
original dialogs.
PalmTweak (`Tweak.exe`) is also opt-in, with initial support for its General
and Power Off property pages.
They execute their original MIPS instructions and window procedures inside a
normal, unprivileged Glaze process. There is no second OS session or CPU emulator.

Calculator uses its original bitmap controls and arithmetic engine. PocketNotepad
supports basic editing, the Glaze keyboard, and opening/saving documents. Both
use a 240×320 surface, hiding Glaze's status bar. Home returns to Glaze and
suspends the app like a native one. Its top-level windows first receive
`WM_ACTIVATE(WA_INACTIVE)` and, when it is resumed, `WM_ACTIVATE(WA_ACTIVE)`
and a repaint. A suspended app may be ended without warning, so save a
document before using Home.

Owner and OwnerInfo support the original Identification and Notes tabs, text
entry, checkboxes, and saving through the original toolbar's close command.
Each app keeps its own private registry. Startup-display checkboxes store app
settings; integrating those settings with Glaze's startup screen remains future
work. Touch scrolling of the forms is not implemented, so a hardware keyboard
is useful for fields below the on-screen keyboard.

Clock runs its original date arithmetic and draws its original digital time
displays. Without the original OS's city registry, the displays use UTC defaults
and city labels can be empty. City selection, alarms, scheduled launches,
and system clock/time-zone changes are not supported. Attempts to set the RTC
or system time zone return failure. Date/time formats are local to the process;
the locale baseline is English.

RegEdit's root selector, key/value lists, double-tap navigation, Edit menu and
standard modal dialogs run through the original window procedures. Registry
keys and typed values persist in its bundle, including empty keys. This is an
initially empty registry for RegEdit alone; it does not inspect other apps'
settings or the original OS registry. Key class metadata is not preserved;
class queries return an empty string.
Lists support single selection and keyboard navigation; touch scrolling,
owner-drawn lists and wizard/modeless property sheets remain future work. Registry-file
import/export and non-ASCII multibyte conversion have not been validated.

PalmTweak runs its original property-page procedures, including page changes,
validation, Apply, OK and Cancel. The page selector lists the original page
names; action buttons stay in the top bar above the software keyboard. General
can store the animation checkbox, and Power Off can store battery/external
idle timeouts with numeric fields and spinner buttons. These preferences and
the corresponding SystemParametersInfo idle-timeout APIs use PalmTweak's
private registry. They do not change Glaze's animation, lock or power policy.
Cold boots restore those private settings, and Cancel leaves unapplied edits
unsaved. The other pages remain unvalidated. In particular, Font and File Types
require additional font/image-list APIs, and input-method management is not
implemented. Decorative resource icons are not drawn.

This is a selected-app milestone, not general Windows CE compatibility.
PocketNotepad's Search, Font and About dialogs remain unvalidated; process creation,
general vendor DLL compatibility, original installers and multimedia APIs are
not implemented. IPv4 Winsock adapters support the original AIM client's local
server sign-on and bidirectional messaging; see the converted-app notes below.
The editor currently supports typing and deletion; clipboard,
undo, scrolling and touch placement of the caret are not yet supported.
Calling an unimplemented import shows its DLL/function name and a
"Cannot Run WinCE App" screen; Home still works. Unused unsupported imports are
reported on the serial console without preventing supported app functions.
`GetProcAddress` returns NULL with `ERROR_PROC_NOT_FOUND` for COREDLL names the
runtime does not implement, so applications that probe for optional APIs can
fall back.
Physical-device behavior has not been validated.

## Importing your local apps

No vendor executable, filesystem image, DLL or decompression dictionary is
included in the repository. Supply a local BE-300 BeShell NAND containing
the selected executables under `Program Files/` as `.cpk` files (and `LC.dll.cpk`
for Calculator).

```sh
./build.sh
python3 tools/import_wince.py --image ../be300/nand/beshell.bin
tools/mkcfimg.sh out/wince/cf.img 16 16 \
  out/wince/wince.calculator.gzp out/wince/wince.notepad.gzp \
  out/wince/wince.owner.gzp out/wince/wince.ownerinfo.gzp
```

Boot a copy of `out/glaze.bin` with `--cf out/wince/cf.img`. In Files, open
Card and install each package. **CE Calculator**, **CE Notepad**, **CE Owner**
and **CE Owner Info** then appear
on the home screen. The packages require the current Glaze kernel and
SpringBoard, and a dynamic build (`DYNAMIC=1`, the default).

The default import includes those four packages. Select apps with repeatable
`--app` arguments; for example, `--app clock` builds the limited **CE Clock**
package. Packages use version 0.2. The importer derives their home-screen icons
from the original executable's icon resources.
Use `--app regedit` to build **CE RegEdit**. It is opt-in and does not change
the four default packages.
Use `--app tweak` to build **CE PalmTweak** with the limited page support above.
Use `--app internet` with an eXpod-family image to build **CE Internet**, the
Tenik WorldTALK browser, with its `IMGDECMP.DLL` image decoder.

Some apps need original data files. The importer copies the FAT files and
folders listed in `DATA` into `res/`, and `res/files.txt` serves them at
their WinCE paths: Clock's `\Program Files\TIMEZONEINFO.txt`, and
WorldTALK's `\My Documents\WebPage` (the Welcome page) and
`\My Documents\Internet`. A `.cpk` file is decompressed and loses the
suffix, as the CCFS driver presents it.

An app's private registry can also start with values from `REGISTRY`. Without
one, WorldTALK opens `file:///windows/default.htm`, which this ROM does not
contain. The image's saved registry sets
`HKCU\Software\Tenik\WorldTALK\Common\HomePage` to the bundled Welcome
page, and the importer seeds that one value.

The importer reads the source image without changing it. It validates the
FAT16 allocation chains, decodes the Casio CPK blocks, reads the matching
COREDLL and COMMCTRL export names, and emits an `audit.json` with hashes,
sections and imports. Ordinals are qualified by DLL name, so COMMCTRL ordinal
1 is not confused with COREDLL ordinal 1. The runner also accepts the earlier
COREDLL-only map format.
All extracted vendor assets stay under the ignored `out/wince/` directory.
Use `--audit-only` to extract and audit without packaging. `--output` changes
the output directory; `--runner` selects a previously built Glaze runner.

The audited local executables are:

| File | Size | SHA-256 |
| --- | ---: | --- |
| calc.exe | 49,664 | `200d0d64d446b62a03511255e4d96c0a92771ee563e9209f422d3823376b36fc` |
| notepad.exe | 64,512 | `86948ca9a810b8f397f9347dd1ccbe9a604ce4a57a451b10cd6e3ebf52fb2388` |
| LC.dll | 27,648 | `42c19f1207b625d6aed80f0c97c764386736681bb44fcfab82069ac2523c7d59` |
| Owner.exe | 44,544 | `f469b5365cdb0ba2507d400aa5d4953c5e7eb86c8e6e10d709b70aa0d56d0a95` |
| OwnerInfo.exe | 19,968 | `61238cf281fa93e715a14be82c57ec63f7ffda073f91d15cc44a38a8759aa0b5` |
| clock.exe | 69,120 | `e3bf037d19078e33e216ab64ab5bdd3fd9bae3a18203755cae59fe9171b3596f` |
| RegEdit.exe | 30,208 | `5d9aa371e689835c2e212e91e2041f6ddaf4340aa651d2446b57aeefbdc2f8c2` |
| Tweak.exe | 62,976 | `dc27793762228e105b1b42f59611e0da2c0da8fcc465e35ec89be54074c76163` |
| Internet.exe | 980,992 | `abae1194315175af4c5eb494988125194e42c83b60795db91f3082176e4960e3` |
| IMGDECMP.DLL | 53,856 | `3700de24b665d757ca10ef9e1061bcdd41be72629ca530bd5ee708e9a570d50c` |

The importer preserves these bytes. It does not patch entry points, imports,
or application instructions on disk. PE import resolution happens in the
process's private memory, as it does in an executable loader.
The Owner CRT also contains an implicit WinCE `TerminateProcess` trap outside
its import table. Before publishing code, the runner translates that known,
complete MIPS call sequence to an ordinary userspace bridge in the private
loaded image, preserving the return address and delay slot. The matching
COREDLL export confirms the trap address. This translation does not provide
general implicit-call or WinCE kernel support.

## Implementation

`apps/wince` implements a bounded PE32/MIPS loader and the WinCE service subset.
It supports section loading, zero-filled data, named/ordinal imports, resources,
and HIGHLOW/MIPS_JMPADDR relocations. The selected binaries load at their
preferred addresses. Packaged original DLLs can also load, including relocation
when their preferred address is unavailable. Supported OS libraries use service
adapters. PE TLS directories, managed images,
delay imports and other relocation formats are rejected.

Every import has a MIPS tail-call bridge that sets `t9` before entering GCC
PIC code, which derives `gp` from it. Application callbacks preserve the O32
argument convention. The original executable receives its module handle, a null
previous instance, an empty UTF-16 command line, and a show flag of 1.

The kernel's `gz_vm_map`, `gz_vm_publish` and `gz_vm_release` interface provides
private fixed-address mappings below the Glaze interpreter. It rejects overlaps,
kernel addresses and invalid sizes, synchronizes the VR4131 instruction cache,
and can remove write access from published pages. Publication cannot restore
writes to read-only pages. The processor has no NX permission bit. PE parsing,
imports and WinCE policies remain outside the kernel.

SpringBoard's new `SB_STATUS_FULLSCREEN` mode is opt-in; ordinary apps retain
their 240×304 surfaces. The compositor still owns Home, lock/unlock, transitions
and notifications. WinCE child controls share the compatibility surface and are
repainted after their parents.
Common-control support now includes the tab messages and notifications used by
Owner, inherited visibility for painting and touch input, checkbox and radio
button state, read-only/limited edit fields, and the selected OEM command-bar
button layout. Edits honour `ES_MULTILINE` (only multiline edits take Enter
and Tab), `ES_PASSWORD`, `ES_NUMBER`, `ES_UPPERCASE` and `ES_LOWERCASE`. The
on-screen keyboard's Enter arrives as `VK_RETURN`, as on WinCE. A button
clicks only when it is pressed and released under the finger. While a modal
dialog runs, only its own controls receive taps. Dialogs keep separate
`DWL_MSGRESULT`/`DWL_DLGPROC`/`DWL_USER` slots, and other windows have their
own extra-window longs. A handled dialog message returns `DWL_MSGRESULT`, as in
user32. The runtime repaints only when a window or DC drew, not on every
message-loop pass.
Clock uses pen, bitmap, text-measurement and formatted-text adapters, 64-bit
MIPS integer helpers, Gregorian FILETIME conversion and date/time formatting.
RegEdit adds owned heaps, registry enumeration/query/delete operations, string
list/combo controls with selection notifications, and bounded standard dialog
template parsing with modal dispatch and recursive window cleanup. The dialog
layout follows Microsoft's [DLGTEMPLATE documentation](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-dlgtemplate);
registry enumeration follows the [Windows CE API](https://learn.microsoft.com/en-us/previous-versions/ms961350%28v%3Dmsdn.10%29).
PalmTweak adds a modal property-sheet controller, lazy page creation, separate
dialog message results, Apply/Cancel validation, numeric conversion with overflow
checks, up/down controls, group boxes and wrapped static labels. A hidden native
tab control answers application queries while the visible selector fits long
page names on the narrow screen. Page notifications follow Microsoft's
[property-sheet documentation](https://learn.microsoft.com/en-us/windows/win32/controls/property-sheets)
and [PSN_APPLY contract](https://learn.microsoft.com/en-us/windows/win32/controls/psn-apply).
Dialog and property-page templates may come from any loaded module (the
program, `LC.dll` or a bundled DLL).
`RegDeleteKeyW` removes subkeys too, as
[Windows CE's RegDeleteKey](https://learn.microsoft.com/en-us/previous-versions/windows/embedded/ms891451(v=msdn.10))
does. `wsprintfW` (`apps/wince/text.c`, host test `tests/wince_text.c`)
supports flags, `*` widths and precisions, `%S`/`%hs`/`%ls`, `%o` and `%p`.
The ANSI code page is Windows-1252.

WinCE `\Documents`, `\My Documents` and `\Nand Disk\Documents` map to Glaze's
`/Documents`. Other paths map beneath the app's `data/drive` directory. The
root, `\Windows`, `\Program Files` and `\Temp` exist there as they do on a
device; their backing directories are created on first use. Typed
registry values are stored in `data/registry` for each app. Path traversal is
rejected. This does not expose a shared original-OS registry or filesystem.
Registry streams retain the `GZCE` typed-value records and append a `GZCK`
key directory. The runtime also accepts older value-only streams and infers
their parent keys. The store is bounded to 128 keys, 128 values, 64 KiB per value
and 256 KiB per file; open handles are separate and reusable. Updates use a
temporary file and rename.
The current editor renderer uses Glaze's available glyphs; ASCII and Latin-1
are the baseline. The file server's new `fs_set_end` operation implements
WinCE's SetEndOfFile without rewriting an open file behind its handle.

The PE structures follow Microsoft's
[PE/COFF specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format).
CPK decoding was derived from the local CCFS driver's block dispatch and reads
its Huffman word dictionary from the supplied image. `cbeaExplorer/CbeaFile.cpp`
and `dumpcbea` were consulted for the `.cbea` installer archive format.

## Converting `.cbea` installer archives (`tools/cbea2gzp.py`)

`tools/cbea2gzp.py` converts Casio BE-300 `.cbea` application installer archives
into installable Glaze `.gzp` packages:

```sh
./build.sh
python3 tools/cbea2gzp.py --list ../be300/tools/dumpcbea/*.cbea
python3 tools/cbea2gzp.py ../be300/tools/dumpcbea/*.cbea -o out/cbea
```

The converter parses the 256-byte `.cbea` header and entry records, interprets
`Install.inf` (`[App Information]`, `[Make Dirs]`, `[Copy Files]`, and
`[Add Registry]`), automatically decompresses any `.cpk` payloads using the
CCFS dictionary from `--image` (defaulting to `../be300/nand/beshell.bin` when
present), extracts 44×44 `GZI1` icons from PE `.rsrc` sections (falling back to
a default WinCE icon when an executable has no icon resource), and writes:

- `res/program.exe`: the target WinCE MIPS executable.
- `res/registry`: initial `GZCE`/`GZCK` registry state encoded from `[Add Registry]`
  and `HKLM\SOFTWARE\MENU\<Program>`, seeded into `<bundle>/data/registry` on
  first launch.
- `res/files.txt`: manifest mapping sanitized package resource names (`res/<name>`)
  to canonical WinCE paths (`/Program Files/...`, `/Windows/...`) so bundled
  DLLs, bitmaps, sounds, and databases (such as `KjvBible.pdb`) can be opened
  directly from the bundle or copied on write into `<bundle>/data/drive/`.
- Multi-executable `.cbea` suites (such as `casiopims.cbea`) emit one `.gzp`
  package per executable by default; use repeatable `--exe <name>` flags to
  select specific executables, or `--id` / `--name` / `--version` to override
  package metadata. Use `--extract [DIR]` to unpack all resolved files onto the
  host filesystem.

## Validation

### Converted packages and REQ

The test runner accepts `--package` instead of `--image` and derives the app ID
and executable from `Info`. It validates package paths and bounds, installs into
a disposable NAND, and replaces the embedded runner with the current build.
`--net` inserts the NE2000; `--headless` uses SDL's dummy video/audio drivers.
`--no-build` reuses the existing NAND boot code, runner and **already compiled input
script**, so use it only after building the desired script.

REQ's original tree, report list, menus and dialogs support this registry workflow:

```sh
python3 tests/run_wince.py --package out/cbea/cbea.req.gzp \
  --input tests/t_wince_req.txt --seconds 65 --headless --expect-exit 0 \
  --check-key 'HKLM\SOFTWARE\GlazeCompat' \
  --check-string 'HKLM\SOFTWARE\GlazeCompat' Greeting 'Hello from REQ'
```

The script creates the key beneath the package's seeded SOFTWARE branch, saves
`Greeting`, and closes REQ. The assertions read its private registry directly from
NAND. To inspect the value after a cold boot, use the printed run directory:

```sh
python3 tests/run_wince.py --package out/cbea/cbea.req.gzp \
  --input tests/t_wince_req_reopen.txt --seconds 35 --headless \
  --reuse-fs out/cbea.req-EXAMPLE/glaze.bin \
  --check-string 'HKLM\SOFTWARE\GlazeCompat' Greeting 'Hello from REQ'
```

`tests/t_wince_req_modify.txt` reopens the existing string, changes it to
`Updated by REQ`, and closes the app. Run it against the save image with
`--seconds 60 --expect-exit 0` and the corresponding `--check-string` assertion.
The REQ scripts allow additional startup time for the larger compatibility runner.
Reused filesystems must contain the same runner as the current build.
Each run writes `report.json` with loaded modules, unresolved imports, runtime
errors, and the compositor's launch/memory measurements, alongside serial logs
and a screenshot.

The MIPS adapters must compile with `-fno-optimize-sibling-calls`. Their uniform
twelve-word C signature does not mean an original caller reserved twelve argument
words. GCC sibling calls between adapters could overwrite that caller's saved
return address. A synthetic executable exercises this exact small-stack call:

```sh
python3 tests/make_wince_abi.py
python3 tests/run_wince.py --package out/wince-abi.gzp \
  --seconds 14 --headless --expect-exit 37
```

The host suite covers tree notifications/deletion, report-list item data,
recursive/named mutexes, events, thread completion, explicit TLS and per-thread
errors, BSTR strings, and Winsock nonblocking I/O and descriptor sets. Explicit
TLS APIs are distinct from unsupported PE TLS directories.

AIM and the Casio PIM suite remain bring-up targets. AIM can load its original
Xprt, Xpcs, Xptl, CoolUtils, CoolSocket and CoolBucky DLLs, but its complete login
flow and message exchange against the existing `oos` Docker service remain
unfinished. PIM applications still require vendor services and persistent CE
database APIs; local data workflows and Mail account setup are not supported yet.
No shared PIM/native Glaze database is exposed.

### Original NAND apps

```sh
make -C host wince-test
python3 tests/run_wince.py --image ../be300/nand/beshell.bin --app calculator
python3 tests/run_wince.py --image ../be300/nand/beshell.bin \
  --app notepad --check-document
python3 tests/run_wince.py --image ../be300/nand/beshell.bin \
  --app owner --check-owner --expect-exit 0
python3 tests/run_wince.py --image ../be300/nand/beshell.bin \
  --app ownerinfo --check-owner --expect-exit 0
python3 tests/run_wince.py --image ../be300/nand/beshell.bin --app clock
python3 tests/run_wince.py --image ../be300/nand/beshell.bin \
  --app regedit --seconds 28 --check-regedit
python3 tests/run_wince.py --image ../be300/nand/beshell.bin \
  --app tweak --seconds 32 --check-tweak --expect-exit 1
```

The Calculator script presses **2 + 3 =**; its screenshot should show **5**
with the complete original keypad. The Notepad script types **Hello from Glaze**
and saves `compatibility.txt` through the original File > Save command. The
`--check-document` assertion reads the resulting littlefs/Dhara image and checks
the saved bytes. Each run prints its disposable output directory, containing
the NAND, build/serial logs and a BMP screenshot.

The RegEdit script selects HKCU, creates `GlazeCompat`, opens that key, and
creates `Greeting` with `Hello from RegEdit` through the original dialogs.
`--check-regedit` reads the saved registry from littlefs. Cold-boot it with
`--input tests/t_wince_regedit_reopen.txt --reuse-fs <previous>/glaze.bin
--check-regedit`. Native sanitizer tests cover registry round trips, empty keys,
legacy streams, deletion, capacity limits and malformed/truncated registry and
dialog templates.

`tests/t_wince_regedit_modify.txt` reopens the original SZ Editor, changes
the data to `Updated by RegEdit`, saves and closes the app. Run it with the
fresh save image, `--check-regedit --regedit-value 'Updated by RegEdit'
--expect-exit 1`. The original RegEdit WinMain returns 1 on normal exit.

The PalmTweak script disables the General animation checkbox, enables the
battery idle timer, enters five minutes, increments it to six with the spinner,
switches pages, applies, and closes. `--check-tweak` verifies the original
callbacks saved DWORD `Animate=0` and `BattPowerOff=360` in its private registry.
Reuse that test image with `tests/t_wince_tweak_reopen.txt --check-tweak` to
inspect the restored Power Off page, or `tests/t_wince_tweak_cancel.txt
--check-tweak --expect-exit 1` to verify Cancel discards a later checkbox edit.
`tests/t_wince_tweak_escape.txt` checks Escape closes the modal sheet before
any touch establishes focus.
Native sanitizer tests check page validation vetoes, Apply/Cancel notifications,
lazy-page persistence, resource-title ordinals and signed/unsigned numeric
boundaries and overflow.

To check persistence after a cold boot, use the Notepad save run's printed path:

```sh
python3 tests/run_wince.py --image ../be300/nand/beshell.bin --app notepad \
  --input tests/t_wince_notepad_reopen.txt \
  --reuse-fs out/wince-notepad-EXAMPLE/glaze.bin --check-document
```

The screenshot should display the same saved text. `BE300` can select another
emulator binary. The test runner fails if the original executable does not
start, or a runtime error/kernel fault occurs. `tests/t_wince_install.txt`
separately exercises installation from a card through Files.

WorldTALK takes about 40 seconds of emulator time to lay out its home page:

```sh
python3 tests/run_wince.py --image ../be300/nand/expod.bin --app internet \
  --seconds 45 --headless
```

The screenshot should show the **Welcome to the Internet World** page and its
images under a two-row bar holding the page's `file:` URL. The bar's own
buttons are not drawn yet.

Owner tests enter **Alex from Glaze**, switch to Notes, enter **Original app
compatibility**, select the Notes checkbox, and save with the toolbar button. `--check-owner`
checks both binary registry records and the checkbox; `--expect-exit 0` checks
that the original CRT terminates normally. For a cold-boot visual check, reuse
that run's NAND with `--input tests/t_wince_owner_reopen.txt` or
`tests/t_wince_owner_notes.txt`, keeping `--app` the same. The test runner
rejects a reused filesystem containing a different runner binary, so an older
runtime cannot accidentally stand in for the current implementation.

Owner and OwnerInfo save/exit tests passed, and both Owner tabs were inspected
after cold boot with the saved values and Notes checkbox restored. The Clock
display smoke test, Calculator's 2 + 3 = 5 test, and Notepad's document save
check also passed with this runner. The added host tests cover per-DLL ordinal
resolution, direct-call bounds/delay slots, Gregorian calendar boundaries,
FILETIME conversion and formatting buffer limits.

Host PE tests run under ASan/UBSan and exercise malformed headers, section
bounds, imports and relocations. Python tests use synthetic FAT/CPK/package
data only. The kernel self-test checks mapping ownership, overlap rejection,
code publication, read-only enforcement, release/reuse, full-length package
ids, and file truncation. The complete kernel suite passed with no network
card; FAT16 and FAT32 tests also passed with the new truncation cases.
Calculator's original engine produced 5 for 2 + 3; Notepad's saved
document was verified from NAND and reopened after a cold boot. The observed
first launches left about 12 MiB free on the 16 MiB emulator.

### Converted application bring-up

The additional runtime services cover the supplied AIM and Casio PIM binaries:

- Original threads, critical sections, events, mutexes and thread-local storage;
  embedded CE thread/error/TLS traps are translated in the private loaded image.
- Named anonymous mappings with independent handle and view lifetimes. File-backed
  mappings remain unsupported.
- Per-app file timestamps in `data/filetimes`, including enumeration, rename and
  copy; timestamps are buffered until close. `data/drive/Temp` is created on first use.
- A persistent catalog for empty CE databases in `data/databases`. Creating,
  opening, closing and querying an empty alarm catalog work. Record writing,
  database change notifications and Casio alarm export remain unsupported.
- UTC/local FILETIME conversion using Glaze's configured time zone, numeric wide
  scanning, and additional CRT string operations. `_wtol` matches the supplied
  COREDLL's hexadecimal extension, used by Tasks' hidden record-ID column.
- Editable combo children, tab item data, owner-data report lists, dialog
  subclassing, command bars in modal loops, and larger bounded dialog templates.
  Report lists preserve item images, send changing/changed and activation
  notifications, and honor selection vetoes. OEM menu state uses CE menu flags.
- Writable DIB sections for indexed, RGB555/RGB565, 24-bit and 32-bit pixels,
  memory-DC copies and source-independent pattern blits.

Tasks saves its original record in `data/drive/ToDo/00000001`; create, append/save,
cold reopen/modify, and cold-launch deletion have been verified against the actual NAND.
Contacts saves
`data/drive/Address/00000001`; entering “Compatibility Contact” produces the
original app's sorted display name “Contact, Compatibility”. Notes saves plain
text in `data/drive/Text/Text1.txt`. These are separate private stores, not Glaze's
native PIM databases. Form scrolling and the other PIM edit/delete workflows
remain under validation. Calendar saves its original appointment record in
`data/drive/Appointment/00000001`, with date values and editable fields. Scheduled
alarms remain unsupported; the no-reminder save path is validated. Mail remains blocked by its
original NFC C++ framework dependency, which is absent from the CBEA package.
A ROM export-name snapshot is not a runnable replacement for that DLL.
BibleReader (`OTKJ`) currently stops at the unsupported COREDLL `_setjmp` call;
its startup baseline does not establish document-reading support.

Private-file assertions support both encodings used by the original apps:

```sh
python3 tests/run_wince.py --package out/cbea/cbea.tasks.gzp \
  --input tests/t_wince_tasks.txt --seconds 120 --headless \
  --check-file-utf16 drive/ToDo/00000001 'Compatibility task'
python3 tests/run_wince.py --package out/cbea/cbea.contacts.gzp \
  --input tests/t_wince_contacts.txt --seconds 140 --headless \
  --check-file-utf16 drive/Address/00000001 'Contact, Compatibility'
python3 tests/run_wince.py --package out/cbea/cbea.notes.gzp \
  --input tests/t_wince_notes.txt --seconds 140 --headless \
  --check-file-text drive/Text/Text1.txt 'Compatibility note'
```

Use `--reuse-fs <previous-run>/glaze.bin` with the corresponding reopen script
for a cold-launch check. The runner must match the current build. Script times
are guest milliseconds; allow more host time when several emulators run together.
`--check-no-file` verifies deletion by listing an existing private parent
directory. The Tasks modify/delete scripts exercise the original UI; report-list
selection queries include `LVM_GETITEMSTATE` and masked state updates. Double
clicks send both `NM_DBLCLK` and `LVN_ITEMACTIVATE`, with the complete item payload.
The modify script presses Right before typing to collapse the original app's
selected subject, then appends ` updated` and saves the same record.

The original AIM client requires a complete first registration launch, followed
by another launch using its saved registry and `Xpcs_Registry.dat`. Its server is
configured through private `HKCU\Software\America Online\AimCE\Config` values
`Host` (REG_SZ) and `Port` (REG_DWORD). Both the login address and the OSCAR
server's advertised BOS address must be reachable from the emulator; guest
`127.0.0.1` addresses the guest. `tests/t_wince_aim_message.txt` uses local test
accounts “Glaze User” and “GlazeBob”. The original client sent “Compatibility
ping” to a local `aim-test` echo account and displayed its “echo from aim-test”
reply in the original conversation window. Notification sounds use Glaze's default
sound when the requested CE WAV is absent; memory/resource WAV playback is not
implemented.

`make -C host wince-test` exercises these services under ASan/UBSan, including
concurrent `select` and `send` over the serialized native network transport,
nonblocking connect errors, mapping lifetime, TLS isolation, date-picker ranges,
tab item data, report-list selection vetoes and activation data, CE hexadecimal
record IDs, owner-data callbacks, and cold file-time reloads. The synthetic
ABI package additionally checks owned-popup lookup, separate window extra data,
editable combo children and writable/overlapping bitmap operations on MIPS.
