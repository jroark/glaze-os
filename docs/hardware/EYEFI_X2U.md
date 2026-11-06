# Eye-Fi X2 Utility: static inspection

## Result

The inspected Windows X2U 1.1.1 package contains no identified card firmware
image. Its card interface exposes configuration, status, network setup and log
operations. No card-firmware flashing, raw flash/memory readback, or arbitrary
packet/socket interface was established. X2U therefore provides no demonstrated
route to turn the card into a general network adapter.

This is a static inspection of the Windows package below, not a hardware test
of X2U and not proof that the card has no undocumented commands. X2U and its
installer were never executed. The BE-300/card were not accessed or changed.
The Mac X2U binary was not acquired or inspected.

## Acquisition and package identity

The installer was retrieved from an archived original vendor URL:

```
https://web.archive.org/web/20161021130848id_/https://download.eyefi.com/x2-windows/EyefiX2Utility.exe
```

- Size: 8,853,328 bytes.
- SHA-256: `2d77721fceacd065ad23c9d9dbfb1dc3f04d057e583f4446dfcaf5d2e4cc9157`.
- A separately retrieved community ZIP contains an identical executable.
  This corroborates file identity, not an independent verification of signing trust.
- The outer installer's version resource says 3.4.29.0 / Eye-Fi Center, but
  the payload's managed assembly is `EyeFiX2Receiver`, version **1.1.1.0**.
  The outer version must not be mistaken for the application or card version.

No vendor binaries or decompiled code are added to source control. All acquired
and derived artifacts are under ignored `out/eyefi-research/x2u/`.

## Unpacking

7-Zip extracts the outer PE's overlay as `installer/[0]`. That overlay consists
of two sequential zlib streams followed by a small file table/SFX trailer:

| Payload | Compressed bytes | Expanded bytes |
| --- | ---: | ---: |
| `vcredist_x86.exe` | 2706300 | 2745256 |
| `Setup.msi` | 5743698 | 7036928 |

The remaining 106 bytes include filenames, lengths/offsets and the `EYEFISFX`
trailer. The MSI contains `media1.cab`; extraction yields 60 files totaling
15,543,448 bytes, including the application, configuration libraries, native
card DLL, runtime/dependency files, localizations and graphics. No standalone
firmware payload was identified in this inventory.

The two main analyzed components are:

| CAB entry | Size | SHA-256 |
| --- | ---: | --- |
| `EyeFiCard.dll` | 35072 | `4821cbdf45b7062031b161379b141577e2a1eed74aedc0d2c8975efcd9d7a5ab` |
| `EyeFiReceiverAppExe` | 5064992 | `e74b0a0f8a34df2712c66e32937cac69df8124b1dcfa703ca72614208d5546d0` |

`EyeFiCard.dll` is native x86, with its own version resource 5.2.0.0. This
desktop DLL version is also independent of the card's 5.0018 firmware.

## Native card interface

Ghidra 12.0.4 analyzed the native DLL and exported its non-thunk functions.
PE exports and managed P/Invoke declarations agree on these twelve operations:

- Initialize and terminate a card handle.
- Get status, scan networks and list configured networks.
- Test/add/delete a network configuration.
- Get/set a card token.
- Check a FAT volume identifier.
- Retrieve the card log.

The transport operates on `EYEFI` mailbox files `REQM`, `REQC`, `RSPM`, and
`RSPC`, using 16 KiB transfers, Windows file I/O, and explicit buffer flushes.
It creates missing mailboxes. This agrees with the bootstrap and transport
already implemented in Glaze.

`EyeFiCardWrapper_GetToken` at **0x100031b0** calls **0x10002880**, which sends
`'o'` plus a one-byte token identifier. `EyeFiCardWrapper_SetToken` at
**0x100031d0** calls **0x10002cf0**, which sends `'O'`, a one-byte token,
a one-byte payload length, and the payload. A generic token setter is not
evidence that undocumented tokens expose arbitrary memory or firmware writing.

The DLL's `EYEFIFWU.BIN` string is at virtual address **0x10006540**. Its direct
code reference is in the get-token routine's token **2** (firmware information)
branch. The routine builds a path, calls `PathFileExistsW`, and if present calls
`DeleteFileW` at **0x10002cc5**. It does not create or flash that file. This is
an important distinction from a simple firmware-filename string search.

## Managed application inspection

The receiver, `EyeFiCardCommon.dll` and `ConfigData.dll` were inspected as .NET
metadata and IL with `dnfile`/`dncil`, without loading them for execution.
All three modules' method bodies were exported for review. Method-name scans
found no firmware/flash/raw-memory methods; this scan alone would be insufficient,
so the actual native card entry points and their callers were also examined.

The receiver's `EyeFiCardWrapper` class imports exactly the native operations
listed above. Its token constants describe identity, registration/upload keys,
product features, upload policy, direct-mode passphrase and timeouts, and marking
mode. Actual `SetToken` callers in this wrapper change selective transfer
(tokens 0x27/0x11) and direct-mode timeout settings (0x24/0x25). No firmware or
memory-writing caller was established.

Activation handling includes card detection, identity/key retrieval and local
settings. Legacy online registration/reporting code is still present in
`RecordCardActivation`; no claim is made that every X2U activation path works
offline or against today's services. No keys were retrieved from the user's
card, and key material/literals are intentionally not reproduced in this report.

## What this establishes for Glaze

X2U confirms the storage-mailbox approach and provides additional reference
material for network setup, direct mode, transfer configuration and logs.
It does not supply a firmware dump or a demonstrated general network transport.
Recovering an actual card firmware image or finding a separate readback/debug
path remains necessary for custom firmware work.

## Artifacts and reproduction

Local files include:

- `X2U-wayback.exe`, `community-download`: original captures.
- `installer/`, `Setup.msi`, `msi/`, `payload/`: extracted package layers;
  `unpack_sfx.py` records and validates the two zlib streams.
- `inventory.json`: per-file sizes and SHA-256 hashes.
- `card-decompiled.c`, `ghidra.log`: native analysis output and completion log.
- `EyeFiReceiverAppExe.il.txt`, `EyeFiCardCommon.dll.il.txt`, `ConfigData.dll.il.txt`:
  managed IL output; `dump_managed.py` records the extraction procedure.

Extraction uses 7-Zip without installing the package:

```sh
7zz x -oout/eyefi-research/x2u/installer out/eyefi-research/x2u/X2U-wayback.exe
python3 out/eyefi-research/x2u/unpack_sfx.py
7zz x -oout/eyefi-research/x2u/msi out/eyefi-research/x2u/Setup.msi
7zz x -oout/eyefi-research/x2u/payload out/eyefi-research/x2u/msi/media1.cab
```

Hash checks, zlib stream boundaries, matching independent installer captures,
the native export table, the managed assembly version/P/Invoke declarations,
and the filename reference's imported calls were checked. No dynamic execution
or flashing tests were performed.
