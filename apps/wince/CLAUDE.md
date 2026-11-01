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
