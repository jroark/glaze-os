# GlazeBoot: booting Glaze from Windows CE

GlazeBoot (`wince/glazeboot`) is a Windows CE 3.0 program that starts Glaze on
a stock BE-300 without flashing anything, as CyaCE did for Linux. It loads the
kernel and the romfs archive from a CompactFlash card into RAM, stops Windows
CE, and jumps into the kernel. Glaze then runs until the device is reset;
Windows CE cold-boots from the untouched NAND afterwards.

In a RAM boot:

- `/` is a RAM disk (default 2 MiB), formatted at boot and lost on reset.
- `/Card` is the CF card GlazeBoot came from.
- `/NAND` is Windows CE's FAT volume in the NAND, read-only unless
  `glaze.cfg` says `nand=rw`.

## Status

| Piece | State |
| --- | --- |
| Kernel RAM-boot mode, RAM disk, `/NAND` | Done; tested in the emulator (`tests/run_ramboot.sh`, ktest passes) |
| Trampoline and hand-over plan | Done; tested natively (`make -C host glazeboot-test`) and in the emulator through the ramboot SPL |
| Loader (`evc/GlazeBoot.cpp`) | Written for eMbedded Visual C++ 3.0; syntax-checked with clang, not yet built by eVC or run on Windows CE |
| Writes to `/NAND` on a real device | Unsafe until Windows CE's spare-area ECC and block replacement are handled (see below) |

## Using it

```sh
./build.sh glazeboot                      # out/glazeboot/: kernel.bin, romfs.img, glaze.cfg, evc/
tools/mkceboot.sh out/ceboot.img          # a CF image with \Glaze holding them
tools/mkceboot.sh out/ceboot.img 16 confirm=0 mode=probe   # extra glaze.cfg lines
```

The loader is built with eMbedded Visual C++ 3.0, not GCC. `make glazeboot`
stages a self-contained project in `out/glazeboot/evc` (CRLF line endings):

| File | From |
| --- | --- |
| `GlazeBoot.vcp`, `GlazeBoot.cpp` | `wince/glazeboot/evc` |
| `tramp_blob.h` | the GCC-built trampoline as a `DWORD` array (`tools/mktrampblob.py`); eVC has no inline MIPS assembly |
| `glazeboot_abi.h` | constants and struct layouts from `glaze/bootinfo.h` and `plan.h` (`wince/glazeboot/mkabi.c`); `GlazeBoot.cpp` checks the struct sizes at compile time |

Copy that folder to the Windows machine, open `GlazeBoot.vcp`, choose
"Win32 (WCE MIPS) Release" and build. The project uses the CPU and platform
IDs of the BEDiag projects in the be300 repo, so the same SDK opens it. Copy
`MIPSRel\GlazeBoot.exe` into `out/glazeboot` before running `mkceboot.sh`.
After any change to `tramp.c`, `plan.h` or `bootinfo.h`, rerun
`make glazeboot` and rebuild in eVC.

The loader looks up `LockPages`, `SetKMode`, `SetProcPermissions` and
`CacheSync` with `GetProcAddress`, because the SDK headers don't declare
them all.

On the device, open `\Storage Card\Glaze\GlazeBoot.exe`. It asks before
handing over (`confirm=1`) and logs each step to `glazeboot.log` beside it.
`mode=probe` checks what the hand-over relies on and returns to Windows CE.
Run it once on new hardware before a real boot. It checks, in order:

1. that the kernel calls can be found;
2. how frame numbers map to physical addresses;
3. reading CP0 through the trampoline blob's virtual address;
4. the same through KSEG0, which is how the hand-over runs;
5. how much memory Windows CE will give.

Each step is logged before it is attempted, so if the program dies, the last
line of `glazeboot.log` names the step that failed.

`glaze.cfg` keys: `kernel`, `romfs` (file names), `ramfs_kb` (128 to 11264),
`nand` (`ro`/`rw`), `confirm` (`0`/`1`), `mode` (`boot`/`probe`). Later lines
win.

## How the hand-over works

1. **Loading.** The loader (`evc/GlazeBoot.cpp`) reads `kernel.bin` (`tools/mkkernelbin.py`: a 4 KiB
   `struct gz_kernel_bin` header, then the flat image) and `romfs.img` into
   memory from `VirtualAlloc`, locks it, and gets each page's frame with
   `LockPages`. It checks how frame numbers map to addresses by writing a page
   and reading it back through KSEG1.
2. **Layout** (`lib/include/glaze/bootinfo.h`):

   | Physical address | Contents |
   | --- | --- |
   | `0xF000` | `struct gz_bootinfo` |
   | `0x10000` | the kernel, at its link address |
   | below the romfs | the RAM disk (`ramfs_kb`, 16 KiB blocks) |
   | top of RAM | the romfs archive, 8 KiB aligned so each page's frame has its archive offset's cache colour |

3. **The plan** (`plan.h`). Every page of content is a move `{src, dst}`.
   Windows CE's pages are scattered, often over the very frames they must end
   up in, so the trampoline places move *i* in step *i*. When the destination
   holds content not yet placed, the two pages are swapped, using a map from
   each frame to the move whose content is in it. No staging memory is needed.
   The plan and the trampoline live in pages whose frames are no destination;
   the control page lists them by frame, because only physical memory is
   usable once Windows CE stops.
4. **Taking over.** `SetKMode(TRUE)` and `SetProcPermissions(~0)`, then
   `CacheSync`, and a call to the trampoline's copy through KSEG0. The
   trampoline (`tramp.c`, position independent; the build rejects absolute
   relocations in it) runs as follows:
   1. Sets Status to 0 and masks the ICU.
   2. Stops the wave DMA and waits for a NAND transfer in flight.
   3. Writes back and invalidates both caches.
   4. Runs the plan.
   5. Flushes the caches again.
   6. Jumps to the kernel with a0 = the boot info.
5. **The kernel.** `start.S` passes a0 to `kmain`, which checks the block. The
   SPL passes 0, a NAND boot.
   - `mm_init` reserves the romfs and the RAM disk.
   - `romfs.c` serves pages straight from RAM, with no cache.
   - `SYS_FLASH_*` act on the RAM disk with NAND semantics.
   - `SYS_FLASH_INFO` gives the file server its size.
   - `SYS_NAND_*` open the whole NAND to the file server. Blocks 0 and 1
     (partition table and boot loader) are never written, and writes need
     `nand=rw`.
6. **`/NAND`** (`servers/fs/nanddisk.c`, FatFs drive 1 beside the card's
   drive 0).
   - The volume is the first FAT partition in the table in NAND page 0. On the
     stock image that is entry 3, at sector 0x1DA0.
   - Writes collect in a copy of one 16 KiB erase block and are written back
     after every request. Changes only to erased pages just program those pages.
     Any other change erases the block and reprograms it.

## Testing

- `make -C host glazeboot-test` runs the plan on random scattered layouts.
- `make -C host fat-test` runs `/NAND` on a RAM NAND model, checking that
  programs only clear bits.
- `tests/run_ramboot.sh [seconds] [make args]` builds `make ramboot` and boots
  it in the emulator.
  - The ramboot SPL (`boot/spl.c`, `-DSPL_RAMBOOT`) reads the kernel and
    romfs into frames scattered over 3–15 MiB. It builds a plan, with boot
    info allowing NAND writes, and runs the real trampoline.
  - The NAND's file-system partition holds a 10 MiB FAT16 volume for `/NAND`.
  - `tests/run_ramboot.sh 90 INIT=bin/ktest` should end with `ktest: ALL PASS`;
    ktest tells a RAM boot by the RAM disk's block count.
- The loader itself still needs a test under the emulator's stock Windows CE
  (`be300 --nand All_nand_300.bin --cf out/ceboot.img`).

## Known gaps

- **Windows CE's NAND format.** `nanddisk.dll` keeps ECC in the spare area and
  replaces bad blocks ("Last BlockReplace"; the restore image is 1004 logical
  blocks of the chip's 1024). `/NAND` maps sectors to pages one to one and
  writes blank spare bytes. That suits the emulator, which synthesizes the spare
  area, but on a device Windows CE may then see ECC errors. Keep `nand=ro` there
  until both are understood.
- **Hardware state.** Glaze's drivers assume what the boot ROM leaves:
  - the LCD mode;
  - the CF socket's I/O decode, which Windows CE's ATA driver sets up;
  - the NAND engine idle in the SPL transfer mode.

  Only real hardware will show which of these Windows CE leaves differently.
- **Memory.** The loader needs room for the romfs (about 4.4 MiB) in Windows
  CE's free program memory. `mode=probe` reports the largest allocation.
