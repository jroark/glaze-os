# UI rendering performance

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
