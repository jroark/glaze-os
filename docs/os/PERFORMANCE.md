# Rendering and flash performance

The first responsiveness pass optimizes shared RGB565 drawing and Settings.
Three-slice back buttons now sample one body column instead of repeatedly
blending overlapping images. Rounded rectangles use spans for their interiors;
vertical gradients calculate four dithered colors per row. Settings clips
pressed-state and scrolling updates, keeps complete hit geometry, and avoids
network/preference reads for root-page values outside the clip.

## Measurements

Compared with commit `6b047a0`, using the native BE-300 emulator at `--speed 166`,
MIPS PIC builds at `-O2`, and medians of three batches of 20 draws:

| Drawing operation | Before | After | Speedup |
| --- | ---: | ---: | ---: |
| Solid cell | 1.56 ms | 0.68 ms | 2.3× |
| Selected cell | 6.08 ms | 1.30 ms | 4.7× |
| Navigation gradient | 1.61 ms | 0.49 ms | 3.3× |
| Three-slice back button | 6.08 ms | 0.26 ms | 23.2× |
| Bar button | 2.28 ms | 1.02 ms | 2.2× |
| Translucent rounded gradient | 14.44 ms | 4.33 ms | 3.3× |

The back-button result includes a sampling correctness fix; its old output is
not the visual reference. Other optimized primitives retain the original pixels.
These are guest drawing times, not whole-app speedups or physical-device results.

Settings timings use `tests/ui/t_datetime.txt`, three fresh NAND copies per build,
and temporary instrumentation around drawing and synchronous presentation:

| Pressed control | Draw before → after | Present before → after |
| --- | ---: | ---: |
| Root page: Date & Time | 71.8 → 1.8 ms | 1.84 → 0.44 ms |
| Set Automatically | 24.3 → 2.7 ms | 1.84 → 0.44 ms |
| Time Zone | 46.2 → 2.3 ms | 1.84 → 0.44 ms |
| Time-zone list row | 38.5 → 1.6 ms | 1.84 → 0.51 ms |

Each row press presents 6,600 pixels instead of 72,960 (91% fewer).
Navigation keeps full frames and existing animation durations. Complex Settings
actions still repaint the full view; this pass does not optimize persistent writes.

## Checks and benchmarks

Run `make -C host gfx-test settings-test` for ASan/UBSan checks. Graphics tests
compare 8,000 cases against scalar pixel oracles, including clipping, alpha,
dithering, narrow buttons, odd strides, and untouched buffer guards, and check
that three-slice caps too wide for the image draw nothing. Settings
tests compare partial and full pixels and hit tables across views, navigation,
slider values, and time-zone overscroll; stationary input must not present.
They also write preview images under `out/host/`.

`make -C host gfx-bench` reports current host timings without sanitizers. It uses
`tests/native/gfx_bench.c`, which also supports MIPS guest timing through `gz_time_us()`.
For before/after comparison, compile both renderer snapshots with identical flags,
rename the baseline's exported functions with a `before_` prefix, and define
`GFX_BASELINE` when compiling the benchmark. Host timings must be reported
separately from guest timings.

Scripted emulator checks include `t_about`, `t_datetime`, `t_sounds`,
`t_wallpaper_home`, `t_passcode`, and `t_settings_scroll` under `tests/`.
Use fresh NAND copies so persisted settings do not change subsequent runs.

## Text fitting and flash verification

The next pass removes two unnecessary scans:

- `gfx_text_fit` keeps the longest prefix that leaves room for the ellipsis
  while measuring the label once. It stops as soon as the label exceeds the
  available width. Previously it measured the entire label, then repeatedly
  measured shorter prefixes, making long truncated strings quadratic.
  Glyph fallback, fractional advances, rounding, and rendered pixels stay the
  same. Work now depends on the visible prefix for overflowing labels.
- Flash program/erase verification and erased-page detection only need to
  know whether differences exceed their error budgets. They now skip matching
  bytes and stop after the budget is exceeded. They no longer call the MIPS
  software popcount helper for every byte. Verification still allows two bit
  differences per page; erased-page detection still allows four. BCH checking,
  bad-block handling, and the on-disk format are unchanged.

Compared with `cdb9557`, using the native BE-300 emulator at `--speed 166`,
identical MIPS II PIC `-O2` builds linked into one benchmark executable, and
medians of three batches of 20 calls:

| Operation | Before | After | Speedup |
| --- | ---: | ---: | ---: |
| Short label that fits | 69.00 µs | 68.85 µs | 1.0× |
| 63-character label, truncated to 160 px | 429.95 µs | 166.70 µs | 2.6× |
| 1,023-character URL, truncated to 160 px | 76.38 ms | 0.164 ms | 466× |
| Detect a programmed page | 128.75 µs | 6.25 µs | 20.6× |
| Detect an erased page | 125.55 µs | 37.80 µs | 3.3× |
| Program a clean page, including BCH generation | 486.30 µs | 395.60 µs | 1.2× |
| Erase and verify a clean 32-page block | 4.05 ms | 1.17 ms | 3.4× |

These isolate CPU work: text uses synthetic 5×7 glyphs; flash uses RAM-backed
raw operations with no NAND latency or syscall overhead. They do not measure
whole-file throughput, complete app responsiveness, or physical hardware.
On the host, the full flash fault-injection suite remained about 145 seconds
before and after; its workload is dominated by other operations, and the host
compiler can vectorize the original popcounts.

Run the regression checks with:

```sh
make -C host gfx-text-test gfx-test settings-test ftl-verify-test
make -C host fstest && out/host/fstest 1
```

`gfx-text-test` compares pixels for 62,706 cases using every bundled font,
fallback glyphs, fractional and zero advances, clipping, negative/zero widths,
and widths at the full-label boundary. `ftl-verify-test` checks zero through
eight injected differences, both clustered and spread through the page,
including errors in the final byte and the final page of an erased block.
The full flash suite covers journal wraparound, bit flips, failing blocks,
200 power cuts, and formatting. The complete MIPS build and a fresh-image
SpringBoard boot also pass.

`make -C host gfx-bench ftl-verify-bench` runs the current native CPU benchmarks.
Both benchmark sources support guest timing with `gz_time_us()`. To compare
flash snapshots, rename the baseline's exported `ftl_*` and `dhara_nand_*`
functions with a `before_` prefix and build `tests/native/ftl_verify.c` with
`FTL_BASELINE` and `FTL_BENCH`. For graphics comparisons, also include and rename
the baseline's text functions alongside its core and blit functions before
building `tests/native/gfx_bench.c` with `GFX_BASELINE`. Use identical compiler flags
for both snapshots and report host and guest results separately.

## Glaze vs Windows CE

glaze-os.com's [comparison page](https://glaze-os.com/performance.html) measures
Glaze and the BE-300's stock Windows CE 3.0 the same way in the emulator:

```sh
tests/bench.py glaze --runs 3         # out/bench/glaze.txt
tests/bench_ce.py --runs 3            # out/bench/wince.txt
```

`bench_ce.py` needs the be300-dev SDK (`BE300_DEV`) to build `wince/ce_bench`
(`make ce-bench-exe`), mtools, and the Windows CE factory image (`BE300_NAND`,
default `../be300/ce/restore_images/All_nand_300.bin`), which stays out of the
repo. The probe runs from a CF card as `\CE\R4100\AutoRun.exe` and logs through
`OutputDebugString` to the emulator's stdout; writing to `COM1:` there blocks.

The emulated cold boot is a battery pull. Windows CE resets once through the
watchdog ("System was rebooted by WDT"), then runs its setup wizard: screen
alignment, a "Restart" notice and the Home city page. `bench_ce.py` taps
through it with `--touch-script`; the alignment taps have to be held about a
second. The wizard can't be ended from another process (`TerminateProcess`
fails with error 5 even in kernel mode), so the probe waits for `Welcome.exe`
to exit. Boot time is host seconds from emulator start to "CASIO Original
Shell started", minus the reset's extra pass, so it favours Windows CE.

What each side measures:

- **Boot:** emulator start to SpringBoard ready / the Casio shell (both
  include the boot ROM and SPL). Host seconds, at the default 166 MIPS.
- **Free memory:** Glaze's allocator free pages; Windows CE's
  `GlobalMemoryStatus` available physical memory, which is program memory
  only: CE keeps 6.4 MiB of system image in RAM and gives the object store
  2.3 MiB of the rest.
- **App launch:** Glaze, the tap to the app's first frame; Windows CE,
  `CreateProcess` until the app's window is in front and answers a `WM_NULL`.
  Warm is a second launch while the app is in the background: Glaze resumes
  it, Windows CE starts the program again (Calculator, Clock and Internet
  keep a second copy running, which the probe then closes).

Glaze at 81fc6d1, be300 at 1731cc7, medians of three cold boots each
(November 8, 2026):

| | Glaze | Windows CE |
| --- | ---: | ---: |
| Power on to usable | 1.22 s | 8.68 s |
| Free memory at rest | 13,800 KiB | 5,068 KiB |
| Free with apps in the background (median) | 10,982 KiB | 4,890 KiB |
| Lowest free | 9,296 KiB | 3,496 KiB |
| Program memory | 15,656 KiB | 7,400 KiB |

| App (Glaze / Windows CE) | Glaze cold | CE cold | Glaze warm | CE warm |
| --- | ---: | ---: | ---: | ---: |
| Calculator / calc.exe | 25 ms | 236 ms | 0 ms | 220 ms |
| Notes / notes.exe | 15 ms | 126 ms | 0 ms | 55 ms |
| Calendar / Calendar.exe | 22 ms | 387 ms | 0 ms | 204 ms |
| Clock / clock.exe | 39 ms | 153 ms | 0 ms | 155 ms |
| Settings / ControlPanel.exe | 142 ms | 256 ms | 0 ms | 42 ms |
| Browser / Internet.exe | 24 ms | 2,018 ms | 0 ms | 635 ms |

These are emulator results; the emulator doesn't model cache contents or NAND
timing exactly. Glaze keeps up to four apps in the background, Windows CE was
measured with one.

### On a real BE-300

The same probes on the device (November 8, 2026), medians of three runs each,
over the USB sync cable:

- **Windows CE:** `glaze-usb put out/glazeboot/CeBench.exe '\Nand Disk\CeBench.exe'`,
  `glaze-usb run '\Nand Disk\CeBench.exe'`, then `glaze-usb get '\Nand Disk\cebench.log'`
  once it says `cebench: done`. CE was already running, so there is no wizard
  and no boot time; a PC Connect session was open.
- **Glaze:** a GlazeBoot RAM boot of hwtest's `default` image (`glaze-usb boot`,
  then `reload` for each later run), then `glaze-usb script` with bench.py's
  launch script (`launch_script()`), reading the `sb: launch` lines from
  `glaze-usb logs --once`. The romfs and a 6.5 MiB RAM disk sit in RAM on a
  RAM boot, so its free memory (4,144 KiB at the lock screen) isn't
  comparable with a NAND boot, and romfs pages come from RAM, not NAND.

| App (Glaze / Windows CE) | Glaze cold | CE cold | Glaze warm | CE warm |
| --- | ---: | ---: | ---: | ---: |
| Calculator / calc.exe | 49 ms | 500 ms | 0 ms | 500 ms |
| Notes / notes.exe | 36 ms | 305 ms | 0 ms | 142 ms |
| Calendar / Calendar.exe | 44 ms | 931 ms | 0 ms | 500 ms |
| Clock / clock.exe | 73 ms | 403 ms | 0 ms | 402 ms |
| Settings / ControlPanel.exe | 252 ms | 983 ms | 0 ms | 108 ms |
| Browser / Internet.exe | 61 ms | 3,609 ms | 51 ms | 1,342 ms |

The median of the six cold launches is 55 ms for Glaze and 716 ms for Windows
CE. Both take about twice as long as in the emulator. The Browser's warm
launch restarted it: the RAM boot's memory was short. Windows CE's free
memory matches the emulator's: 5,128 KiB at rest (emulator 5,068), 4,908 KiB
median with an app in the background, 3,560 KiB lowest (with Internet).

## Kernel, flash and boot pass (November 11, 2026)

A pass over the kernel's per-switch work, the flash path and boot, measured in
the emulator with `tests/bench.py` (one run each; guest times repeat to the
millisecond) and ktest's own timing lines. Images built with `WL54CF=0`.

| Measure | Before | After |
| --- | ---: | ---: |
| Emulator start to SpringBoard ready | 1.20 s | 0.84 s |
| SpringBoard's own start (guest) | 282 ms | 170 ms |
| Settings cold launch | 142 ms | 59 ms |
| Photos cold launch | 57 ms | 21 ms |
| Music cold launch | 26 ms | 12 ms |
| Calendar cold launch | 23 ms | 15 ms |
| Notes cold launch | 15 ms | 10 ms |
| Clock cold launch | 40 ms | 31 ms |
| Browser cold launch | 24 ms | 20 ms |
| Calculator cold launch | 25 ms | 24 ms |
| ktest: 1000 IPC round trips | 68 ms | 11 ms |
| ktest: 8000 locked increments (two threads) | 257 ms | 42 ms |
| ktest: 20,000 bytes written to flash | 59 ms | 20 ms |
| ktest: the same read back | 19 ms | 9 ms |
| Kernel text and data, scripted build | 179,224 bytes | 169,676 bytes |

What changed, most effect first:

- **Zombie reaping** ran after every context switch: all 32 process slots,
  and all 128 threads for each one in use. It now runs only when a thread or
  process has exited (`reap_pending`, `kernel/sched.c`). This is most of the
  IPC and lock numbers.
- **The clock.** `time_us()` divided a 64-bit count on every `schedule()` and
  tick, through libc's 64-step loop. It now converts each Count delta with
  one DIVU and carries the remainder (`kernel/timer.c`), and libc's 64-bit
  division takes three DIVUs when the divisor is below 2^16
  (`lib/libc/divdi3.c`), which covers every microsecond-to-millisecond
  conversion after 71 minutes of uptime.
- **BCH on every flash page.** The vendored remainder loop works a bit at a
  time in 64-bit arithmetic; `servers/fs/ftl.c` now does the same polynomial a
  byte at a time through a table. Same bytes on flash, checked against the
  vendored code by `make -C host ftl-bch-test`.
- **One request for a small file** (`FS_READ_FILE`): a preference was open,
  read, close. With the BCH change this is the launch-time gain; Settings
  reads over a dozen preferences as it starts.
- **Boot calibration** of Count against the RTC took three 100 ms windows; it
  takes three consecutive 31 ms ones and uses all 94 ms when they agree.
- **TLB.** Mapping a page flushed all 32 entries; it now probes for the one
  entry that can be stale (`tlb_invalidate`, `kernel/vectors.S`).
- **Zoom animation** frames restore and flush only the rectangle that
  changed, not the whole screen twice; **transitions** pace frames by their
  start time, not a fixed sleep after drawing.
- Smaller: table lookups in the boot reader's Reed-Solomon check
  (`kernel/drivers/nand_ecc.c`), one NAND read per block while indexing a
  compressed romfs, `clz`/`ctz` without loops, `memcmp` by words, and the
  kernel linked with `--gc-sections`.

Not visible in the emulator, which models neither caches, NAND timing nor
the uncached framebuffer: the TLB change's effect on refills, the zoom and
transition changes, the Reed-Solomon check (emulator images carry no
parity) and the romfs index reads. These want a run on the device
(`tools.hwtest`, and `glaze-usb script` with bench.py's launch script as
above).

Looked at and left:

- `-mtune=vr4130` builds and uses no new instructions, but grows romfs by
  32 KiB and its effect can only be measured on the device.
- A reply-and-receive system call and direct hand-off between IPC partners
  would save a trap per request; it changes the ABI the sibling repositories
  build against.
- Reading several flash pages per system call, straight into the caller, and
  keeping the last compressed block for the next romfs fault.
- Rounded-corner coverage (a square root per corner pixel), the keyboard's
  hidden shadow rectangles, cached home-screen layers for page swipes, and
  Notes redrawing its whole view per keystroke.
