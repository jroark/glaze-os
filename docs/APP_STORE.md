# Glaze App Store

The built-in App Store has Search and Updates tabs. Search lists every app
before a query is entered and matches names, developers, summaries and descriptions.
Apps are free and curated in `store/apps.json`; there are no accounts or payments.
The catalog includes Hello, Fifteen, Sketch, Minesweeper, Solitaire and Task Manager.

## Build and publish

```
./build.sh
./build.sh store
```

The OS image is `out/glaze.bin`. Publish the complete `out/store/` directory
to a static HTTPS host configured for `appstore.glaze-os.com`. No application
server, database or JavaScript is needed. The default native client requests
`https://appstore.glaze-os.com/catalog.txt`.

Configure the host's custom domain, then add the CNAME or A/AAAA records it
provides for `appstore` in the `glaze-os.com` DNS zone. Provision a publicly
trusted certificate for `appstore.glaze-os.com`. Glaze uses IPv4 and TLS 1.2/1.3
with the OS's bundled roots. A correct device date is needed for validation.

Serve assets directly, without login pages, bot challenges or redirects.
Disable content compression: the streaming client requests identity encoding
and rejects compressed responses. Content-Length, chunked coding and
connection-close framing work. Use `Cache-Control: no-cache` for the catalog
and `Cache-Control: public, max-age=31536000, immutable` for versioned assets.
The included `_headers` helps on hosts supporting that format; configure
those headers separately elsewhere. Upload assets first and replace the
catalog last. Retain older assets for clients with cached catalogs.

The AWS backend uses private S3 storage and CloudFront. See
[App Store on AWS](APP_STORE_AWS.md) for infrastructure, initial setup and the
release publisher. Credentials stay in your AWS CLI profile.

## Add or release an app

1. Add `packages/<id>/main.c`, `Info`, and a 44x44 `icon.gzi`.
2. Add its developer, summary and description to `store/apps.json`.
3. Build and run `./build.sh store`.
4. Test installation and a data-preserving update before publishing.

The generator derives ID, name and version from the package, checks the MIPS
executable and icon, and computes package/icon SHA-256 digests. Descriptions are
single-line printable ASCII, matching Glaze's fonts. Numeric dotted versions
have at most six digits per component and fifteen characters total. `1.10`
follows `1.9`; `1` and `1.0` compare equal. Bump the version for every changed
release: reusing a version with different bytes fails generation.

Publish packages tested against the current OS libraries: v1 has no separate
OS compatibility resolver or package signing system. HTTPS authenticates
distribution; SHA-256 detects mismatched or damaged assets.

The `GZSTORE1` text catalog contains `[app]` records with `id`, `name`, `version`,
`developer`, `summary`, `description`, `package`, `icon`, `size`, `sha256` and
`icon_sha256`. Paths are relative to the store root; external origins and
traversal are rejected. Malformed, duplicate or incomplete records invalidate
the catalog. Limits are 32 apps, a 64 KiB catalog, 8 KiB icons and 2 MiB packages.

## Installation and recovery

When a Card is mounted, new installations offer Flash or Card; otherwise they
use Flash. Updates retain their volume. Downloads stream into a scratch file
on that volume, followed by size, hash, ID and version checks. Home terminates
the store; downloads do not continue in the background. Cancelled or abandoned
scratch files are removed. Catalogs and verified icons are cached under
`/Library/Caches/AppStore` for offline browsing.

`pkg_install` stages new files and a copy of the existing bundle's `data/` in
`<id>.app.tmp`. Packages cannot supply `data/` or `.pkg-ready` entries. A ready
marker records the source bundle path. An existing destination is renamed to
`<id>.app.bak`, then staging is renamed to the live bundle to commit. Lookup
and listing roll back an interrupted swap or finish cleanup after commit.
Cross-volume moves defer deletion of an absent source Card until it returns.
Insert that source Card again before performing another replacement of the
same app. Insufficient space aborts staging and leaves the old app and data. Uninstalling
removes the bundle and its data.

## Local development and tests

```
python3 -m http.server 18765 --directory out/store
./build.sh APPSTORE_URL=http://HOST_LAN_IP:18765 INPUT_SCRIPT=tests/t_appstore.txt
be300 --nand COPY_OF_OUT_GLAZE_BIN --rtc-host-time --ne2000
```

`APPSTORE_URL` is an explicit build-time fixture override, not an end-user
setting. Use the host's LAN address: the guest cannot reach host loopback.
Changing the endpoint rebuilds the store. Ship the default HTTPS endpoint,
without scripted input, and always boot a copy of the NAND image.

```
make -C host store-catalog-test
make -C host store-package-test
make -C host store-http-test
```

The installer harness runs production code on filesystem fixtures, interrupting
every mutation for replacement on Flash and moves to/from Card. The transport
harness starts local HTTP/HTTPS fixtures and checks framing, bounds, cancellation,
trusted TLS and rejected certificates. It needs permission for loopback sockets.
Existing `ktest` cases also exercise device package installation, saved-data
preservation and launch. `tests/store_fixture.py` offers Hello 1.0 on its first
catalog request and 1.1 thereafter for `tests/t_appstore_install.txt`. That
scenario installs and launches the original three apps, draws in Sketch, and checks
Hello after updating; allow at least 160 seconds of guest time. The
`t_appstore_minesweeper.txt` and `t_appstore_solitaire.txt` scenarios each
install a new game from the scrolled catalog, play it, and reopen its saved
game. Run these with a local store endpoint and allow 70 seconds of guest time.
`t_appstore_taskmanager.txt` installs Task Manager from the six-app catalog and
exercises its tabs, process details and pause/resume. Allow 75 guest seconds.
See [Task Manager](TASK_MANAGER.md) for its kernel ABI and offline checks.

Regenerate original store and launch-app artwork without rewriting other art:

```
swift tools/mkassets.swift assets/gen assets/fonts userdata/Pictures --store-only
```
