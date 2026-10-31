# Glaze BE-300 USB Installer

Development test installer. Installs from stock CE and restores the exact saved
CE NAND snapshot using the existing verified NAND transaction protocol. No CF
card, Python installation, compiler, or preinstalled device helper is needed by
an end user. Windows needs WinUSB; the GUI provides Zadig instructions.

The workflow is Detect → Prepare installation → review the backup path → Install
Glaze. Prepare RAM-boots Glaze and saves a two-pass physical backup. CE's volatile
object store is cleared by RAM boot, so save those files beforehand. Internal CE
staging files are present in the saved NAND snapshot. Keep the complete generated
backup directory permanently. Restore CE accepts its `installation.json`.

The app pins the USB port and the writer compares every source byte before
programming. Reconnect to the recorded port for restore. Restore needs running
CE or Glaze USB; if both bootstrap paths are lost, external recovery is required.
No recovery claim is made for a completely unbootable device.

## Build payloads once

From this worktree, with `mips-cross-dev` Docker image and the BE300 CE toolchain:

```sh
./build.sh all glazeboot BUILD_ID=cc8c1c4-installer-test
make glazeboot-exe usb-probe-exe BE300_DEV=/path/to/be300-dev
```

Transport `out/glaze.bin`, the four files `GlazeBoot.exe`, `UsbProbe.exe`,
`kernel.bin`, `romfs.img` under `out/glazeboot/`, and the source checkout to each
native packaging host. The build ID passed to packaging must match the compiled
kernel. CE binaries and Glaze payloads must come from matching source revisions.

## macOS

Use Python 3.11 with working Tk, CMake, pkg-config, libusb and PyInstaller 6.16.0.
Homebrew packages: `python@3.11 python-tk@3.11 cmake pkg-config libusb`.
Build separately on Apple Silicon and Intel hosts. Select native Homebrew's
pkg-config to avoid accidentally linking the other architecture's libusb.

```sh
python3.11 -m venv out/installer-venv
out/installer-venv/bin/python -m pip install -r installer/requirements.txt
cmake -S installer -B out/installer-native -DCMAKE_BUILD_TYPE=Release
cmake --build out/installer-native -j8
out/installer-venv/bin/python installer/package.py --build-id cc8c1c4-installer-test
```

Outputs: `out/installer/Glaze-Installer-macos-ARCH.dmg`, the `.app` under
`out/installer/dist`, and `SHA256SUMS`. macOS bundles are ad-hoc signed local test
builds, not notarized. Minimum OS compatibility depends on Python/Tk and libusb;
the local build uses Homebrew Sequoia binaries and has not been validated on
macOS 13. Public distribution and signing credentials are outside this version.

## Windows x64

Use native Windows Python 3.11 including Tk, Inno Setup 6 (`iscc` on PATH),
and MSYS2 UCRT64 GCC, CMake, pkg-config and libusb. In an MSYS2 UCRT64 shell,
install `mingw-w64-ucrt-x86_64-{gcc,cmake,pkgconf,libusb}`. Add the native Windows
Python and Inno Setup directories to PATH. Keep native Windows Python first;
MSYS Python is not suitable for this package.

```sh
python -m venv out/installer-venv
out/installer-venv/Scripts/python -m pip install -r installer/requirements.txt
cmake -S installer -B out/installer-native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/installer-native
out/installer-venv/Scripts/python installer/package.py --build-id cc8c1c4-installer-test
```

Output: `out/installer/Glaze-Installer-windows-x64.exe` plus `SHA256SUMS`.
The current Windows packaging uses a console-capable executable so worker JSON
can be piped reliably. A console accompanies the GUI in these development builds.
Driver installation is a separate, guided administrator action in Zadig and
replaces the legacy sync driver. Uninstalling the application preserves backups
and does not uninstall the USB driver.

## Tests and limitations

```sh
python3 tests/installer.py
make -C host nand-image-tools nand-ecc-test nand-boot-test nand-policy-test nand-backup-test
make -C host nand-program-test nand-writer-test BE300_SOURCE=/path/to/be300
python3 tests/physical_nand.py
python3 tests/nand_transaction.py
```

Native helpers require an NTFS backup volume on Windows (atomic publication uses
hard links). Failures stop automatic writes; restore always takes a fresh source
snapshot and generates a new plan. Do not force-close the app during programming.
Hardware interruption recovery, Windows driver binding, and clean-machine GUI
behavior require platform/hardware validation before public release.

Resource manifests bind bundled firmware and saved installation artifacts to
SHA-256. They are integrity checks, not signatures. Native executables are handled
by the platform packager because signing relocates their load commands. Logs and
backups stay local; diagnostic logs exclude firmware and entropy payload bytes.
