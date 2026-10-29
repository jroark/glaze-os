# Networking through the USB cable

Glaze can use its existing USB sync connection as an IPv4 network interface.
No PCMCIA network card is needed. A host helper, `glaze-usb usb-net`, carries
bounded Ethernet frames over the existing bulk endpoints. On macOS it opens
the built-in **utun** interface, translates IPv4 packets to/from Ethernet and
answers Glaze's ARP requests. No kernel extension or TAP driver is required.
On Linux it creates a TAP Ethernet interface.

This is a Glaze protocol with a host helper, not a plug-and-play CDC ECM,
NCM or RNDIS USB gadget. The BE-300's proven descriptors, endpoint pair and
controller initialization remain unchanged. Windows CE does not provide this
interface. Boot an updated Glaze kernel, network service and USB service first.

## macOS

Build the host helper with Apple's command-line tools and Homebrew:

```sh
brew install libusb pkgconf
make -C host glaze-usb
sudo out/host/glaze-usb usb-net
```

The helper prints the allocated `utunN` name, configures it and runs in the
foreground. `sudo` is needed to create/configure the virtual network device.
An optional unused name can be requested: `usb-net utun12`. Automatic allocation
is preferable when VPNs also use utun interfaces. The helper uses fixed argv
to `/sbin/ifconfig`; it does not run a shell or modify existing interfaces.

| Endpoint | Address |
| --- | --- |
| Computer | `192.168.7.1` |
| Glaze | `192.168.7.2` |
| Glaze mask | `255.255.255.252` (`/30`) |
| Glaze gateway | `192.168.7.1` |
| Glaze DNS | `1.1.1.1` |

Verify the local link in a second Terminal:

```sh
ping 192.168.7.2
```

Settings → Network shows **USB Network**, the address and MAC. Its
Ping Router action checks the reverse path. Glaze apps use their usual socket
APIs: TCP, UDP, DNS and HTTPS do not need USB-specific application code.
The host can reach services listening on Glaze and Glaze can reach services
listening on the host's `192.168.7.1` address.

## Internet access

The local link works without Internet Sharing. Reaching the internet also
requires the computer to forward/NAT traffic from `192.168.7.2` to its uplink;
the helper deliberately does not change host forwarding, DNS or firewall policy.

On macOS, use a deliberately configured PF NAT anchor for the allocated utun
interface and enable IPv4 forwarding. For example, **if the uplink is `en0`**,
the NAT rule is `nat on en0 inet from 192.168.7.2 to any -> (en0)`. The
anchor must be referenced by the host's PF ruleset; loading a disconnected
anchor alone has no effect. Forwarding is enabled with
`sudo sysctl -w net.inet.ip.forwarding=1`. Preserve existing VPN/firewall rules
and account for their forwarding policy. The macOS Internet Sharing settings
do not automatically configure an arbitrary utun interface. Internet setup
is separate from the local USB link and has not been exercised in this cloud.

The fixed subnet must not overlap a LAN or VPN route. A /30 host route to
Glaze is installed by ifconfig; avoid using this link when a conflicting
`192.168.7.0/30` network is already in use. IPv6 and jumbo/VLAN frames are not
supported. The MTU is 1500 bytes.

## Debugging on the same cable

Pair and enable [network debugging](NETWORK_DEBUG.md) **before** starting the
USB helper:

```sh
out/host/glaze-usb net-keygen /private/tmp/DEBUG.KEY
out/host/glaze-usb net-enable /private/tmp/DEBUG.KEY
sudo out/host/glaze-usb usb-net
```

In another Terminal:

```sh
out/host/glaze-usb --host 192.168.7.2 --key /private/tmp/DEBUG.KEY probe
out/host/glaze-usb --host 192.168.7.2 --key /private/tmp/DEBUG.KEY logs
```

The physical USB handle belongs to the helper. A separate parser and replay
cache pump the tunnel while the regular debugger accepts paired TLS sessions
through the IP link. Raw USB debugger commands require stopping the helper.
The tunnel does not enable the TLS debugger or distribute pairing keys.
`--trace` suppresses tunnel packet payloads, which can contain application data.
RAM reload or reboot resets the USB link: restart the helper after Glaze boots;
the new image must also contain USB networking. A network debugger's reconnect
window does not itself restart the helper.

## Lifetime and card selection

USB networking is disabled at boot. Starting the helper allocates two bounded
eight-frame queues and makes USB the selected interface. The network service
starts on demand, even with no card; PCMCIA storage remains available. The
physical card's claim and driver remain intact if one is installed, but its
traffic is not mixed into the USB subnet. Glaze currently has one routed
interface: enabling/disabling USB restarts the network service and invalidates
existing sockets. Stop/reconnect application networking after switching links.

Ctrl-C, DTR drop, a USB reset, cable removal, helper inactivity for ten seconds,
or USB service termination takes the link down. Queues are invalidated in the
reset path and reclaimed by the deferred service worker. With no other network
hardware, the network service stops. With a working card, routing returns to
that card and its normal DHCP/Wi-Fi configuration.

## Linux

```sh
make -C host glaze-usb
sudo out/host/glaze-usb usb-net             # new nonpersistent TAP glzusb0
ping 192.168.7.2
```

Requires libusb development headers, `/dev/net/tun`, and CAP_NET_ADMIN (normally
provided by sudo). An optional unused interface name can be supplied. The
helper sets its MAC, MTU and `192.168.7.1/30` address. Closing it removes the
nonpersistent interface. Internet access requires the host's forwarding/NAT
configuration just as on macOS.

## Validation and limits

```sh
make -C host usb-net-test net-driver-test service-test usb-driver-test
make -j4 all glazeboot
```

Native ASan/UBSan tests run the production host packet pump and device tunnel
parser over fragmented byte streams and datagram-pair interfaces. They cover
both the TAP and Darwin utun packet format, ARP translation, malformed packets,
backpressure, duplicate/reordered requests, CRC rejection, timeout/disconnect
cleanup and partial writes. Kernel tests exercise real queue handling, copy
fault atomicity, allocation failure, generation invalidation, card precedence
and deferred release. USB controller tests use the emulator's independent
hardware model.

The cloud host is Linux without a real USB device or usable TAP/utun adapter.
Native macOS compilation, macOS interface creation/forwarding and physical
BE-300 traffic still need testing on a Mac. These limits are not substitutes
for the native protocol/queue tests or the MIPS image build.
