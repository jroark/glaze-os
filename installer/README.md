# Glaze BE-300 USB installer

One offline download per platform:

- `Glaze-Installer-windows-x64.exe`: setup, GUI launcher, private Python/Tk runtime,
  libusb, all app and hardware options, matching firmware, and Zadig USB driver setup.
- `Glaze-Installer-macos-arm64.dmg`: self-contained Apple Silicon app, Applications
  shortcut, instructions, native helpers, all app choices and matching firmware.

No CF card, system Python, build tools, separate firmware or additional runtime
download is needed. The app makes no network requests. The Windows USB driver
setup is guided and uses the bundled Zadig; it may request administrator access.

## End-user flow

On the BE-300, first set **Connections → PC Connect → Connection method: USB**.
The serial setting prevents CE PC Connect from responding over the USB cable.
Open the app, connect USB and external power, choose apps if desired, and click
**Install Glaze**. The app detects the device and runs preparation, flashing and
restart automatically after one replacement confirmation. The main window shows
one status line and progress; diagnostic output and manual USB selection are
hidden under **Show details**. No separate Detect or Prepare steps are needed.

If NAND space is insufficient for staging GlazeBoot, use Windows CE to delete
unwanted built-in apps, keeping PC Connect available, then retry installation.
The installer reuses matching staged files and does not delete CE apps itself.

**Save Windows CE backup** and **Verify Windows CE** start enabled and can be
unchecked independently. Backups go automatically to `Documents/Glaze Backups`,
with an optional folder chooser and an **Open backup** button after success.
Skipping backup stores the CE source only in an OS temporary directory, removed
on success or failure. It leaves no CE restore copy from this installation.
Skipping verification uses one source read instead of two and omits the optional
CE executable/nonce self-test. Glaze payload validation, bad-block mapping,
source binding, writer preflight and write verification remain required.

A temporary raw NAND source read is necessary for physical placement and the
existing transaction format even when no backup is retained. Diagnostics stay
in the host's per-user log directory. Restoring a previously saved CE backup is
still available through **Restore Windows CE**.

The app chooser offers built-ins and all 16 additional packages. Files, Settings,
and system services are required; built-ins start selected and optional packages
start unselected. Hello is excluded from this installer, Windows CE compatibility
starts unchecked, and WL54-CF Wi-Fi firmware is a separate unchecked hardware
option. Packages install into `/Applications` on NAND. Unchecked built-in
executables are omitted from the generated ROMFS while services/libraries remain.
Existing GlazeBoot staging files are compared and reused when matching. Only
missing/changed files require additional CE staging space, and previous files
stay intact until verified replacement. The matching read-only Glaze RAM session
can be reused after a failed attempt without rebooting CE.

Save important CE RAM files beforehand: RAM bootstrap clears CE's volatile object
store. Installation replaces CE and its internal files. Keep complete saved backup
directories outside the application's install folder. Restore needs a working CE
or Glaze USB bootstrap at the recorded USB port. Use NTFS for saved Windows
backups because atomic publication uses hard links. CE compatibility and game
engines need user-supplied software or game data. Replacing the Windows USB driver
may affect old CE sync software; uninstalling the app does not remove the driver.

The verified, redistributable Linux WL54-CF firmware pair is bundled for offline
installation on the physical Marvell card, with its full `LICENCE.Marvell` notice
and pinned provenance. Selecting it inserts both firmware files into NAND ROMFS;
deselecting it omits them. It is not added to the temporary RAM boot. Some large
combinations exceed the fixed ROMFS slot: the chooser catches this before install
and asks the user to deselect Windows CE compatibility or another optional app.

## Build matching payloads

Use the `mips-cross-dev` Docker image and the BE-300 CE toolchain:

```sh
python3 tools/fetch_wl54cf_firmware.py
./build.sh all glazeboot BUILD_ID=cc8c1c4-installer-test
make glazeboot-exe usb-probe-exe BE300_DEV=/path/to/be300-dev
```

The packaging build ID must match the compiled kernel. The NAND image, RAM image,
CE loader, all `out/pkg/*.gzp` packages, and source must come from matching builds.
Do not include proprietary ROM or game data in the input tree.

## macOS packaging

Use Python 3.11 with working Tk, CMake, pkg-config, libusb, and PyInstaller 6.16.0.
Homebrew packages: `python@3.11 python-tk@3.11 cmake pkg-config libusb`.
Select native Homebrew's pkg-config when multiple architectures are installed.

```sh
python3.11 -m venv out/installer-venv
out/installer-venv/bin/python -m pip install -r installer/requirements.txt
cmake -S installer -B out/installer-native -DCMAKE_BUILD_TYPE=Release
cmake --build out/installer-native -j8
out/installer-venv/bin/python installer/package.py --build-id cc8c1c4-installer-test
```

`--native-dir` selects an alternative helper build directory. The generated DMG
contains the app, an Applications shortcut, and offline instructions. It is
ad-hoc signed and not notarized. This Apple Silicon build uses Homebrew Sequoia
binaries; compatibility with macOS 13 has not been established. Intel packaging
still requires an Intel Python/Tk environment.

## Windows packaging from macOS/Linux

The Windows setup can be built without a Windows runner. Install a MinGW x64
cross compiler, NSIS, `7z`, and `msiextract`. On macOS:

```sh
brew install mingw-w64 nsis p7zip msitools
python3 installer/package_windows.py --build-id cc8c1c4-installer-test
```

This downloads official CPython 3.11.9/Tk, libusb 1.0.30, and Zadig 2.9 with locked
SHA-256 checksums in `windows-downloads.json`; subsequent builds use the local
cache. It compiles the native helpers and GUI launcher, bundles the private
runtime and driver setup, and uses NSIS to produce one offline setup EXE. It
installs per-user, adds a Start menu shortcut, and offers to launch the GUI.
The driver setup stays a separate guided action. No console window accompanies
the GUI; workers run using the bundled console interpreter with a hidden window.

Downloaded libusb/libwdi redistribution sources and licenses are bundled.
Windows native helpers use system crypto APIs; no wolfSSL runtime is needed.

## Collect and verify

```sh
python3 installer/collect_downloads.py
python3 tests/installer.py
python3 tests/installer_apps.py
python3 tests/physical_nand.py
python3 tests/nand_transaction.py
make -C host usb-pcconnect-test usb-ce-staging-test
```

The ready-to-share files and `SHA256SUMS` are collected under
`out/installer/downloads`. `--check-bundle` verifies bundled resources, encodes a
NAND page and starts every native helper without opening USB. `--smoke-test`
opens the GUI and app chooser briefly, then closes without device access.

These are development builds. See `VALIDATION.md` for the exact checks and
remaining hardware/OS validation. Packaging does not publish a release.
