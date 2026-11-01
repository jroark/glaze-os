# Installable packages

Read with the root CLAUDE.md, which covers building, testing and toolchain rules.

`packages/<name>/` holds an installable app (sources, `Info`, `icon.gzi`): it is linked like a built-in app but never put in romfs, and `tools/mkromfs.py --package` packs it as `out/pkg/<name>.gzp` (dynamic builds only). Packages are not on the factory volume; put them on a card image (`tools/mkcfimg.sh out/cf.img 16 16 out/pkg/*.gzp`) and install them from Files. `tools/mkgzi.py` turns a PNG into a GZI icon. Files in `packages/<name>/res/` are packed as `res/<file>`.

Only system packages live here: `hello` is the test package (ktest uses it), `taskmanager` is Task Manager, and `maintenance` is the offline installer's on-device helper (the installer itself is in glaze-installer). The other apps (AIM, Gmail, Chat, Doom, SomaFM, the games) are in the sibling glaze-apps repo, and the console emulators are in glaze-emulators. Both build against this repo's SDK. glaze-web's `make store` runs `tools/mkstore.py` over all three repositories' `out/pkg`, and the offline desktop installer (the glaze-installer repo) bundles them too. AIM's and Gmail's sounds still come from `tools/mksounds.py --aim` / `--gmail`, pointed at those packages' `res/`.

`tests/run.sh` takes `NET=1` to boot with `--ne2000`, and `FS_FROM=<nand image>` to boot another run's file system (apps installed from a card, then a network boot). Relative paths given to it resolve in the caller's directory first, so the sibling repos run their own `tests/ui` scripts through it.
