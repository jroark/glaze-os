# Glaze starter app

Set `GLAZE_SDK` to the extracted SDK, then run `make` to build
`build/app.gzp`. `make check` checks the package and executable;
`make cf` creates a card image for installing it from Files.

`make run GLAZE_NAND=/path/to/glaze.bin BE300=/path/to/be300` boots a
disposable NAND copy with that card. Unlock, open Files > Card, tap
app.gzp, install it, then return Home to launch the new icon.

On a BE-300 already running Glaze with USB enabled, `make deploy`
uploads, installs and launches the package. Unlock the device first.

`Info` is your package identity and version. Put resources under `res/`;
keep `res/icon.gzi` as a 44x44 GZI image. Add C sources to `SOURCES` in
Makefile. `app.h` contains the title shown inside the app. Keep saved data
under `res_bundle()/data`: updates preserve it, uninstallation removes it.
See the SDK's `docs/SDK.md` for API details and platform limits.
