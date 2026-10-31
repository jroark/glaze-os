# Glaze BE-300 USB installer

One offline download per platform:

- `Glaze-Installer-windows-x64.exe`: setup, GUI launcher, private Python/Tk runtime,
  libusb, all 29 app choices, matching firmware, and Zadig USB driver setup.
- `Glaze-Installer-macos-arm64.dmg`: self-contained Apple Silicon app, Applications
  shortcut, instructions, native helpers, all app choices and matching firmware.

No CF card, system Python, build tools, separate firmware or additional runtime
download is needed. The app makes no network requests. The Windows USB driver
setup is guided and uses the bundled Zadig; it may request administrator access.

## End-user flow

Open the Mac app or run Windows setup. Connect USB and external power and wake
stock Windows CE. Existing GlazeBoot staging files are compared byte for byte and
reused when they match. Free-space checks cover only missing or changed files
plus a small workspace; previous files stay intact until a verified replacement
is ready. Choose apps, prepare the installation, choose a permanent backup
folder, then review and click Install Glaze. After GlazeBoot, the app refreshes
the USB connection automatically if macOS retains CE’s old device state. If a
previous attempt stopped at that step, reconnect USB and click Prepare again;
the matching read-only Glaze RAM session can be reused without rebooting CE. Files, Settings, and system services
are required. Built-ins are selected by default; the 17 additional packages start
unselected. Selected packages are preinstalled in `/Applications` on NAND. The
app chooser also supports Select all and Required only.

Preparation repacks the NAND ROMFS to omit unchecked built-in executables while
retaining services and shared resources. It populates the fresh filesystem with
selected package bundles and builds a device-specific physical image using the
actual bad-block inventory. The saved installation record includes selected app
IDs and the exact image/transaction hashes. Changing choices requires preparing
a new image. Restore always targets the original saved CE NAND, independent of
app choices. CE compatibility and game engines still need user-supplied software
or game data; those external files are not included.

Save important CE RAM files first: RAM bootstrap clears CE's volatile object
store. Internal CE staging files are included in the saved NAND snapshot. Keep
the complete backup directory permanently. Restore accepts its `installation.json`
and needs a working CE or Glaze USB bootstrap at the recorded USB port. Backups
must be outside the application's install directory; use NTFS on Windows because
atomic publication uses hard links. Driver replacement may affect old CE sync
software; uninstalling the app does not remove the driver or erase backups.

## Build matching payloads

Use the `mips-cross-dev` Docker image and the BE-300 CE toolchain:

```sh
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
