# JerryScript

Source: https://github.com/jerryscript-project/jerryscript
Commit: `b7069350c2e52e7dc721dfb75f067147bd79b39b`
License: Apache-2.0 (`LICENSE`); the bundled fdlibm routines also retain their
Sun Microsystems permission notices.

Generated with upstream's `tools/amalgam.py --jerry-core --jerry-math
--output-dir <output>`; configuration lives in `apps/browser/js_config.h`.
Glaze supplies its own port and MIPS soft-float compiler helpers.

Local changes in `jerryscript.c`:

- Make the two `ecma_uint128_shift_*` always-inline definitions explicitly
  `inline`, as required by GCC.
- Replace `rand()`/`RAND_MAX` in Math.random with the browser's private PRNG.
- Poll `glaze_js_budget_expired()` in the native regexp backtracker, which
  does not pass through the VM's backward-jump halt callback. Poll the native
  Promise job loop too, discarding jobs when their entry budget is exhausted.
- Expose a private stack-base setter so each browser entry can measure its
  own C stack budget (load, event and timer paths have different depths).
- Guard the Array iterator's TypedArray path with `JERRY_BUILTIN_TYPEDARRAY`;
  upstream's unguarded code does not build when that feature is disabled.

The amalgamation is source code; bytecode snapshots are not used.
