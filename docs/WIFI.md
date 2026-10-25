# Wi-Fi and network security

Glaze provides Settings > Wi-Fi for the Z-Com XI-825 (PRISM) and AmbiCom
WL54-CF (Marvell CF8385). Select an open network to join, or a secured
network to enter its key. Passwords are masked; Show Password reveals them.
Done closes the keyboard and exposes security, WEP options, and Join.

| Card | Open | WEP 40/104 | WPA Personal / TKIP | WPA2 Personal / CCMP |
| --- | --- | --- | --- | --- |
| XI-825 | yes | yes | unavailable | unavailable |
| WL54-CF | yes | yes | yes | yes |

The PRISM station host interface requires host data-cipher processing for
WPA/TKIP and CCMP; this version uses firmware data encryption only. Settings
reports unsupported security explicitly. It does not flash firmware or
silently downgrade a network. WPA Enterprise, WPA3, protected-management-frame
requirements, ad-hoc mode, and hidden-network entry are not implemented.
Physical-card interoperability remains unvalidated.

WEP accepts 5 or 13 printable ASCII bytes, or 10 or 26 hexadecimal digits.
Key slots 1–4 and open/shared authentication are selectable. WPA accepts an
8–63 character ASCII passphrase or a 64-digit hexadecimal PSK. WPA2 prefers
CCMP pairwise encryption and also accepts a TKIP group cipher advertised by
the access point. Enterprise-only networks are rejected.

The last explicit choice and its credentials are saved in
`/Library/Preferences/net.wifi` and reconnected at boot. Disconnect disables
automatic connection while retaining that record. Forget Network disconnects
and deletes it, including after a failed connection or explicit disconnect.
Only one network is remembered. Preferences use the existing local filesystem;
they are not encrypted at rest. Legacy version-1 open-network preferences are
migrated to version 2. Credentials never appear in status replies or logs.

## Architecture

`glaze/wifi.h` supplies status, scan, open connect, secured connect,
disconnect, and forget operations. SSIDs have explicit lengths up to 32 bytes;
scan replies contain at most 16 networks. Security capabilities and progress
are separate from the fixed-size `net_status` reply, whose reserved halfword
now identifies the device. Existing syscall and request numbers are preserved.
`SYS_WIFI_CTL` and raw-frame access remain restricted to `PROC_NET`.

The shared network dispatcher recognizes both wireless identities before
NE2000 probing and reserves a recognized card even if initialization fails.
WL54-CF initialization uploads the existing locally extracted firmware and
leaves the card disconnected. Runtime commands scan, select an AP, configure
WEP/RSN, associate, and install pairwise/group keys. PRISM uses resident
station firmware and its WEP RIDs. See [WL54CF.md](WL54CF.md) for firmware
extraction and register references.

A minimal PSK-only port of wpa_supplicant 2.11 runs inside the network server.
It derives the PSK, verifies EAPOL-Key MICs and replay counters, performs the
four-way and group-key handshakes, and prevents repeated key installation.
EAPOL frames reach it before lwIP. The kernel permits only EAPOL while WPA
authentication is incomplete; normal traffic and DHCP start only after
required keys and authorization. Failure, disconnect, removal, and switching
networks revoke traffic and clear queued packets, leases, and DNS. The card
must be rebooted/reinitialized after removal; live replacement is deferred.

Factory filesystem images receive a private 32-byte seed from the build
host's CSPRNG. The network server advances and saves it before releasing
nonce bytes, and refuses authentication if the seed is missing or cannot be
saved. The seed is not in ROMFS or source control. Copies or rollbacks of NAND
also copy or roll back its random state; generate separate images/seeds for
separate devices. Existing images without the seed support open/WEP but need
a freshly provisioned seed for WPA. The lwIP timing PRNG is never used for
WPA nonces.

## Verification

Use the matching be300 security worktree for model tests:

```sh
make -C host net-driver-test settings-test xi825-test wl54cf-driver-test \
  wifi-supplicant-test BE300_SOURCE=/path/to/be300-security
```

The supplicant test uses an independent OpenSSL authenticator and published
PBKDF2/AES-wrap vectors. It checks WPA/WPA2, rekey, duplicate messages without
key reinstallation, malformed input, wrong passwords, bad MICs, and random
source failure. Override `OPENSSL_FLAGS` and `OPENSSL_LIBS` outside the default
Homebrew location. Driver tests use structural firmware fixtures, without
vendor binaries, and run under ASan/UBSan.

## Cards inserted at run time and real hardware

Either card can be inserted and pulled at run time (`kernel/drivers/cf.c`'s
`pcmcia_poll`, `net.c`), which is how a GlazeBoot boot gets one: the network
server attaches to a card that appears and stops the supplicant when it goes.
On a real BE-300 the XI-825 (station firmware 1.3.5) scans with the host scan
RID 0xFCE5, which firmware before 1.3.1 lacks (it falls back to the scan
inquiry), and its command deadlines follow Linux hostap's. The WL54-CF's real
CIS has CONFIG at attribute 0x200.

For guest tests, extract local firmware as described in WL54CF.md, build the
emulator with OpenSSL available, then:

```sh
./build.sh WL54CF=1 INIT=bin/ktest
make -C host wifi-mkfs
/usr/bin/python3 tests/run_wifi_security.py \
  --emulator /path/to/be300-security/build-host/be300 --host-ip <host-LAN-IPv4>
```

The runner builds disposable filesystems with separate entropy seeds and
saved choices. It tests XI-825 open/WEP (40/104-bit), legacy preference migration,
WL54-CF open/WEP (40/104-bit)/WPA/WPA2, Ethernet,
no card, and CF storage. Wireless cases run again with the persisted NAND.
Guest checks require authentication before DHCP, gateway/DNS/TCP/UDP, local
HTTP, disconnect cleanup, Forget Network, and reselection. Logs and shutdown
screenshots are saved under `out/wifi-security-*`; the source NAND is unchanged.
On macOS use native Python: an older Intel Python can fail to receive LAN
connections even when external guest HTTP succeeds.

The virtual AP is an authentication and firmware-key-installation fixture.
Its Ethernet frames represent already decrypted firmware payloads; it does
not emulate RF encryption. Real-card testing is needed before claiming
physical WEP/TKIP/CCMP interoperability.
