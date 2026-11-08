# Windows CE apps

Read with the root CLAUDE.md, which covers building, testing and toolchain rules.

Direct CE NAND launches (`docs/apps/WINCE_NAND.md`) use Files' `sb_wince_launch`
request and SpringBoard contexts under `/Library/WinCE`. `apps/wince/source.c`
keeps original NAND assets read-only and stores mutations in the context;
`assets.c` recovers matching NK exports/CCFS metadata and decodes CPK blocks.
The appended `FS_NAND_READ` RPC remains bounded and read-only, using the existing
kernel CE block-map/ECC path. Preserve packaged CE behavior and never commit
recovered vendor binaries, dictionaries, maps or test NANDs. Run native
`wince-test wince-nand-test`; `tests/run_wince_nand.py` checks actual Files launch
through the RAM-boot trampoline and verifies unchanged source partitions.

Casio's application DLLs run from NK: `assets.c` caches their ROMHDR records
(`rom_libraries[]`) and rebuilds each as a PE at its ROM base (0x01870000-
0x02000000, kept free of relocated DLLs), decoding BinaryDecompress sections;
`main.c` binds them like any library, after the native adapters. Direct
launches share `/Library/WinCE/registry` (seeded from `\Backup\System.reg`,
reloaded on `registry.gen` changes). System bridges live in `system.c`
(passcode, sound, touch calibration, notifications), `gdi.c` (ExtEscape
display), `process.c` (CreateProcess via SpringBoard, `SB_GO_HOME`), `ras.c`
(dial-up) and `fpe.c` (soft-float, libm, setjmp). `tools/wince_audit.py --image`
lists every app's unresolved imports; `tests/run_wince_nand.py --app X --input
tests/ui/t_wince_nand_*.txt` drives one through Files (`--no-build` repacks
romfs with the run's script, so one build serves parallel runs).

