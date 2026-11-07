# Hardware-support packages

Some hardware support doesn't fit in the NAND romfs beside the built-in apps
(its slot ends at 0x500000). Until now each was a build option, and a NAND
image that wanted one had to drop apps (`NAND_DROP=wince`):

| Package | Supplies | Build option it replaces |
|---|---|---|
| `hw-pccards` (PC Card Drivers) | `drivers/3c589.ko`, `drivers/hermes.ko` | `PCCARDS=1` |
| `hw-modem` (Dial-up Modem) | `bin/net` with lwIP's PPP and the Hayes dialer | `MODEM=1` |
| `hw-wl54cf` (WL54-CF Firmware) | `firmware/cf8385_helper.bin`, `firmware/cf8385.bin` | `WL54CF=1` |

Each is now also a `.gzp` that installs like an app, from Files on the flash
volume, and works on a stock image without a rebuild. The build options
still work: a romfs copy always wins over a package, and GlazeBoot builds
keep `PCCARDS=1 MODEM=1` by default.

GPS needs no package: `kernel/gps.c` and `drivers/cfuart.ko` are built in,
and Maps is an ordinary app package in glaze-apps ([GPS](../hardware/GPS.md)).

## Format

A hardware package is the GZPK container of `glaze/pkg.h`, with `Info`
saying `type=hardware`, and no executable:

```
Info          id=hw-pccards / name=PC Card Drivers / version=1.2 / type=hardware
Manifest      struct gz_hw_manifest, gz_hw_entry[count], [64-byte Ed25519 signature]
hw/<path>     each file the manifest lists, e.g. hw/drivers/hermes.ko
```

`lib/include/glaze/hwpkg.h` defines the manifest. It names the package id
and the OS version (`VERSION`) the code was built for. Each entry has the
romfs-style path the kernel looks up, the size, the kind (code or data) and
the SHA-256. Paths are `drivers/<name>`, `bin/<name>` (code) or
`firmware/<name>` (data), with a single plain name component.

`pkg_install` puts it in `/Applications/<id>.app` like an app, so the
transactional install, recovery and `pkg_uninstall` are shared. There are
two differences:
- It installs on the flash only (`GZ_EPERM` for `/Card`): the CF socket may
  hold the very card it drives.
- `pkg_list` leaves it out; `pkg_list_hardware` lists these packages instead.

`struct pkg_info` is unchanged for SDK apps. After installing or removing a
hardware package, `pkg.c` asks the file server to re-register (`FS_HW_RESCAN`).

## Trust

Kernel modules run with kernel privilege, and `bin/net` gets `PROC_NET`, so
code from the writable flash is accepted only with a signature. The rules:

- A manifest with any code entry must carry the OTA release key's Ed25519
  signature (`config/ota_pubkey.bin`, the same key and wolfCrypt code that
  verify OS updates). It must also name exactly the running `GLAZE_VERSION`,
  since modules are built against one kernel's structures.
- Every file's bytes must match the manifest's SHA-256 before the kernel
  uses them. The file server only delivers bytes; it can't change what they
  hash to.
- Firmware (`firmware/`) may come in an unsigned manifest. It runs on the
  card's own processor, not the VR4131; WL54-CF is an I/O-mapped card with
  no bus mastering. Vendor firmware can't be redistributed, so its package
  is built locally from the user's own files.
- Romfs wins: a package can't replace a built-in driver or firmware file.
  It can replace only a service the trusted catalog marks
  `"overridable": true` (only `net`; never a required service). The service
  starts with the catalog's privileges, not the package's.

## How a file gets to the kernel

The kernel holds only the registered manifests (`kernel/hwfile.c`, 16 files
in all), never the files themselves until something needs one.

1. **Registration.** At startup, and on `FS_HW_RESCAN`, the file server
   reads each `type=hardware` bundle's `Manifest`. It checks that the
   manifest's id names its folder, and passes it in with `SYS_HW_CTL`
   (`GZ_HW_OP_BEGIN`/`ADD`/`END`). The kernel checks the signature and
   version, then bumps a generation.
2. **Request.** When the network dispatcher (`net.c`) or the service worker
   (`service.c`) needs a file that romfs lacks, `hwfile_open()`:
   - allocates pages for it;
   - sends the file server `GZ_NOTIFY_HW`;
   - returns `GZ_EINPROGRESS`.

   The caller tries again on its next 100 ms worker pass. Nothing in the
   kernel waits on a user process. Before the file server has registered,
   every lookup answers `GZ_EINPROGRESS`, so a card present at cold boot
   waits for its driver rather than being given up on.
3. **Delivery.** The file server takes the request (`GZ_HW_OP_FETCH`) and
   streams `/Applications/<id>.app/hw/<path>` in 16 KiB steps
   (`GZ_HW_OP_SUPPLY`), serving other clients in between, as it does for
   OTA updates. The kernel hashes the bytes as they arrive. The file is
   handed out only when the digest matches; otherwise it is marked bad until
   the packages are next registered.
4. **Use.** The consumer copies what it needs: a module's sections, a
   service's image frames, or firmware streamed into the card. Then
   `hwfile_trim()` frees the pages.

The pieces that use it:
- **Drivers.** `module_load()` reads through `hwfile_open()`; it can load a
  verified file in place.
- **Pending and missing drivers.** `net.c` keeps a socket whose driver is on
  its way claimed and "pending". A card whose driver no one has stays
  reserved, and is tried again when the registration generation changes.
  So installing `hw-pccards` with a 3c589 already in the jacket brings the
  card up without a reboot.
- **WL54-CF firmware.** `wl54cf.c` reads its firmware through
  `hwfile_open()` (a module import). `net.c` fetches both files before the
  driver's init, which must never wait.
- **Replacement services.** For an overridable service, `service.c` asks
  `hwfile_open_package()` first. It spawns a verified static ELF from kernel
  pages with `proc_spawn_verified()`; `image.c` reads the source with
  `as == NULL`, which no syscall can pass. A changed registration restarts
  the service, so removing `hw-modem` returns to the romfs `net`.

Settings → About → Hardware Support lists the installed packages, using
`GZ_HW_OP_STATUS`:
- **Ready:** the kernel registered it.
- **Not accepted:** a wrong key or OS version.
- **Damaged:** a file didn't match its manifest.

Each package's page shows its files and a Remove button.

## Building

```sh
./build.sh hwpkg-files        # the payloads, in the container: out/drivers/*.ko, out/modem/romfs/bin/net
make hwpkgs                   # natively: sign with GLAZE_OTA_KEY, pack out/pkg/hw-*.gzp
make hwpkgs GLAZE_OTA_KEY=out/testkey/test.pem   # a test key, for an image built with OTA_PUBKEY=
make hwpkg-wl54cf             # from local firmware/wl54cf only; never commit or distribute it
tools/mkhwpkg.py verify --pub config/ota_pubkey.bin out/pkg/hw-pccards.gzp
```

Signing needs OpenSSL 3 and the release key, as `make ota` does. The
packages are tied to the `VERSION` they were built with: rebuild and re-sign
them for each release. `bin/net` with PPP is built in its own `OUT=out/modem`,
because the option changes lwIP's structures.

## Testing

- `make -C host hwfile-test` runs natively, with OpenSSL. It covers:
  - registration, and fetches in odd chunk sizes;
  - digest, signature, unsigned-code, wrong-key and other-OS rejection;
  - malformed manifests, over-supply, a failing file server and a restart
    mid-file;
  - page accounting.

  `tools/mkhwpkg.py`'s own checks and `verify` run with it.
- `make -C host net-driver-test service-test module-test` cover:
  - a pending, then missing, then installed driver, and firmware prefetch;
  - a replacement service while it is fetched, then with a fallback;
  - loading a module in place.
- The emulator, with an image built with `OTA_PUBKEY=` a test key.
  `tests/hwpkg_fs.py` unpacks packages into `FS_EXTRA` entries for a
  pre-installed factory volume. These were run:
  - **`--jacket --3c589` and `--jacket --hermes`:** the driver is fetched
    and loaded; with `hw-modem`, `net` also comes from the package (DHCP
    over the 3c589).
  - **`tests/ui/t_hw_install.txt`:** installs `hw-pccards` from Files with
    a 3c589 already in, and the card comes up.
  - **A ktest image with `hw-modem` (no `MODEM=1`):** `tests/run_modem.sh`'s
    dial-up passes.
  - **`--wl54cf` with `hw-wl54cf`:** the firmware loads from the package.
  - **Rejections:** the release key rejecting test-signed packages, and a
    flipped byte in an installed driver.
