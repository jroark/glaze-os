# Device entropy

Glaze can bootstrap a missing random seed from CPU/memory execution timing
jitter on the BE-300. It does not require a CF card, USB host, network traffic,
or a Windows CE crypto provider. Collection uses the pinned Jitterentropy
3.6.3 implementation, with its normal startup and continuous health checks.
Raw timer bits, MAC addresses and wall-clock time are not used as seeds.

`gz_entropy_seed(out, size)` in `<glaze/random.h>` collects 1–256 bytes,
returning zero on success or a negative Glaze error. Collection is serialized
per process, and failed output is erased. The collector is released after each
request. Use it to seed a cryptographic DRBG rather than for bulk random data.
`gz_entropy_load_seed(path, seed)` loads an exact 32-byte saved seed or collects
fresh device-local material; callers retain ownership of saving and ratcheting.

The first collection takes seconds on this CPU. TLS consumers perform it on
their existing network workers when their saved seed is missing or malformed.
Browser, App Store, radio, Chat and Gmail use the fallback. TLS reports an RNG
error if collection fails instead of falling back to bare timing bytes. Saved
per-app seeds retain the existing fast path and seed persistence behavior.

WPA/WPA2 continues to prefer the existing factory, boot-card or USB host seed.
If none is present, the network server collects and saves a 32-byte local seed
before scanning/associating, avoiding a collection delay during EAPOL exchange.
The initial Join request can therefore take longer. The existing Wi-Fi ratchet
still saves its next state before releasing nonce bytes. Open/WEP connections
do not wait for collection. A RAM boot collects again after reset unless a
host or card supplies a seed.

The OS and exported SDK build the collector at `-O0`, as upstream requires.
Its port and provenance are documented in
[`third_party/jitterentropy/UPSTREAM.md`](../third_party/jitterentropy/UPSTREAM.md).
No new kernel syscall or privileged hardware access is needed.
The extra collector code uses image space: use the standard GlazeBoot app
set if `GB_APPS=all` plus Wi-Fi firmware exceeds the existing 4.75 MiB limit.

## Verification

```
make -C host entropy-test store-http-test chatgpt-test radio-worker-test wifi-supplicant-test
./build.sh glazeboot
./build.sh sdk-test
```

The native tests check saved-seed reuse, missing/short/oversized seed files,
provider failures, output erasure, TLS seed-failure propagation and slow seed
collection without a spurious TCP timeout. Actual
upstream startup checks reject frozen and deterministic incrementing clocks.

Initial measurements on a real USB-attached BE-300 running a GlazeBoot RAM
boot collected 196,608 raw timing samples. Four isolated collector runs each
passed startup and continuous health checks and generated 256 bytes in about
47.6 seconds; an incrementing-clock control was rejected. This is hobby-device
validation on one device, not an SP800-90B entropy-rate/restart assessment or
cryptographic certification. Emulator timing is not evidence of hardware
entropy; its collector may fail and host/card seeds remain available.

The integrated shared-library API was also tested on the same physical device:
an absent per-app seed was collected in 14.9 seconds, saved as 32 bytes, then
loaded unchanged in 11 ms. No seed contents were printed or downloaded.
