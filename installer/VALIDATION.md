# Validation — 2026-10-31

Built from isolated branch `codex/usb-gui-installer`, base `cc8c1c4`.
Bundled firmware BUILD_ID: `cc8c1c4-installer-test`.

Passed:

- Matching NAND image, RAM kernel/romfs, GlazeBoot.exe and UsbProbe.exe builds.
- Installer native helpers compile and link for macOS arm64 and x86_64.
- Windows MinGW compilation of the helper, transport, client, PC Connect,
  backup, transaction writer, inventory and filesystem builder sources.
  Windows linking, runtime and packaging remain unverified.
- Nine installer tests, including production image encoding and a fresh-source
  restore plan against synthetic NAND, without any physical device access.
- Four physical NAND tests and five transaction tests.
- Native NAND ECC, boot map, raw policy, two-pass backup, low-level programming,
  and transaction-writer suites. Writer tests include source drift, bad blocks,
  ordering, verification failures and filesystem preservation.
- Apple Silicon app packaging, resource-manifest verification, strict deep
  ad-hoc signature verification and GUI initialization/clean shutdown smoke test.

Artifact: `out/installer/Glaze-Installer-macos-arm64.dmg` (about 16 MiB),
with checksum in `out/installer/SHA256SUMS`.

Incomplete:

- A CF-free USB/emulator integration attempt could not reach CE USB enumeration.
  The disposable stock CE restore image stalled before the CLI ran, with the
  emulator reporting a framebuffer write while LCD clock gates were off. The
  harness then timed out in `peer.offer()`. This is not a passing USB test.
- Windows setup EXE and Intel macOS DMG need their native Python/Tk packaging
  environments. This session had no authenticated Windows runner. Build recipes
  and the Inno Setup definition are included, but those artifacts were not built.
- Clean-machine OS compatibility, Windows WinUSB driver setup, and physical
  install/restore/interruption recovery have not been tested. No physical NAND
  erase or programming was performed.
- The Apple Silicon package uses Homebrew's Sequoia binaries; compatibility with
  macOS 13 has not been established.

These are local development builds. Public signing, notarization and publication
were not requested and were not performed.
