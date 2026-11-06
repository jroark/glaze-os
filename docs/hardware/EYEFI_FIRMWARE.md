# Eye-Fi X2 firmware acquisition and initial analysis

Follow-up: [X2 Utility package and card-interface inspection](EYEFI_X2U.md)
recovers and analyzes the complete Windows X2U 1.1.1 package.

## Result (2026-11-03)

No verified X2 firmware image has been acquired. The physical card runs
`5.0018 Oct 30 2012 14:21:10`, established through its mailbox interface.
The recovered binaries are desktop installation bootstraps, not card firmware.
Their contents and download paths have been analyzed without executing them.
No card writes, radio configuration changes, or flashing occurred in this
acquisition/analysis session.

All vendor binaries, extracted resources, and decompiled output remain under
ignored `out/eyefi-research/`. Only this report is intended for source control.

## Recovered artifacts

Both files were downloaded read-only from the card's factory `START HERE`
directory using `glaze-usb get`, then inspected with 7-Zip. These hashes identify
the captured files; they are not an independent verification of vendor authenticity.

| Artifact | Card path | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| `Eye-Fi-card.dmg` | `/Card/START HERE/Mac OS X/Eye-Fi.dmg` | 897184 | `c136fd425b6e1edc62631dc3fb6638dfa3b22415357c6fcf1fed7ac8c9a810bd` |
| `Setup-card.exe` | `/Card/START HERE/Windows/Setup.exe` | 719752 | `e89821e77b0bc489fc885b4e26e74a3809d35bcec57377e2d67e009b37e5bb08` |

The Mac DMG contains an HFS+ volume with `Eye-Fi Installer.app`, version
3.4.13, build `2011091515`. Its executable is a universal PowerPC/i386 Mach-O.
The Windows file is a 32-bit x86 PE executable, version 3.4.13.0, with a PE
timestamp of 2011-09-15. These desktop version numbers are independent of the
card's firmware version.

The recovered bundles contain installer code/resources and manifests; no card
firmware payload was identified. The card's separate AUTORUN directory contains
a launcher, locales, and an icon, not an identified firmware image.

## Bootstrap analysis

The Mac manifest contains:

```
version="3.4.13"
build-id="2011091515"
update-config="http://support.eye.fi/agent/updates2/osx-update.cnf"
```

The Windows executable's 272-byte overlay, extracted by 7-Zip as `[0]`, starts
with a 214-byte zlib stream. Decompression produces a matching XML manifest
with `win32-update.cnf` instead of `osx-update.cnf`. The remaining 58 bytes
contain an `app.manifest` UTF-16LE filename and an `EYEFISFX` trailer. The
trailer has not been fully reverse engineered. A separate plaintext string
also references `http://support.eye.fi/agent/updates2/win32-update.dat`;
that string alone does not establish that the active path uses it.

The i386 slice of the Mac executable was imported into Ghidra 12.0.4. Retained
symbols identify the following functions; addresses apply only to this slice:

| Address | Function | Observed behavior |
| --- | --- | --- |
| `0x42dc` | `UpdaterController2::getUpdateUrl` | Opens local XML manifest, reads `version` and `update-config`, fetches remote configuration, reads `app-version` and `download-url` attributes. |
| `0x3d46` | `UpdaterController2::getRemoteConfig` | Downloads the remote configuration through cURLpp. |
| `0x35d4` | `UpdaterController2::downloadUpdates2` | Downloads the selected payload, uses user agent `Eye-Fi Updater/1.5`, reports whether HTTP status is 200. |
| `0x48d6` | `UpdaterController2::downloadUpdates` | Starts configuration/download work using a worker thread. |

Strings additionally reference `/tmp/EyeFiInstaller.dat`, `/tmp/Eye-Fi`, and
`/tmp/Eye-Fi/Eye-Fi.mpkg`, including package installation scripts. Together with
the decompiled configuration/download functions, this establishes a desktop
bootstrap chain. It does not establish an X2 flash protocol or signature scheme.

Ghidra completed analysis and exported the four functions to
`out/eyefi-research/updater-decompiled.c`. It reported warnings about external
C++ address-table/demangling application; the output needs normal manual review
and is not a reconstruction of original source.

## Firmware acquisition evidence and limits

- Public searches for `EYEFIFWU`, 5.0018, and X2 update images yielded protocol
  references and release descriptions, but no verified firmware download.
- Internet Archive metadata queries returned no matching software items. This
  does not exclude firmware bundled inside an unrelated archive item.
- A Wayback CDX query for archived `api.eye.fi` firmware URLs returned `[]`.
  Other updater-index requests returned HTTP 503 or 429. Further availability
  queries were stopped after rate limits; archive coverage remains unresolved.
- The original `support.eye.fi` configuration hostname failed DNS resolution,
  including outside the command sandbox.
- The TechSpot Windows mirror returned HTTP 403; no file from that mirror was
  accepted or analyzed. Its listing is not evidence of an acquired image.

The historical
[firmware-fetch script](https://github.com/hansendc/eyefi-config/blob/master/eyefi-firmware-fetch.pl)
requests an XML firmware response by MAC/version, base64-decodes it, inflates
the zlib data, and saves the result. It fetches an update package from a server;
it is not installed-firmware readback. It was inspected but not executed.

The reference client's experimental update code writes a 524288-byte
`EYEFIFWU.BIN.2.0001` to `EYEFIFWU.BIN` and reboots the card. This is an old
experimental path, not validation for X2 firmware 5.0018, and was not run.

The author's [2009 report](https://dave-hansen.blogspot.com/2009/10/eye-fi-releases-source-code.html)
describes per-card firmware acceptance on earlier hardware. The
[2017 X2 discussion](https://groups.google.com/g/eyefi-config-discuss/c/KY-NKYi-3_A)
recalls a card-specific, possibly encrypted header. Neither source supplies a
verified X2 binary here, nor proves a particular encryption/signature algorithm.

The next required artifact is the full desktop package named by an archived
update configuration, an installed Eye-Fi Helper/Center executable from an old
installation, or a captured X2 firmware update. The full helper is the target
for locating `UpdateFirmware`, firmware-download parsing, card acceptance data,
and the actual file/command sequence. No raw-flash readback command or general
network transport has been established from these bootstraps.

## Local reproduction

```sh
out/host/glaze-usb get '/Card/START HERE/Mac OS X/Eye-Fi.dmg' out/eyefi-research/Eye-Fi-card.dmg
out/host/glaze-usb get '/Card/START HERE/Windows/Setup.exe' out/eyefi-research/Setup-card.exe
7zz l out/eyefi-research/Eye-Fi-card.dmg
7zz l out/eyefi-research/Setup-card.exe
```

The extracted trees are `mac-installer/` and `windows-installer/`. The saved
`analysis.json` records sizes, hashes, provenance, URLs, and function addresses.
`windows-app.manifest` is the decompressed overlay manifest.
`mac-installer-symbols.txt` and the two `*-strings.txt` files preserve the
static inspection results. `ghidra-scripts/EyefiUpdater.java` records the
decompiler export filter; `ghidra.log` records analysis completion.
