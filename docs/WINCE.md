# Original BE-300 applications

Glaze has an initial userspace WinCE compatibility runtime for two locally
supplied programs: Casio Calculator (`calc.exe`, with its original `LC.dll`
engine) and PocketNotepad (`notepad.exe`) from the BeShell NAND image.
They execute their original MIPS instructions and window procedures inside a
normal, unprivileged Glaze process. There is no second OS session or CPU emulator.

Calculator uses its original bitmap controls and arithmetic engine. PocketNotepad
supports basic editing, the Glaze keyboard, and opening/saving documents. Both
use a 240×320 surface, hiding Glaze's status bar. Home returns to Glaze and
terminates the app, as it does for native apps: save a document before using Home.

This is a selected-app milestone, not general Windows CE compatibility.
PocketNotepad's Search, Font and About dialog templates, process creation,
arbitrary vendor DLLs, original installers, networking and multimedia APIs are
not implemented. The editor currently supports typing and deletion; clipboard,
undo, scrolling and touch placement of the caret are not yet supported.
Calling an unimplemented import shows its DLL/function name and a
"Cannot Run WinCE App" screen; Home still works. Unused unsupported imports are
reported on the serial console without preventing supported app functions.
Physical-device behavior has not been validated.

## Importing your local apps

No vendor executable, filesystem image, DLL or decompression dictionary is
included in the repository. Supply a local BE-300 BeShell NAND containing
`Program Files/calc.exe.cpk`, `notepad.exe.cpk` and `LC.dll.cpk`.

```sh
./build.sh
python3 tools/import_wince.py --image ../be300/nand/beshell.bin
tools/mkcfimg.sh out/wince/cf.img 16 16 \
  out/wince/wince.calculator.gzp out/wince/wince.notepad.gzp
```

Boot a copy of `out/glaze.bin` with `--cf out/wince/cf.img`. In Files, open
Card and install each package. **CE Calculator** and **CE Notepad** then appear
on the home screen. The packages require the current Glaze kernel and
SpringBoard, and a dynamic build (`DYNAMIC=1`, the default).

The importer reads the source image without changing it. It validates the
FAT16 allocation chains, decodes the Casio CPK blocks, reads the matching
COREDLL export names, and emits an `audit.json` with hashes, sections and imports.
All extracted vendor assets stay under the ignored `out/wince/` directory.
Use `--audit-only` to extract and audit without packaging. `--output` changes
the output directory; `--runner` selects a previously built Glaze runner.

The audited local executables are:

| File | Size | SHA-256 |
| --- | ---: | --- |
| calc.exe | 49,664 | `200d0d64d446b62a03511255e4d96c0a92771ee563e9209f422d3823376b36fc` |
| notepad.exe | 64,512 | `86948ca9a810b8f397f9347dd1ccbe9a604ce4a57a451b10cd6e3ebf52fb2388` |
| LC.dll | 27,648 | `42c19f1207b625d6aed80f0c97c764386736681bb44fcfab82069ac2523c7d59` |

The importer preserves these bytes. It does not patch entry points, imports,
or application instructions on disk. PE import resolution happens in the
process's private memory, as it does in an executable loader.

## Implementation

`apps/wince` implements a bounded PE32/MIPS loader and the WinCE service subset.
It supports section loading, zero-filled data, named/ordinal imports, resources,
and HIGHLOW/MIPS_JMPADDR relocations. The selected binaries load at their
preferred addresses. Only the packaged `LC.dll` is loaded as an original DLL;
the remaining supported libraries are service adapters. TLS, managed images,
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

WinCE `\Documents`, `\My Documents` and `\Nand Disk\Documents` map to Glaze's
`/Documents`. Other paths map beneath the app's `data/drive` directory. Typed
registry values are stored in `data/registry` for each app. Path traversal is
rejected. This does not expose a shared original-OS registry or filesystem.
The current editor renderer uses Glaze's available glyphs; ASCII and Latin-1
are the baseline. The file server's new `fs_set_end` operation implements
WinCE's SetEndOfFile without rewriting an open file behind its handle.

The PE structures follow Microsoft's
[PE/COFF specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format).
CPK decoding was derived from the local CCFS driver's block dispatch and reads
its Huffman word dictionary from the supplied image. `cbeaExplorer/CbeaFile.cpp`
was consulted for the distinct installer archive format; `.cbea` installation
is outside this milestone.

## Validation

```sh
make -C host wince-test
python3 tests/run_wince.py --image ../be300/nand/beshell.bin --app calculator
python3 tests/run_wince.py --image ../be300/nand/beshell.bin \
  --app notepad --check-document
```

The Calculator script presses **2 + 3 =**; its screenshot should show **5**
with the complete original keypad. The Notepad script types **Hello from Glaze**
and saves `compatibility.txt` through the original File > Save command. The
`--check-document` assertion reads the resulting littlefs/Dhara image and checks
the saved bytes. Each run prints its disposable output directory, containing
the NAND, build/serial logs and a BMP screenshot.

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

Host PE tests run under ASan/UBSan and exercise malformed headers, section
bounds, imports and relocations. Python tests use synthetic FAT/CPK/package
data only. The kernel self-test checks mapping ownership, overlap rejection,
code publication, read-only enforcement, release/reuse, full-length package
ids, and file truncation. The complete kernel suite passed with no network
card; FAT16 and FAT32 tests also passed with the new truncation cases.
Calculator's original engine produced 5 for 2 + 3; Notepad's saved
document was verified from NAND and reopened after a cold boot. The observed
first launches left about 12 MiB free on the 16 MiB emulator.
