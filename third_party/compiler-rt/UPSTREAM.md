# LLVM compiler-rt builtins (selected sources)

Source: https://github.com/llvm/llvm-project/tree/llvmorg-18.1.8/compiler-rt/lib/builtins
Commit: `3b5b5c1ec4a3095ab096dd780e84d7ab81f3d7ff`
License: Apache-2.0 WITH LLVM-exception (`LICENSE.TXT`).

Only the binary64/binary32 arithmetic, conversion, comparison and 64-bit shift
helpers needed by JerryScript and its math library are vendored, together with
the headers and `.inc` files they include. Upstream files are unmodified.
`glaze_fp_mode.c` is the Glaze implementation of the soft-float environment:
round to nearest, ties to even, with no hardware exception flags. These helpers
are private to the browser, not a new libgcc dependency or a system ABI change.
