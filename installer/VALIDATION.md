# Validation — 2026-10-31

Branch `codex/usb-gui-installer`, base `cc8c1c4`.
Bundled firmware BUILD_ID: `cc8c1c4-installer-test`.

Produced:

- A standalone Windows x64 setup EXE (about 23 MiB), including Python/Tk, libusb,
  Zadig, firmware and all optional packages.
- An Apple Silicon macOS DMG (about 18 MiB), including the app, Applications
  shortcut and offline instructions.
- Files and checksums collected under `out/installer/downloads`.

Passed:

- Matching NAND, RAM kernel/romfs, CE loaders and package builds.
- Thirteen installer tests and five app-selection tests. These cover executable
  exclusion, required-app preservation, aligned ROMFS repacking, bundle layout,
  malformed archive rejection, actual image encoding and fresh restore plans.
- Four physical NAND tests, five transaction tests, and native NAND ECC,
  boot-map, raw-policy, backup, programming and transaction-writer suites.
- Apple Silicon packaged bundle self-check: firmware/catalog integrity,
  production ECC encoding and startup of all three native helpers without USB.
- Apple Silicon GUI and app-chooser initialization, clean shutdown and strict
  deep ad-hoc signature verification.
- Both the all-apps candidate and a required-only candidate booted through the
  emulator ROM to SpringBoard without CF, preserving quarantined NAND blocks.
- Windows setup installed silently into an isolated Wine 11.21 prefix. The
  installed private Python 3.11.9 runtime passed the bundle self-check, and
  the installed GUI/app chooser initialized and closed successfully.
- The installed Windows runtime prepared a full physical NAND image selecting
  all 29 apps, using the real Windows encoder/filesystem helpers and a synthetic
  CE backup with bad blocks. USB operations were simulated; no hardware was used.

USB reconnect fix:

- Reproduced the physical macOS failure: the host retained CE revision 0000
  and configuration 0 after GlazeBoot; setting configuration returned
  `LIBUSB_ERROR_OTHER`.
- Resetting the unclaimed USB device recovered the connection. A live HELLO
  returned the expected Glaze build and read-only NAND backup capabilities.
- The installer now mirrors the existing CLI: reset after three seconds of
  failed reconnects, then retry every five seconds within a 90-second deadline.
  Regression tests cover stale state, reset failure, timeout, and capability
  mismatch. Preparation can continue from the same matching read-only RAM boot.
- This diagnostic reset/probe did not erase or program physical NAND.

CE staging reuse fix:

- Production PC Connect tests verify exact file matches, missing and changed
  files, shorter/longer remote files, multi-chunk reads, EOF boundaries, and
  read/close failures. Comparison makes no CE filesystem mutations.
- Production staging planner tests cover matching large artifacts on a nearly
  full volume, allocating only changed files, missing local inputs, and stopping
  on failed inspection. Previous staged files are preserved.
- On the physical BE-300, all five existing artifacts matched and were reused,
  including the 4,669,440-byte ROMFS. The fresh nonce execution check passed,
  GlazeBoot launched without reuploading boot files, and the automatic USB reset
  reconnected to the expected read-only Glaze RAM build. No raw NAND was programmed.
- Reused boot files still go through the fresh nonce-based execution check.
  Changed files keep the existing temporary upload/read-back/atomic publication
  path; no unchecked executable is launched.

Incomplete:

- Wine checks do not validate a physical Windows machine or USB driver binding.
  WinUSB setup and actual install/restore/interruption recovery need hardware
  validation. No physical NAND erase or programming was performed.
- A CF-free USB/emulator attempt with a stock CE restore image stalled before
  USB enumeration. The emulator reported a framebuffer write with LCD gates
  off; the CLI never ran. This is not a passing end-to-end USB test.
- Intel macOS packaging still requires an Intel Python/Tk environment. The
  Apple Silicon bundle uses Homebrew Sequoia binaries; macOS 13 compatibility
  and clean-machine Gatekeeper behavior have not been established.
- Public signing, notarization and publication were not performed.
