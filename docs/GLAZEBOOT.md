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
| Trampoline and hand-over plan | Done; tested natively (`make -C host glazeboot-test`), in the emulator, and on a real BE-300 |
| Loader (`GlazeBoot.c`) | Built with be300-dev; boots Glaze from the emulator's stock Windows CE (`tests/run_ceboot.sh`) and on a real BE-300, to SpringBoard with touch, buttons and apps |
| `/NAND` on a real BE-300 | Done read-only (`nand=ro`, the default): pages mapped through Windows CE's block table and ECC-corrected |
| `/Card` on a real BE-300 | Done: the boot card (8-bit transfers), and cards inserted later |
| Network cards on a real BE-300 | Done when inserted after boot (the boot card is the CF): an AmbiCom AMB8610 NE2000 gets DHCP and loads https; a Z-Com XI-825 scans (open networks only) |
| Writes to `/NAND` on a real device | Unsafe until writes keep Windows CE's spare-area tags and ECC (see below) |

## Using it

For USB-only setup, use [`glaze-usb`](USB.md). It stages GlazeBoot and the
images through stock CE and launches the loader with `--usb-boot`, bypassing
the confirmation window and serial-console setup. `--usb-probe` runs the
loader's diagnostic checks unattended and returns to CE; both modes retain
`glazeboot.log` beside the executable.

```sh
./build.sh glazeboot                      # out/glazeboot: kernel.bin, romfs.img, glaze.cfg, gen/*.h
make glazeboot-exe                        # out/glazeboot/GlazeBoot.exe, natively with ../be300-dev
tools/mkceboot.sh out/ceboot.img          # a CF image with \Glaze holding them
tools/mkceboot.sh --autorun out/ceboot.img 16 confirm=0   # \CE\R4100\AutoRun.exe instead; extra glaze.cfg lines
tests/run_ceboot.sh 90 [mode=probe] [INIT=bin/ktest]      # the autorun card under the emulator's stock Windows CE
```

The loader is C, compiled on the host by the be300-dev toolchain (clang with the
Casio SDK headers and its PE linker; set `BE300_DEV` if it isn't at
`../be300-dev`). `./build.sh` runs in a container without it, hence the two
steps. `make glazeboot` generates the headers the loader needs in
`out/glazeboot/gen`:

| File | From |
| --- | --- |
| `tramp_blob.h` | the GCC-built trampoline as a `DWORD` array (`tools/mktrampblob.py`) |
| `glazeboot_abi.h` | constants and struct layouts from `glaze/bootinfo.h` and `plan.h` (`wince/glazeboot/mkabi.c`); `GlazeBoot.c` checks the struct sizes at compile time |

After any change to `tramp.c`, `plan.h` or `bootinfo.h`, rerun both steps.

The loader looks up `LockPages`, `SetKMode`, `SetProcPermissions`,
`CacheSync` and the memory-division calls with `GetProcAddress`, because the
SDK headers don't declare them all.

With `--autorun`, Windows CE starts the loader when the card is inserted (or
present at boot), like CyaCE's loader. Otherwise open
`\Storage Card\Glaze\GlazeBoot.exe`. With `confirm=1` it shows a window
counting down `countdown` seconds, with Boot and Cancel. It logs each step to
`glazeboot.log` beside it and, since it opens `COM1:` at 115200 8N1, to the
sync cable's serial port, where the kernel's console follows.
`mode=probe` checks what the hand-over relies on and returns to Windows CE:

1. that the kernel calls can be found;
2. how frame numbers map to physical addresses (`LockPages` gives EntryLo PFNs, the address shifted right by 4 on the VR41xx);
3. reading CP0 through the trampoline blob's virtual address;
4. the same through KSEG0, which is how the hand-over runs;
5. how much memory Windows CE will give, whole and in 256 KiB pieces.

Each step is logged before it is attempted, so if the program dies, the last
line of the log names the step that failed.

`glaze.cfg` keys: `kernel`, `romfs` (file names), `ramfs_kb` (128 to 11264),
`nand` (`off`/`ro`/`rw`, default `off`), `confirm` (`0`/`1`), `countdown`
(seconds, 0 waits for a tap), `mode` (`boot`/`probe`). Later lines win.

On a CF or USB RAM boot, Settings shows **Reboot to Windows CE**, with a
confirmation before discarding apps and files held in RAM. NAND boots hide
this action and reject the reboot request in the kernel.
An updated USB build can also return to Windows CE with `glaze-usb reboot --ce`,
which clears retained RAM before resetting. A full reset (battery out) also
works. The reset switch alone warm-boots
Windows CE into its RAM object store, which Glaze has overwritten, and it loops.
Anything kept only in that store is lost; `\Nand Disk` and the card are not
touched.

## How the hand-over works

1. **Loading.** The loader (`GlazeBoot.c`) reads `kernel.bin` (`tools/mkkernelbin.py`: a 4 KiB
   `struct gz_kernel_bin` header, then the flat image) and `romfs.img` into
   memory from `VirtualAlloc` in 256 KiB pieces, locks it, and gets each page's
   frame with `LockPages`. It checks how frame numbers map to addresses by
   writing a page and reading it back through KSEG1. The stock Windows CE gives
   programs about 4.5 MiB, short of the romfs, so in boot mode it first lends
   programs the object store's free pages (`SetSystemMemoryDivision`).
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
- `make -C host nand-ecc-test` checks the NAND ECC decoder on pages encoded
  in Windows CE's layout, with one and two bytes corrupted.
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

## Audio after Windows CE

The wave driver explicitly initializes the codec and companion power/wake
registers using the stock `wavedev.dll` sequence. It preserves the original
halfword/word access widths, enables codec power, waits for each command to
complete and allows 5 ms to settle before starting DMA. Stop disables codec
power. Codec waits time out after 10 ms, leave DMA stopped and return
`GZ_ETIMEDOUT` from the raw start/stop syscall; failed starts preserve queued
PCM for a retry.

The DMA address pairs are current (`0x8B0/0x8B4`) and next (`0x8B8/0x8BC`).
Hardware promotes next to current at a completion. Like wavedev's helper
at `0x01912174`, every refill queues the newly filled buffer through the
next pair, and every playback start restores both pairs. Buffer rotation
is tracked in software (`0x019121A0`), since normal hardware playback can
repeatedly report status `1` for successive RAM buffers. The allocator's
cached zeroing is flushed before using uncached DMA aliases.

The audio GIRQ0 source stays masked while stopped and is masked before
handling, following Windows CE's OAL. If a completion remains asserted after
acknowledgement, the source stays masked while the existing 100 Hz timer
poll continues to service buffers. This keeps an audio interrupt from
starving the scheduler. Bounded serial diagnostics report initialization,
the first four starts and their initial completions.

`make -C host audio-driver-test` exercises the production driver with fake
MMIO: codec power/settling, current/next buffer replenishment, repeated and
simultaneous completion flags, restarts, IRQ masking, timeouts and recovery.
NAND boot, the RAM-boot trampoline and full Windows CE/GlazeBoot emulator
runs pass `INIT=bin/ktest`. Headphone output is also confirmed on a real
BE-300. The internal buzzer uses a separate BLG block at `0x0A000980`
(Casio SDK `buzzer.h`, `hardware.txt:186`); this driver does not implement it.

Settings → Sounds → Volume controls music, radio, video streams and effects.
The audio server reads `snd.volume`, uses the existing squared gain curve
and passes Q15 gain through the appended, audio-server-only `SYS_AUDIO_GAIN`
syscall. Existing syscall numbers and audio IPC stay unchanged. Gain is
applied once after effects mixing at DMA refill, so queued PCM responds
within two buffers after releasing the slider without restarting playback.
Effects are no longer pre-scaled. Deploy the matching kernel and audio-server
romfs together.

`make -C host audio-volume-test` checks preference reload, mute/full gain,
clamping and single-stage effects scaling; the driver test checks live gain
changes on queued PCM and access control. Music volume adjustment is also
confirmed on real hardware with the `audio-volume4` build.

## Boot trace

Until the console works, every stage of a RAM boot paints a block across the
top of the screen (`gz_boot_trace` in `glaze/bootinfo.h`, which lists them):
GlazeBoot first blackens the area and paints block 0; the trampoline paints
1-5, the kernel 6-10 and, on a second row, one block per driver it brings up.
A kernel panic paints the whole third row red. After a hang, the last block
names the stage that stopped. SpringBoard's first frame covers it.

## What a real BE-300 needed

None of these showed in the emulator:

- **Serial.** The sync cable's UART (the companion SIU at 0x0A008680, the
  Stowaway's port) is silent until a Windows CE program opens `COM1:`, which
  powers its transceiver; GlazeBoot opens it, as CyaCE did, and leaves it open.
  The kernel's polled console never waits forever for THRE.
- **The VR4131 CACHE erratum.** PRId 0x0C80 needs Linux's workaround (Config
  bits 22 and 16) before the first Hit cache op, or cached data is corrupted.
  It also halves the D-cache line size, so `start.S` applies it uncached
  between two cache flushes, before any cached store.
- **Interrupt sources Windows CE left on.** `irq_init` disables every GIU line
  but 0, and masks the CF and button sources of the companion's GIRQ0 with bit
  3 of 0x0A000010/14 as Windows CE's handlers do; the CF card's level interrupt
  otherwise stormed.
- **Touch.** The PIU stops after one touch without PADATSTOP; it uses Windows
  CE's settings (`CNT 0x1304`, mask `0xDD`, a 45 ms interval). The resistive
  panel's samples are filtered so that taps don't read as swipes.
- **CompactFlash.** After Windows CE each 16-bit access of the data register
  carries one byte, so the driver sets the card's 8-bit mode (SET FEATURES
  01h) and moves data a byte at a time.
- **Cards inserted later** (`cf.c`'s `pcmcia_poll`, from `SYS_CF_INFO` and
  `SYS_NET_INFO`). As pcmcia.dll does: A034 bits 2 and 3 (Vcc, presumably),
  reset, then the CIS (read only below attribute 0x200, since 0x0A00D200 is
  also the NAND's direct-I/O port). Storage gets CCSR 0xFD, the COR Windows
  CE gave the boot card, CCSR 0x01, with A020 bit 3 set. A network card gets
  its CONFIG register written with a CFTABLE entry decoding I/O 0x300, and
  goes to the NE2000 or XI-825 driver (`net.c`). Retries cover a card pushed
  in slowly. Pulling a card masks its interrupt until the next one is up
  (otherwise its state change held GIRQ0 forever). The emulator's
  `--net-insert-after` swaps the CF card for a network card.
- **NE2000 clones.** The AmbiCom AMB8610 keeps its station address in
  attribute memory (0x1C0) rather than buffer RAM, as Linux pcnet_cs knows,
  and its DP8390 runs remote DMA only with STA set, as Linux ne.c always
  issues it; the DMA width is picked by a RAM read-back test.
- **XI-825.** Station firmware 1.3.5 needs hostap's command deadlines (INIT
  takes far longer than 20 ms) and scans with the host scan RID 0xFCE5; its
  real CIS (logged on insertion) has CONFIG at 0x3E0 and a 16-bit-only I/O
  entry.
- **Shared memory and the D-cache.** Session buffers shared by a server and
  its client had frames of any cache colour, so the two could read different
  lines for one frame: TLS and gzip data arrived corrupted at random. Frames
  and mappings now agree (`kernel/shm.c`).
- **NAND.** Windows CE's nanddisk.dll reads pages by DMA (0x1CC0-0x1CF4) and
  leaves the engine set up for it; Glaze's PIO reads stalled after it. The
  kernel now frames operations as Windows CE's kernel does (A400/A408/A40C,
  CTRL), with the NAND unit's clock at 0x0A001134 first (nanddisk.dll
  gates it between operations, and gated reads return zeros), turns that
  DMA off and resets the engine and chip first. Turning the DMA off
  raises GIRQ0 source 12, which the interrupt handler now masks, as it does
  any source with no driver.
- **Windows CE's NAND format** (`kernel/drivers/flash.c`, `nand_ecc.c`).
  Spare bytes 0-3 of a page are `AA 55 0F FF` (`AA 55 FF ..` erased), 4-7 its
  block's logical number, 8-15 a Reed-Solomon parity over GF(2^10)
  (x^10 + x^3 + 1) of the 512 data and first 8 spare bytes, one bit-reversed
  byte per symbol, correcting two bytes. Before the first NAND read the
  kernel builds nanddisk.dll's block table (NandColdBoot, 0x019A3B24): a
  physical block whose first five pages agree three times on a logical
  number serves it, erased blocks get the numbers from 1004, the rest are
  bad. On the test device 656 of 1024 blocks were moved and one was bad,
  which is what garbled the names when pages were read one to one. The
  layout was worked out from raw pages: the NAND engine's ECC registers
  (A464-A4C0), loaded as the DLL loads them, gave syndromes that matched
  nothing.
- **The watchdog.** Casio's power-management microcontroller (ScCmcu,
  0x0A007800) resets the device 150 s after its last reload; Windows CE writes
  0x96 to 0x7814 every ~9 s, and `kernel/drivers/cmcu.c` does the same.

## Apps in a RAM boot

A RAM boot copies all of romfs into RAM, unlike a NAND boot, which pages it in
on demand. GlazeBoot's `romfs.img` therefore carries only the apps named in
`GB_APPS` (default `appstore browser settings`; `make glazeboot GB_APPS=all`
keeps them all), their icons, the servers, and the test programs. The NAND
image is unchanged. SpringBoard shows a built-in app only when romfs holds its
program. The default saves about 580 KiB, leaving about 8.3 MiB free at
SpringBoard on the emulator. The public full download on glaze-os.com uses
`GB_APPS=all`, restoring Calendar, Photos, Clock, Calculator, Files, Music,
Videos and Notes alongside Browser, App Store and Settings. The full archive
uses more RAM; additional packages can still be installed from the App Store.

## Known gaps

- **Writes to Windows CE's NAND.** Reads go through Windows CE's block table
  and ECC, but writes program blank spare bytes, with no tag or parity, and
  replace blocks in place. That suits the emulator, whose restore images have
  no spare area; on a device Windows CE would lose those blocks. Keep
  `nand=ro` there until writes keep the tags and ECC.
- **Socket registers.** A020, A024 and A034's bits are inferred from
  pcmcia.dll's helpers and traces, not documented; A020 bit 3 made no
  difference to the XI-825's 16-bit I/O.
- **Memory.** The romfs (about 4.7 MiB) fits only with the object store's free
  pages lent to programs; `mode=probe` reports what Windows CE gives.

