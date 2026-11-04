# Eye-Fi SDHC storage through an SD-to-CF adapter

A 4 GB Eye-Fi SDHC card works as FAT32 storage on the BE-300 through the
OnSpec `02b6:0003` SD-to-CF adapter. Glaze corrects the adapter's IDENTIFY
response and retries a mount while its media becomes readable.

## Adapter identification

On the physical adapter (`OnSpec`, `Version 1.06`, fixed-disk FUNCID 4),
IDENTIFY returns each byte in the low lane of a word. Reading those values
as ordinary ATA words produced a blank model and 4,390,982 sectors, too
small for the card's existing partition.

The CF driver recognizes this transport only when all three checks match:

- CIS manufacturer/product is `02b6:0003`.
- The first two received words are `0x008a`, `0x0084`.
- All 256 received words have zero upper bytes.

The adapter completes IDENTIFY after 512 bus bytes, so only ATA words
0–127 are available. Glaze packs the low-byte lanes into those words and
clears the unavailable upper half. The model and LBA28 capacity fields
are within the recovered half. Ordinary sector transfers keep their
existing byte-wide CF PIO path.

The corrected result is `SD to CF Adapter`, 7,520,256 sectors. The existing
FAT32 partition starts at LBA 8192 and spans 7,507,456 sectors, fitting
within the corrected capacity.

## Media readiness and mounting

The bridge can identify before it returns valid media sectors: the first
filesystem read of sector zero returned zeroes, while later reads returned
the valid MBR. A single mount attempt therefore left the card unavailable
until reinsertion.

The file server now retries a failed mount at most once per second, for
at most eight attempts per insertion, during normal filesystem requests.
A new insertion restarts the retry allowance. A mounted volume is never
remounted by this retry path, preserving its open handles. No card is
formatted or repartitioned.

## Physical validation

On a BE-300 on 2026-11-03:

- Repeated raw MBR and partition-boot-sector reads matched.
- Corrected IDENTIFY model/capacity were verified after a USB RAM reload.
- The FAT32 volume mounted as `SD to CF Adapter, 3672 MB` without reinsertion.
- A 4,148-byte test file was written, closed and read back byte for byte.
  Cleanup rechecked its contents before removing that test file.

The filesystem has 512-byte sectors, 64 sectors per cluster, 6360 reserved
sectors, two FATs of 916 sectors, and root cluster 2. The on-device storage
checks used a development kernel loaded into RAM. These observations do
not establish behavior for other adapter identities or media geometries.

Captured sectors and test artifacts remain under ignored `out/`; no card
image or user files are included in source control.

## Validation and reproduction

```sh
make -C host cf-boot-test fat-test cf-recovery-tools
python3 tests/cf_recovery.py
```

The native CF test checks adapter identity/signature guards, high-byte
rejection, recovered LBA28 capacity and cleared upper words, alongside the
existing socket and card-detection tests. FAT12/16/32 tests cover initially
blank reads, retry timing and exhaustion, reinsertion, successful delayed
mounting, normal file operations, and filesystem integrity checks.

With a responsive connected BE and this card inserted:

```sh
out/host/glaze-usb cf-info
out/host/glaze-usb cf-read 0 1 out/eyefi-mbr-new.bin
out/host/glaze-usb cf-read 8192 1 out/eyefi-vbr-new.bin
```

Use unused output names and finish each USB command before starting the
next. For another card, read its partition table before choosing a VBR LBA.
