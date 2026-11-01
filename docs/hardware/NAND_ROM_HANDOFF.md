# Masked ROM handoff and protected boot content

Follow-up to [NAND_BOOT_AUDIT.md](NAND_BOOT_AUDIT.md), 2026-10-29.
No physical NAND writes were performed. Addresses below refer to the stock ROM.

## Reproducing the ROM evidence

The local 16 KiB ROM has SHA-256
`dcef0dc4b672596a4021a4e005d149835761a5906634028e7f5a4f008f31c9f3`.
Its mixed ISA needs a MIPS16 function symbol: decoding the whole dump as MIPS32
produces misleading instructions. This command builds a local ELF wrapper; it
never modifies or distributes the ROM:

```sh
python3 tools/rom_boot_audit.py \
  --rom ../be300/docs/hardware/be300_boot_rom.bin \
  --mips16-elf out/nand-blockers/rom/mips16.elf
# In the cross-toolchain container:
mipsel-linux-gnu-objdump -d out/nand-blockers/rom/mips16.elf
```

Only reachable instructions should be interpreted as code: literal pools in the
same section are also printed as instructions. Raw MIPS32 routines below 0xC20
were separately decoded as little-endian MIPS32.

A disposable Glaze NAND image booted with `--log-mmio --mmio-coverage` provided a
companion MMIO trace from ROM reset through SPL entry. Summarize it with:

```sh
python3 tools/rom_boot_audit.py \
  --rom ../be300/docs/hardware/be300_boot_rom.bin \
  --mmio out/nand-blockers/rom/mmio.log \
  --out out/nand-blockers/rom/handoff.json
```

The summarizer stops at the first SPL MMIO access, counts ROM accesses, and
omits stream payloads and ECC input values. Missing SPL handoff is an error.
This trace does **not** capture complete VR4131 register values or prove silicon
reset defaults; it corroborates execution of the raw ROM instructions.

## Confirmed loader contract

| ROM routine / block | Confirmed behavior |
| --- | --- |
| `9FC00C20..C7A` | Selects a boot callback table, calls the B000FF parser, then passes the returned entry to mailbox helper `9FC00464`. This connects the MIPS16 loader to the MIPS32 mailbox/jump flow. |
| `9FC00D7C..DE4` | Opens the stream, reads/compares the seven-byte signature, reads the eight-byte image header, then invokes record walker `9FC00DEC`. |
| `9FC00DFA..E1C` | Reads 12-byte record headers. A nonzero address and length invokes the stream-copy callback with destination ORed with `20000000` (uncached alias). The word at header +8 is not validated. |
| `9FC00E1E..E26` | An address-zero record returns its second word as the entry. The terminator consumed is `(0, entry, 0)`; the trailing `FFFFFFFF` emitted by the packer is unused. |
| `9FC01758..17C4` | Reads tags from the first five pages of a block: spare prefix `AA 55`, status byte `0F`, and logical ID at spare +4. Uses three agreeing IDs as the quorum. |
| `9FC017C6..1816` | Physical/logical mismatch handling includes the reserve range starting at logical block 1004, bounded by 1024. Glaze's reader builds an explicit map instead of copying the ROM's search heuristic. |
| `9FC01A90..1AB8` | Reads 130 stream words (512 data +8 spare), selects MODE 4, then reads two more words. It consumes all 16 spare bytes. |

The loader does not provide a verified application checksum through the B000FF
record checksum field. Protection must come from the NAND ECC and, for a future
flashing tool, validation of the source image and every programmed page.

## Initialization supplied by the ROM

These settings previously required an ownership check. The raw instructions and
ROM-only trace establish ROM ownership on the observed NAND boot path:

| Physical register(s) | ROM PC(s) | Writes before SPL |
| --- | --- | --- |
| `0A00111C`, socket timing `0A00A010/A014/A018` | `9FC008B0`, `8C0/8C8/8D0` | `1`, then `1073/1061/10B1`, respectively |
| `0A001120`, `0A001B24/1B20` | `9FC01D9E..1DA6` | `1`, then `0/1` |
| `0A001134`, `0A00A400/A408/A40C` | `9FC01D8E..1D96` | `1` to NAND clock and all three PIO enables |
| `0A001104`, `0A00A0DC` | `9FC01DAC/1DB4` | `1`, then `FF` |
| `0A007804/7800` | `9FC00D2E..D40` | Both zeroed, then `35/5C`; polls the MCU status at `7834` afterwards |

At the observed handoff, NAND CTRL and phase are zero, MODE is 1, and the unit
clock/PIO enable words remain 1. Glaze's explicit selection/reset is still
appropriate: a reader should not rely on transaction state left by the loader.
The new cold socket routine also retains explicit setup and resets/configures
cards; these timing writes are no longer evidence of a routine absent from ROM.
The protocol's full power semantics and UART electrical enable remain open.

## Glaze boot-reader changes and validation

`kernel/drivers/nand_boot.c` is shared by the SPL and NAND-backed romfs. It scans
physical spare tags, requires a three-page quorum for each logical mapping,
rejects duplicate mappings, and never substitutes a physical page for a missing
logical page. Its namespace covers immutable content below logical offset
`0x500000`; physical placement can include replacement blocks elsewhere.
The writable filesystem retains its separate physical arena and FTL.

Pages with parity are checked by the Reed-Solomon decoder before returning data.
The decoder now verifies all six residual syndromes before mutating a page and
repairs parity-symbol errors as well as data/tag errors. It rejects the previous
false-clean case where the first four syndromes are zero but later ones are not.
Uncorrectable data never reaches the destination buffer.

Data-only emulator images still need their synthetic tag/no-parity path. Once
any real parity is observed during scanning, selected block-tag quorums and
subsequent boot-content pages must be ECC protected. Mixed protected/unprotected
content is rejected. An entirely unprotected data-only image remains compatible
with the existing emulator and must not be treated as a physical flash artifact.

Validation:

- `make -C host nand-ecc-test nand-boot-test boot-init-test`: ASan/UBSan tests
  cover data/tag/parity repair, triple-error rejection, the false-clean syndrome,
  relocated blocks, tag quorum, duplicates, missing blocks and failed reads.
- `python3 tests/rom_boot_audit.py`: validates the ELF's MIPS16 symbol and address
  layout, trace handoff boundary and stream-payload redaction.
- Normal emulator NAND boot, NAND boot with CF, and the scattered RAM-boot test
  reach SpringBoard using the shared mapped reader. The CF variant mounts its
  card. Images/logs are under ignored `out/nand-blockers`.
- `./build.sh glazeboot NAND_BOOT_PROBE=1` builds a read-only hardware diagnostic.
  It runs after RAM handoff, requires readable NAND, scans tags with mandatory
  parity, and prints only mapping/partition metadata. The physical BE-300
  reported 320/320 boot logical mappings, zero corrected symbols, and a 12,422 ms
  scan. Logical blocks 0/1/10/16 mapped to physical 0/1/10/16. It read the protected
  partition page successfully and subsequently mounted CF and read-only CE NAND.

## Physical-image follow-up (2026-10-30)

The read-only USB backup, conservative CE inventory, physical image encoder,
reserved replacement arena and filesystem bad-block import are implemented.
The sibling emulator now preserves real OOB and models the ROM's hardware ECC
remainder interface. Synthetic remapped SPL/kernel/romfs fault tests pass
through the stock ROM; the image made from this device's verified backup also
boots. A standalone CF recovery image boots with blank NAND in the emulator.

See [physical NAND artifacts and recovery](PHYSICAL_NAND.md) for the format,
reproduction commands, hardware evidence and remaining gates. Physical recovery
boot and a separately reviewed raw programming/restore path remain outstanding.
No raw NAND program or erase operation has been performed on the device.
