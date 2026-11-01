# GlazeBoot

Read with the root CLAUDE.md, which covers building, testing and toolchain rules.

**RAM boot** (`wince/glazeboot`, `glaze/bootinfo.h`, `docs/dev/GLAZEBOOT.md`):
- GlazeBoot is a Windows CE program, not a Glaze one, and lives outside `apps/`. The loader (`wince/glazeboot/GlazeBoot.c`) is C built with the be300-dev toolchain (clang and the Casio SDK; `make glazeboot-exe`, natively, since `./build.sh` runs in a container). `make glazeboot` supplies its generated headers in `out/glazeboot/gen`:
  - `tramp_blob.h`: the trampoline (`tramp.c`, position independent) as bytes;
  - `glazeboot_abi.h`: shared layouts from `wince/glazeboot/mkabi.c`.
- Its `romfs.img` is GlazeBoot's own: a RAM boot holds all of romfs in RAM, so it carries only the apps in `GB_APPS` (default `appstore browser files settings wince`; `GB_APPS=all` for every app) and their icons, plus servers and test programs. SpringBoard shows only the built-in apps whose program romfs holds (`sb_has_program`).
- It loads `kernel.bin` (`tools/mkkernelbin.py`) and `romfs.img` from a CF card (`tools/mkceboot.sh --autorun` makes one that Windows CE starts on insertion). The trampoline runs the page plan in `plan.h`. Until the console works, each stage paints the boot trace (`gz_boot_trace`, `glaze/bootinfo.h`) across the top of the screen.
- The kernel gets a0 = `struct gz_bootinfo` (the SPL passes 0). `ram_boot` is then set:
  - romfs pages come straight from reserved RAM, with no cache;
  - `SYS_FLASH_*` are a RAM disk (`SYS_FLASH_INFO` gives its size);
  - `SYS_NAND_*` open the whole NAND to the file server, never writing blocks 0-1.
- The file server mounts Windows CE's FAT partition as `/NAND` (`servers/fs/nanddisk.c`, FatFs drive 1; the card is drive 0, and card paths go to FatFs unprefixed). The kernel maps NAND pages through Windows CE's block table (built from the spare tags as nanddisk.dll does) and corrects them with its spare-area Reed-Solomon ECC (`kernel/drivers/nand_ecc.c`, `make -C host nand-ecc-test`); erased-parity pages, as in the emulator, pass unchecked. Writes don't keep the tags or ECC, so `glaze.cfg` ships `nand=ro` and writes are opt-in (`nand=rw`); `nand=off` is `GZ_BOOT_NAND_OFF`.
- `make ramboot` (`boot/spl.c` with `-DSPL_RAMBOOT`) hands over the same way from the SPL, so the emulator can test it.
