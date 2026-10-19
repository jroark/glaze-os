# Native shared libraries

The default build links apps and SpringBoard as ELF32 little-endian MIPS O32
PIE programs. Their `PT_INTERP` names `/lib/ld-glaze.so.1`. The kernel maps the
program and this independent, statically linked interpreter; the interpreter
loads `DT_NEEDED` dependencies, relocates them eagerly and runs constructors
before entering the program. `fs` and `net` remain static bootstrap services.

`libc.so.1` owns the allocator, threads, errno, resource mapping, filesystem and
network clients, and BSD socket wrappers. `libgfx.so.1` owns graphics and font
resources; `libui.so.1` owns widgets and the app client. Each has an explicit
export map. lwIP, Dhara/littlefs and Browser's engine still have one consumer
each, so moving them into separate libraries would add loader and file overhead
without removing copies. Internal helpers stay private to their library.

## Loading modules

```c
#include <dlfcn.h>
#include <stdio.h>

void *h = dlopen("libexample.so.1", RTLD_NOW | RTLD_LOCAL);
if (!h) {
    printf("%s\n", dlerror());
    return 1;
}
int (*run)(int) = (int (*)(int))dlsym(h, "example_run");
char *error = dlerror();
if (error) printf("%s\n", error);
else printf("%d\n", run(42));
dlclose(h);
```

Bare names search romfs `lib/`, then `/Library/Libraries/` on the writable
filesystem. Explicit paths such as `/Documents/plugin.so` use the filesystem.
An existing pathname or SONAME reuses the loaded object. SONAMEs identify one
version within a process; use a new SONAME for an incompatible version.
Filesystem images are immutable snapshots while loaded, including when another
process rewrites the file. A later load after unloading reads the new contents.
Startup dependencies must be in romfs: the file server cannot participate in
bootstrapping the interpreter and libc.

`RTLD_NOW`, `RTLD_LOCAL`, `RTLD_GLOBAL`, `RTLD_DEFAULT` and `dlopen(NULL, ...)`
are supported. Handle lookup searches the object then its dependency graph in
breadth-first `DT_NEEDED` order. Global lookup follows the startup scope then
global load order; strong definitions take precedence over weak definitions.
Errors are per-thread and `dlerror()` consumes the pending error. Clear an old
error before a lookup when a null symbol value is meaningful.

Loading is serialized with a kernel semaphore; constructors and destructors can
reenter the loader. Another opener waits for an in-progress constructor.
Constructors run dependencies first, destructors in reverse initialization
order. References, dependencies and symbol bindings keep providers alive;
unreachable dependency cycles are collected. The final `dlclose` runs the
destructors and releases the mappings and private pages. Module metadata pages
are reused up to the process's module high-water mark.

The caller must stop module-owned threads and callbacks and discard pointers
before releasing the last handle. `dlsym` does not create a new reference.
Do not race a final close with use of that handle. Normal `exit` runs remaining
finalizers; a kill or fault releases memory without running user finalizers.

## Building

Use the existing `PIC_CFLAGS` and `DYN_LDFLAGS` in the Makefile when adding a
library target. For example, inside the cross-toolchain container:

```sh
mipsel-linux-gnu-gcc -march=mips2 -mabi=32 -msoft-float -G0 -mabicalls -fPIC \
  -ffreestanding -fno-builtin -nostdlib -fno-stack-protector \
  -Ilib/include -Ilib/libc/include -O2 -c example.c -o example.o
mipsel-linux-gnu-ld -m elf32ltsmip --hash-style=sysv -z now -z relro -z text \
  -z max-page-size=4096 -z separate-code -shared -soname libexample.so.1 \
  -z defs -o libexample.so.1 example.o -Lout/shared -l:libc.so.1
```

Add a version script to constrain exports. Do not link a private copy of libc
or the static client archives into a plugin. For ROM packaging, add its link
artifact to `ROMFS_LIBS` before the packaging rules. Shipped libraries are
stripped separately; `out/shared` retains debugging symbols. Filesystem plugins
can be copied into `/Library/Libraries` using the ordinary filesystem API.

For a fully static fallback, use a separate output directory:

```sh
./build.sh -s OUT=out/static DYNAMIC=0
```

Use separate `OUT` directories when changing `DYNAMIC`; object/link command
changes are not dependencies in make. Static processes retain their startup
ABI, but their `dlopen` reports that a dynamic process is required.

This is a Glaze-native C ABI: MIPS I/II/III O32, soft float, SysV hashes,
implicit MIPS GOT relocations, `R_MIPS_REL32` and `R_MIPS_32`. Lazy binding,
compiler TLS, symbol versions, C++ runtime/unwinding, Linux binaries, MIPS16,
microMIPS, RELA and text relocations are unsupported. The limits are 32 live
objects per process (including the executable), 32 program headers and 2 MiB
per ELF file. Unsupported inputs fail before executing their constructors.

## Mappings and persistence

The kernel maps immutable romfs code pages directly into multiple address
spaces. Identical filesystem snapshots share cached frames after hash and byte
comparison. GOT, writable data and BSS have private pages per process; there is
no copy-on-write. A shared frame survives until its last mapping is removed.
The DSO arena is `0x20000000`–`0x40000000`; holes are reused after unload.
Region records grow in page-sized blocks rather than imposing the previous
fixed 32-region limit. Failed mappings roll back partial pages and tables.

ELF validation checks ranges, overflow, alignment, ABI and overlapping load
pages. Dynamic images reject writable/executable load segments and read-only
relocation targets. RELRO pages are sealed after relocation. The VR4131 has no
per-page execute-disable permission; read-only text is enforced by the TLB's
write permission. New owned pages match their kernel alias's cache colour,
and code publication writes back D-cache and invalidates I-cache. The emulator
does not model these caches, so physical-hardware validation remains necessary.

The NAND filesystem stays at `0x500000` with its existing littlefs/Dhara format.
This change adds `/Library/Libraries` to the standard folders and does not
migrate or reset existing user files. A newly built `glaze.bin` still contains
a factory filesystem: boot a copy, and preserve the existing filesystem region
when updating an image that contains user data.

## Validation and measurements

```sh
make -C host elf-test stowaway-test
./build.sh -s LD_TESTS=1 INIT=bin/ldtest
# Boot a copy with be300 --nand ... --rtc-host-time; require ldtest: ALL PASS.
./build.sh -s INIT=bin/ktest
# Run with and without --ne2000; require ktest: ALL PASS.
./build.sh -s OUT=out/static DYNAMIC=0 INIT=bin/ktest
```

`ldtest` covers dependency order, GOT/function/data pointers, weak symbols,
exports and visibility, duplicate opens, finalizers, cycles, reentrancy,
concurrent opens/closes and thread-local errors. Separate processes verify
physical code-frame identity, private writable state, text-write faults, and
exit/kill cleanup. It writes a filesystem plugin, replaces it while loaded,
and exercises malformed headers/dynamic tables and partial mapping OOM.
Repeated success/failure/unload cycles check exact physical-page recovery.
Fixtures and test apps are only packaged with `LD_TESTS=1`.

For size and RAM comparisons:

```sh
./build.sh -s
./build.sh -s OUT=out/static DYNAMIC=0
python3 tools/library_sizes.py out/static/romfs.img out/romfs.img
./build.sh -s MEM_TESTS=1 INIT=bin/ldmem
./build.sh -s OUT=out/static DYNAMIC=0 MEM_TESTS=1 INIT=bin/ldmem
```

The `ldmem` workload draws the same graphics/text/widgets in three processes
and measures free physical pages while all are live. Results on the BE-300
emulator, 2026-10-19, with no Ethernet card:

| Metric | Static | Dynamic |
| --- | ---: | ---: |
| Normal romfs, 13 programs | 3084 KiB | 2412 KiB |
| Packaged program + library bytes | 1,571,248 | 881,248 |
| ELF PT_LOAD file bytes | 715,780 | 639,990 |
| Archive padding for ELF files | 42,576 | 44,448 |
| Free RAM pages, three UI workloads | 2784 | 2933 |

The archive saves 672 KiB (21.8%). The RAM workload saves 149 pages (596 KiB)
overall, including the smaller kernel-resident romfs and the cost of private
GOT/data, interpreter metadata and page tables. It is not a measurement of code
sharing alone. Each dynamic UI workload reports 27 shared image pages and 46
private image pages; the regression test separately verifies identical code
frame numbers across processes.

Three headless cold boots to `springboard: ready` took 1297/1303/1297 ms static
and 1309/1310/1312 ms dynamic on the same host (10 ms polling, not a hardware
benchmark).

Kernel tests passed in static and dynamic builds; dynamic networking passed
DHCP, DNS, ping, TCP, UDP and BSD-socket tests. UI smoke checks covered Notes
save/edit, Files folder naming, and Browser addresses and text/password/multiline
forms through a Stowaway keyboard. The native Browser renderer and ELF/input
tests ran with ASan/UBSan.
