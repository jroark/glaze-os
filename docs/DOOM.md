# Doom for Glaze

Doom is an installable, standalone Glaze app using the pinned DoomGeneric engine.
It runs the original single-player software renderer with BE-300 buttons and
streamed sound effects. It needs no network or privileged hardware access.

## Install and supply game data

Build `make out/pkg/doom.gzp` using the normal MIPS toolchain (or
`./build.sh packages`). Copy the package to a CompactFlash card and open it in
Files to install on flash or the card. Launch Doom from the home screen.

Put your own `.wad` IWAD in `/Documents` or anywhere on `/Card`. The app browses
folders and pages through large directories. Choose the WAD to start it.
Supported games are vanilla-compatible Doom shareware, Doom, Ultimate Doom,
Doom II, TNT and Plutonia. Compatible Freedoom IWADs can also be selected, though
large maps may exceed the classic engine's limits or the available RAM.
PWAD add-ons, DeHackEd, multiplayer and modern source-port features are not
supported. Heretic and Hexen are different engines and are not supported.
The directory validator rejects invalid headers, overflowing directory/lump
ranges, files above 32 MiB and directories above 16,384 lumps. This is still a
classic engine: use trusted, compatible game files.

**No WAD or ROM is in the repository, package, factory image or build downloads.**
You must supply game data separately. Keep any downloaded or uploaded WAD outside
the checkout, or in ignored `out/`, including freely licensed and shareware data.

## Controls

There is no on-screen gameplay controller. The full picture and status bar fit
in a 240x180 area, correcting Doom's non-square pixels to 4:3, below navigation.

| Control | During play | Doom menus |
| --- | --- | --- |
| Up / Down | Forward / backward | Move selection |
| Left / Right | Turn | Adjust setting |
| OK | Fire (hold to repeat) | Select / confirm yes |
| Esc | Use / open doors | Back / confirm no |
| Menu in navigation | Open / close Doom menu | Open / close Doom menu |
| Pause in navigation | Pause and show app actions | Pause and show app actions |
| Rocket / Home | Suspend to SpringBoard | Suspend to SpringBoard |

The pause sheet provides Resume, Doom menu, Save slot 1, Load slot 1, Map,
a run toggle, weapon choices 1–7 and Quit. Quit uses Doom's own confirmation;
press OK to confirm or Esc to cancel. Reopen the app to select a different IWAD.

A Stowaway keyboard also supports text, weapon keys 1–7, Space (use), Z/X
(strafe), P (Doom pause), Enter and Tab (automap). Text input arrives as pulses;
physical directional and OK/Esc buttons support held input. The native Doom
save menu can name its six slots using a hardware keyboard; the pause sheet
saves slot 1 as “Glaze save” without needing one.

## Saves, resources and limits

Saves go to `<bundle>/data/<iwad-directory-hash>/saves/`. The namespace hashes
IWAD header/directory metadata, not the entire game. Modified game data with
identical metadata can share a namespace; don't mix saves between different
versions. Files are compatible with the engine's vanilla save format.
Save explicitly before Home or quitting: Home suspends without autosaving, and
SpringBoard may reclaim suspended apps. WAD access uses buffered, random reads;
the app does not copy a whole WAD into RAM. Keep a card containing the active WAD
inserted until quitting.

The 4 MiB zone, indexed framebuffer and eight-voice 11,025 Hz mono PCM mixer fit
the 16 MiB device budget. Other open apps or complex maps may exhaust it. Failed
audio initialization allows silent play. The port has **sound effects but no
MIDI/MUS music synthesis**. Options changed in Doom's menus last for this process;
upstream config-file persistence is disabled in this DoomGeneric configuration.

Buffered save writes and close must succeed before replacing an existing save.
Flash supports rename replacement; FAT needs removal of the old destination
before a second rename. A completed `temp.dsg` remains recoverable if that rename
fails or power is lost in that FAT replacement window. An incomplete write never
replaces the previous slot. Don't power off while saving. Fatal errors close
sound and show a dismissible message. Close and relaunch to retry.

## Validation

`make -C host doom-test` uses ASan/UBSan on the port's production buffered I/O,
short transfers and write failures, synthetic WAD-directory validation, and
physical/menu key mappings. It needs no game data.

To test the **real engine** with an external IWAD:

```
make -C host doom-test DOOM_WAD=/absolute/path/doom1.wad
```

That optional harness targets the Doom shareware E1M1 and checks rendered frames,
player movement, firing with nonzero PCM, and save/load position restoration.
Leak checking is disabled for this process-lifetime engine run; ASan and UBSan
remain active except historical signed-overflow and shift arithmetic.

`tests/t_doom.txt` installs from a card holding only `doom.gzp` and an external
`doom1.wad`, starts E1M1 with the real button event path, saves/loads, and returns
from Home. `tests/t_doom_reload.txt` reboots that saved filesystem, reinstalls
the package while keeping data, loads directly from the title screen, and quits
using physical OK. Boot a copy of the NAND. See `CLAUDE.md` for scripted emulator runs.
Physical BE-300 performance still requires testing on a device.

Engine provenance and local patches: `third_party/doomgeneric/UPSTREAM.md`.
