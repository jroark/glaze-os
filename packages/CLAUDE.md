# Installable packages

Read with the root CLAUDE.md, which covers building, testing and toolchain rules.

`packages/<name>/` holds an installable app (sources, `Info`, `icon.gzi`): it is linked like a built-in app but never put in romfs, and `tools/mkromfs.py --package` packs it as `out/pkg/<name>.gzp` (dynamic builds only). Packages are not on the factory volume; put them on a card image (`tools/mkcfimg.sh out/cf.img 16 16 out/pkg/*.gzp`) and install them from Files. `tools/mkgzi.py` turns a PNG into a GZI icon. Files in `packages/<name>/res/` are packed as `res/<file>`.

Only system packages live here: `hello` is the test package (ktest uses it), `taskmanager` is Task Manager, and `maintenance` is the offline installer's on-device helper (the installer itself is in glaze-installer). The other apps (AIM, Gmail, Chat, Doom, SomaFM, the games) are in the sibling glaze-apps repo, and the console emulators are in glaze-emulators. Both build against this repo's SDK. glaze-web's `make store` runs `tools/mkstore.py` over all three repositories' `out/pkg`, and the offline desktop installer (the glaze-installer repo) bundles them too. AIM's and Gmail's sounds still come from `tools/mksounds.py --aim` / `--gmail`, pointed at those packages' `res/`.

The maintenance companion's protocol 2 adds built-in/firmware inventory and
`uninstall:<package-id>`. It accepts package IDs only and protects built-in apps,
services, and itself. Built-in apps and WL54-CF firmware are read-only ROMFS
content: the desktop installer changes them with a matching-build, source-bound
NAND transaction in repair RAM, retaining the entire file system. The companion
does not write ROMFS or raw NAND. Keep protocol metadata in `Info` synchronized
with the installer packager, which rejects stale companions.

`tests/run.sh` takes `NET=1` to boot with `--ne2000`, and `FS_FROM=<nand image>` to boot another run's file system (apps installed from a card, then a network boot). Relative paths given to it resolve in the caller's directory first, so the sibling repos run their own `tests/ui` scripts through it.
