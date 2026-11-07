# Stowaway input regression checks

Run `make -C host stowaway-test` for the native ASan/UBSan decoder and editor tests.
Boot `INIT=bin/ktest` builds with and without `--stowaway-keyboard`, each with and
without `--ne2000`; require `ktest: ALL PASS` and no `ktest: FAIL`.

For an offline Browser fixture:

```sh
./build.sh INPUT_SCRIPT=tests/ui/t_browser.txt \
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
Fn+Left (Home) must move the cursor without closing the app; Rocket and the
keyboard's Today key still close it. Typing on the lock or home screen must not activate touch controls or be
replayed into the next app. Hold a character or arrow to check repeat starts
at 450 ms, advances every 50 ms, and stops on release.

Capture stdout and stderr and stop the emulator with SIGTERM to save a screenshot.
Use disposable image copies: emulator shutdown writes NAND back to its input.
Test images built with `INPUT_SCRIPT` or `WEB_FILES` overrides are not release
images; finish with plain `./build.sh`.

## Physical keyboard (Targus / Stowaway) bring-up

The keyboard takes the dock connector, so a docked BE-300 has no USB or serial.
Debug it over paired network debugging (docs/dev/NETWORK_DEBUG.md) on a Wi-Fi
card, with a DHCP reservation so `reload` reconnects to the same address.

1. With the USB cable attached and no keyboard, boot Glaze, join Wi-Fi, and pair:
   `out/host/glaze-usb net-keygen DEBUG.KEY`, then `glaze-usb net-enable DEBUG.KEY`.
   Check `--host IP --key DEBUG.KEY probe`, `logs --once` and `screenshot`.
2. Build a trace image with `./build.sh glazeboot WL54CF=1 STOWAWAY_TRACE=1`.
   `WL54CF=1` keeps the Wi-Fi firmware in the reloaded image. Load it with
   `glaze-usb --host IP --key DEBUG.KEY reload` (about 3.5 minutes over Wi-Fi),
   then unplug USB and dock the keyboard; it's picked up live. Read the trace
   with `logs --once`, which drains the buffer, so save its output:
   - `input: dock socket NN` on every change: 08 is the serial dock, 0c the USB
     cradle, 0f nothing docked (the emulator says 07). 0e, 0a and 0c pass by
     while the connector slides in.
   - `dock control XXXX -> YYYY`: Windows CE's powered-up value for a serial
     dock is 1f08; 2008 or 200c is powered down.
   - modem status (MSR) through the handshake and every byte received. A Targus
     raises DCD (`98`) about 0.4–0.6 s after DTR, drops it 200 ms later, and
     sends `fa fd` about 20 ms after the RTS edge that follows 500 ms on.
   - after `input: Stowaway keyboard connected`, `stowaway: rx NN` for every
     byte. Bit 7 set is a release; the last release of a burst comes twice.

   If the dock reads 08 but no `fa fd` arrives, boot through GlazeBoot instead,
   where Windows CE's serial.dll opens `COM1:` and powers the port itself. If the
   keyboard answers there but not after a NAND boot, the NAND power-up is
   incomplete. If it answers in neither, suspect the keyboard or its handshake.
3. Type every key, plain and with Shift, Fn, Ctrl and Alt. Compare the scancodes
   with `kernel/drivers/stowaway.c` and the emulator's `src/stowaway_keys.def`.
4. Undock and redock: the log should show `disconnected`, the dock IDs, and
   `connected` again. Sleep and wake with it docked: it shakes hands again.
5. Rebuild without `STOWAWAY_TRACE` before flashing or releasing anything.

The device sleeps when idle unless a debug session is open, and its battery
runs down quickly with Wi-Fi on. A `Goodbye` screenshot is usually one of
those, so check the log for `power off` and its reason.

The dock power-up follows Windows CE's socket.dll; kernel/drivers/stowaway_hw.c
has the addresses.
