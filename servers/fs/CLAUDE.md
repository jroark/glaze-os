# File server

Read with the root CLAUDE.md, which covers building, testing and toolchain rules.

**File system** (`servers/fs`, `glaze/fs.h`, client in `lib/libc/fs.c`):
- littlefs runs on the Dhara NAND translation layer, both vendored unmodified in `third_party/` and built into `libfsstack.a`. Don't edit them; put changes in `servers/fs/ftl.c`.
- ECC is in-band: the spare area is synthesized by the emulator and absent from image files. Each page gets a 4-bit BCH code stored in the unused tail (bytes 480–511) of the checkpoint page Dhara writes after every 3 data pages.
- The rest of `ftl.c`'s design is documented in its header comment and depends on Dhara's journal layout (`log2_ppc == 2` for 512-byte pages): the ECC cache, the bad-block table in the region's first two blocks, read-back verification, and torn-checkpoint handling. Re-run `out/host/fstest` after any change to `ftl.c`.
- littlefs's block count is fixed from the geometry, not from `dhara_map_capacity`, which changes between mounts.
- Clients get a session with an 8 KiB shared buffer; paths and data travel in it.
- Packages (`glaze/pkg.h`, `lib/libc/pkg.c`): a `.gzp` is the romfs container with the magic `GZPK` (entries `Info`, the executable, `res/...`). `pkg_install` unpacks into `<id>.app.tmp` and renames it, replacing an install with the same id on either volume; `pkg_list` removes interrupted `.tmp` installs. An app's bundle path (at most 47 characters) is its identity for badges and notes; its data lives in `<bundle>/data`. `fs_remove_all` deletes a tree.
- Layout: `/Documents`, `/Pictures` (Photos' Camera Roll), `/Applications` (installed apps), `/Music`, `/Movies`, `/Library/Preferences` (settings, one file per key, via `glaze/prefs.h`), `/Library/Notes`, `/Library/Caches` (rebuildable). A blank or erased volume gets only the folders in `servers/fs/folders.h`.
- A CompactFlash card with a FAT volume appears as `/Card` (`servers/fs/card.c`, FatFs in `third_party/fatfs`, configured by its `ffconf.h`): a synthetic first entry of `/`, mounted from a partition table's first FAT partition or a table-less volume. `/Card` itself can't be removed, renamed or created on the flash, and renames between volumes fail. `fs_statvfs(path)` reports the volume holding a path. The server calls `card_check()` before each request; it compares the kernel's card generation and remounts, and handles from a previous card fail with `GZ_EIO`. Long names are UTF-8; hidden and system entries are not listed.
- Reads are slow, about 1.5 s for a 150 KB picture in the emulator, so apps should cache derived data (Photos keeps thumbnails in `/Library/Caches/Photos`) and load in the background.
