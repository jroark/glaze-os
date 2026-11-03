# Mac classic Bluetooth serial echo helper

Build with `make -C host bt-echo` on macOS. This creates an ad-hoc-signed app in
`out/host/GlazeBluetoothEcho.app`; it uses Apple's public IOBluetooth APIs.
`--self-test` validates the service dictionary without advertising a service.

For an end-to-end test, use the card address printed by `glaze-usb bt-status`:

```sh
GLAZE_BT_TEST_PIN="$(openssl rand -hex 4)"
export GLAZE_BT_TEST_PIN
out/host/GlazeBluetoothEcho.app/Contents/MacOS/GlazeBluetoothEcho \
    --allow CARD_ADDRESS --pin "$GLAZE_BT_TEST_PIN"
```

The helper prints the Mac address and allocated RFCOMM channel. In another
terminal with the same temporary PIN, use those reported values:

```sh
out/host/glaze-usb bt-connect MAC_ADDRESS "$GLAZE_BT_TEST_PIN" CHANNEL
out/host/glaze-usb bt-serial-status
out/host/glaze-usb bt-echo 'Hello from Glaze over Bluetooth'
out/host/glaze-usb bt-disconnect
```

`CARD_ADDRESS`, `MAC_ADDRESS` and `CHANNEL` are placeholders. The channel is
important: on the tested Mac, a normal SDP SPP query returned a separate `COM5`
service on channel 3, while the helper's working channel was 1. Use the helper's
reported channel rather than assuming the first SPP record is the echo endpoint.
Automatic SDP selection remains useful for devices with a single SPP service.

The Mac pairing delegate supplies the specified temporary PIN through
`IOBluetoothDevicePair`. It initiates pairing when the helper starts and when
matching connections are observed; start the Glaze connection promptly afterward.
No global Bluetooth preference or security-policy changes are made. macOS may
ask for Bluetooth permission or pairing confirmation. The helper only accepts
RFCOMM traffic from the explicitly allowed card and echoes bounded chunks.

Glaze keeps the PIN/link key in RAM only. A warm serial reconnect can reuse its
key. After a Glaze reboot, macOS can retain a stale bond and time out without
requesting a PIN. In Bluetooth settings, forget this test card, then restart the
helper and reconnect with a matching temporary PIN. This recovery was verified
after loading the full app image. macOS manages its own pairing record.
No helper pairing files are written. Command-line PINs are temporary
test credentials and may be visible in process arguments; do not reuse a real
password here.

Stop the helper with Ctrl-C. It closes its channel and removes its transient
service. The first version uses one authenticated/encrypted outbound session,
127-byte RFCOMM frames, bounded queues and one selected peer. Echo tests reject
unread data rather than silently consuming data from a previous operation.

Validated on this Mac and a physical BE-300 with the SMART/Brainboxes BCSP card:
legacy pairing, encryption, service/channel setup and exact 31-/192-byte echoes.
Native tests: `make -C host bluetooth-test bluetooth-serial-test`.
