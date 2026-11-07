# Glaze repositories

Glaze is split across five repositories. glaze-os is the core; the others build
against its exported SDK or its outputs, and find it as a sibling checkout.

| Repository | Owns | Builds from glaze-os |
| --- | --- | --- |
| [glaze-os](https://github.com/jroark/glaze-os) | Kernel, SPL, servers, built-in apps, libraries, the SDK, glaze-usb, GlazeBoot, the system packages (`hello`, `taskmanager`, `maintenance`), `tools/mkstore.py` (the App Store catalog format) | n/a |
| [glaze-apps](https://github.com/jroark/glaze-apps) | AIM, Gmail, Chat, Doom, SomaFM, Maps, Minesweeper, Solitaire, Tetris, Fifteen, Sketch | the SDK (`out/sdk/glaze-sdk`); glaze-os sources for native tests |
| [glaze-emulators](https://github.com/jroark/glaze-emulators) | NES, Game Boy, Master System, Genesis, Game Gear, TurboGrafx-16 and their cores | the SDK; `lib/include` for native tests |
| [glaze-web](https://github.com/jroark/glaze-web) | glaze-os.com, App Store listings (`store/apps.json`), the site and App Store stacks and publishers | `tools/mkstore.py`, `tools/package_glazeboot.py`, `firmware/`; every repo's `out/pkg` |
| [glaze-installer](https://github.com/jroark/glaze-installer) | The offline macOS/Windows desktop installer | OS images, `tools/`, `userdata/`, `firmware/`, C sources for its helpers; every repo's `out/pkg` |

## Layout

Check the repositories out side by side, next to the emulator:

```
~/src/glaze-os   ~/src/glaze-apps   ~/src/glaze-emulators
~/src/glaze-web  ~/src/glaze-installer   ~/src/be300   (~/src/be300-dev for GlazeBoot.exe)
```

Each repository's paths to the others default to these siblings and can be
overridden: `GLAZE_DIR` (or `GLAZE_SDK`, `GLAZE`, `GLAZE_INCLUDE` for narrower
uses), `APPS_DIR` and `EMU_DIR`.

## Build order

1. glaze-os: `./build.sh` (OS image and system packages), then `./build.sh sdk`.
2. glaze-apps and glaze-emulators: `./build.sh` (Docker, mounting `../glaze-os` for the SDK).
3. glaze-web: `make store`, which builds the catalog over all three repos' `out/pkg`.
4. glaze-installer: stage and package against the glaze-os build and the app repos.

## Testing across repositories

- **Scripted UI tests:** glaze-os's `tests/run.sh` runs scripts from any repository
  (`CF=out/cf.img ../glaze-os/tests/run.sh tests/ui/t_minesweeper.txt 34`). It copies
  an outside script into its `out/input/` for the containerized build.
- **App repos' native tests:** they compile the glaze-os sources they need from the
  sibling checkout, so a change to libui or libgfx can break them. Run `make test` in
  glaze-apps and glaze-emulators after changing shared libraries.
- **SDK and ABI changes** (`lib/include/glaze`, exported library symbols) affect every
  app repo. Run `make sdk-test` here, then rebuild the app repos against the new SDK.

## App Store versions

Published store assets are immutable per version. A package whose bytes change
needs a version bump in its `Info` before glaze-web can publish it.
