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

Settings timings use `tests/t_datetime.txt`, three fresh NAND copies per build,
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
`tests/gfx_bench.c`, which also supports MIPS guest timing through `gz_time_us()`.
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
make -C host fstest && host/fstest 1
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
functions with a `before_` prefix and build `tests/ftl_verify.c` with
`FTL_BASELINE` and `FTL_BENCH`. For graphics comparisons, also include and rename
the baseline's text functions alongside its core and blit functions before
building `tests/gfx_bench.c` with `GFX_BASELINE`. Use identical compiler flags
for both snapshots and report host and guest results separately.
