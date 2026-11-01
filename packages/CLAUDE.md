# Installable packages

Read with the root CLAUDE.md, which covers building, testing and toolchain rules.

`packages/<name>/` holds an installable app (sources, `Info`, `icon.gzi`): it is linked like a built-in app but never put in romfs, and `tools/mkromfs.py --package` packs it as `out/pkg/<name>.gzp` (dynamic builds only). Packages are not on the factory volume; put them on a card image (`tools/mkcfimg.sh out/cf.img 16 16 out/pkg/*.gzp`) and install them from Files. `tools/mkgzi.py` turns a PNG into a GZI icon. `hello` is the test package (ktest uses it); `minesweeper` and `solitaire` are games, which keep their saved game in `<bundle>/data/state` (`CF=out/cf.img tests/run.sh tests/ui/t_minesweeper.txt 34`, likewise `t_solitaire.txt`; their taps in Files assume a card holding only hello, minesweeper and solitaire). `tests/run.sh` takes `NET=1` to boot with `--ne2000`, and `FS_FROM=<nand image>` to boot another run's file system (apps installed from a card, then a network boot). `aim` is an AIM client speaking OSCAR to a server the user names (AOL's are gone; test against a self-hosted Open OSCAR Server): `make -C host aim-test` checks its protocol engine (`packages/aim/oscar.c`) natively, `--live host port sn pw [buddy msg]` signs on to a real one, and `tests/ui/t_aim_install.txt` then `tests/ui/t_aim.txt` drive the app. `gmail` is a Gmail client speaking IMAPS (`imap.gmail.com:993`) and SMTPS (`smtp.gmail.com:465`) over wolfSSL with an App Password (or a custom IMAP/SMTP server): `make -C host gmail-test` checks its MIME, IMAP4rev1 and ESMTP engines (`packages/gmail/mime.c`, `imap.c`, `smtp.c`) natively, `--serve <imap_port> <smtp_port>` runs a local mock IMAP/SMTP server, `--live` tests a live server, and `tests/ui/t_gmail_install.txt` then `tests/ui/t_gmail.txt` drive the app. Files in `packages/<name>/res/` are packed as `res/<file>`; AIM's and Gmail's sounds come from `tools/mksounds.py --aim packages/aim/res` and `tools/mksounds.py --gmail packages/gmail/res`.

`make out/pkg/doom.gzp` builds the DoomGeneric port (no WAD is packed or
fetched). Platform I/O, buttons, indexed video and PCM effects live in
`packages/doom/`; pinned engine patches are in `third_party/doomgeneric/UPSTREAM.md`.
`make -C host doom-test` uses synthetic data; `DOOM_WAD=/external/doom1.wad`
adds real-engine movement/audio/save/load checks. See `docs/DOOM.md`.

`make out/pkg/chatgpt.gzp` builds the direct Codex chat package. Its device-code
OAuth, login import, token refresh and SSE client live in `packages/chatgpt/`;
`tools/chatgpt_signin.py` uses a dedicated official Codex CLI login for browser
authorization. `make -C host chatgpt-test` runs native protocol/TLS fixtures.
See `docs/CHATGPT.md`. Credentials and `CODEX.AUTH` exports must never be committed.
