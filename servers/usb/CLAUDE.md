# USB server

Read with the root CLAUDE.md, which covers building, testing and toolchain rules.

Network development transport is in `servers/usb/network.c`, sharing the
USB dispatcher's capabilities and lifecycle. `glaze-usb --host HOST --key FILE`
uses TLS 1.2 ECDHE-PSK; it is off by default. See `docs/dev/NETWORK_DEBUG.md`.
`make -C host debug-net-test` runs native paired-TLS tests; the opt-in
`NETDEBUG_TESTS=1 INIT=bin/debug_guest DEBUG_RELOAD_ADDR=HOST_LAN_IP` image and
`tests/debug_runtime.py` check real lwIP/RPC and reload/reconnect. Rebuild normally
after testing. Never commit `DEBUG.KEY`, other pairing files, or fixture images.

USB networking (`docs/hardware/USB_NETWORK.md`) uses `kernel/drivers/usbnet.c` queues
and `servers/usb/ethernet.c`'s independent framed tunnel parser. macOS uses
utun (IPv4 plus an AF_INET prefix, ARP adaptation); Linux uses TAP. Native
`make -C host usb-net-test net-driver-test service-test usb-driver-test` covers
packet framing, backpressure/replay and lifecycle. USB takes routing precedence
while the host helper runs; changing transport restarts the network service.

Screen mirroring (`docs/dev/MIRRORING.md`) adds a localhost-only VNC bridge to
`glaze-usb mirror`. Device snapshots/deltas are in `servers/usb/mirror.c`;
the host bridge/RFB engine are `tools/glaze-usb/mirror.c` and `rfb.c`. Input is
capability-gated and shares timed input's disconnect release state. Use
`make -C host mirror-test usb-input-test usb-driver-test`; the network guest
fixture also checks complete mirror CRCs and live input. No capture buffer
exists until a viewer connects; no VNC listener is opened on the device.
