# Stowaway input regression checks

Run `make -C host stowaway-test` for the native ASan/UBSan decoder and editor tests.
Boot `INIT=bin/ktest` builds with and without `--stowaway-keyboard`, each with and
without `--ne2000`; require `ktest: ALL PASS` and no `ktest: FAIL`.

For an offline Browser fixture:

```sh
./build.sh INPUT_SCRIPT=tests/t_browser.txt \
  'WEB_FILES=assets/web/start.html tests/web/stowaway.html'
cp out/glaze.bin /tmp/glaze-stowaway.bin
be300 --nand /tmp/glaze-stowaway.bin --rtc-host-time --stowaway-keyboard
```

Tap the address field, type `res:stowaway.html`, and press Enter. Tab should
focus Name, Password, and Notes in order, skipping the disabled field and
wrapping to Name. Check shifted punctuation, Backspace/Delete in the middle
of a value, Home/End and arrows, newline insertion in Notes, and Enter submission
from Name. Long multiline input should scroll inside the textarea to the cursor.

Also check Notes creation/editing and persistence, Files new-folder/rename and
Enter-to-save, and Browser address entry. With a detected dock, editing controls and
focus remain while the keyboard area becomes available to content. Without the
dock, touch keyboards should keep their existing layouts and character behavior.
Hardware Home must move the cursor without closing the app; Rocket still closes
it. Typing on the lock or home screen must not activate touch controls or be
replayed into the next app. Hold a character or arrow to check repeat starts
at 450 ms, advances every 50 ms, and stops on release.

Capture stdout and stderr and stop the emulator with SIGTERM to save a screenshot.
Use disposable image copies: emulator shutdown writes NAND back to its input.
Test images built with `INPUT_SCRIPT` or `WEB_FILES` overrides are not release
images; finish with plain `./build.sh`.
