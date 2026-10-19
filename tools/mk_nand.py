#!/usr/bin/env python3
"""Pack a BE-300 stage-1 SPL, flat kernel image, and optional rootfs as a
NAND boot dump.

Layout (matches WinCE's partition table convention; see
build-host/All_nand_300_repacked.bin in the emulator source tree for the
reference image):

  NAND[0x000..0x200)   page 0 = 16-byte/entry partition table
                         entry 0  : (start_sector=0,        count=0x20)
                         entry 1  : (start_sector=0x20,     count=<spl sectors>)
                         entry 2  : (start_sector=0xA0,     count=<kernel sectors>)
                         entry 3  : (start_sector=0x2800,   count=<rootfs sectors>)
                                    (only present when --rootfs is supplied)
                         entries 4..7: 0xFFFFFFFF (unused)
  NAND[0x4000..)       partition 1 = SPL B000FF container
                         "B000FF\\n", image_start, image_length, records,
                         terminator (0, kernel_entry_via_SPL, 0xFFFFFFFF)
  NAND[0x14000..)      partition 2 = flat kernel binary (objcopy -O binary)
  NAND[0x500000..)     partition 3 = rootfs blob (e.g. JFFS2)
  NAND[0x10000000-1]   padded with 0xFF to 16 MiB

The boot ROM walks partition 1 (the SPL); the SPL then reads partition 2 a
page at a time via the VRC4173 SPL transfer engine and jumps to the kernel
entry. The rootfs is read by Linux's MTD/JFFS2 subsystem via
board/casio-be300/nand.c (mtd3 = "rootfs"). See board/casio-be300/spl.c for
the SPL.
"""

import argparse
import struct
import sys
from pathlib import Path

NAND_IMAGE_SIZE = 16 * 1024 * 1024
SPL_OFFSET = 0x4000
KERNEL_OFFSET = 0x14000
ROOTFS_OFFSET = 0x500000
B000FF_SIG = b"B000FF\n"
NAND_FILL = 0xFF
RECORD_CKSUM = 0xFFFFFFFF
PARTITION_ENTRY_SIZE = 16
DEFAULT_CHUNK = 0x8000

ELF_MAGIC = b"\x7fELF"
ELFCLASS32 = 1
ELFDATA2LSB = 1
EM_MIPS = 8
PT_LOAD = 1


def parse_elf32_le(data: bytes):
    if len(data) < 52 or data[:4] != ELF_MAGIC:
        raise ValueError("not an ELF file")
    if data[4] != ELFCLASS32 or data[5] != ELFDATA2LSB:
        raise ValueError("must be ELF32 little-endian")
    e_machine = struct.unpack_from("<H", data, 18)[0]
    if e_machine != EM_MIPS:
        raise ValueError(f"e_machine={e_machine}, expected MIPS ({EM_MIPS})")
    e_entry = struct.unpack_from("<I", data, 24)[0]
    e_phoff = struct.unpack_from("<I", data, 28)[0]
    e_phentsize = struct.unpack_from("<H", data, 42)[0]
    e_phnum = struct.unpack_from("<H", data, 44)[0]
    if e_phentsize != 32:
        raise ValueError(f"unexpected e_phentsize={e_phentsize}")

    loads = []
    for i in range(e_phnum):
        off = e_phoff + i * 32
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = (
            struct.unpack_from("<IIIIIIII", data, off)
        )
        if p_type != PT_LOAD or p_filesz == 0:
            continue
        loads.append((p_vaddr, p_filesz, p_memsz, p_offset))
    if not loads:
        raise ValueError("no PT_LOAD segments with file data")
    return e_entry, loads


def is_kseg(addr: int) -> bool:
    top = addr & 0xE0000000
    return top in (0x80000000, 0xA0000000)


def build_b000ff(elf_data: bytes, chunk: int) -> tuple[bytes, dict]:
    """Pack an ELF as a B000FF container. Returns (container_bytes, metadata)."""
    if chunk <= 0:
        raise ValueError("chunk must be positive")
    e_entry, loads = parse_elf32_le(elf_data)
    if not is_kseg(e_entry):
        raise ValueError(f"e_entry 0x{e_entry:08X} is not KSEG")

    image_start = min(addr for addr, _, _, _ in loads)
    if not is_kseg(image_start):
        raise ValueError(f"lowest segment 0x{image_start:08X} is not KSEG")

    records = bytearray()
    n_records = 0
    for addr, filesz, _memsz, offset in loads:
        if not is_kseg(addr):
            raise ValueError(f"PT_LOAD vaddr 0x{addr:08X} not KSEG")
        pos = 0
        while pos < filesz:
            take = min(chunk, filesz - pos)
            records += struct.pack("<III", addr + pos, take, RECORD_CKSUM)
            records += elf_data[offset + pos:offset + pos + take]
            pos += take
            n_records += 1

    # Terminator framing matches the WinCE SPL container byte layout: a
    # 4-byte zero sentinel followed by a 12-byte (entry, 0, 0xFFFFFFFF)
    # record. The boot ROM record walker (`FUN_9fc00dec`) reads 12 bytes
    # at the sentinel position, sees `addr=0`, and uses `len` as the entry.
    records += b"\x00\x00\x00\x00"
    records += struct.pack("<III", e_entry, 0, RECORD_CKSUM)

    container = bytearray()
    container += B000FF_SIG
    container += struct.pack("<II", image_start, len(records))
    container += records

    meta = {
        "e_entry": e_entry,
        "image_start": image_start,
        "image_length": len(records),
        "records": n_records,
        "loads": loads,
    }
    return bytes(container), meta


def kernel_flat_bytes(kernel_data: bytes) -> tuple[bytes, dict]:
    """Concatenate ELF PT_LOAD segments into a flat image starting at the
    lowest segment vaddr. Used as partition 2's payload (read by the SPL)."""
    e_entry, loads = parse_elf32_le(kernel_data)
    base = min(addr for addr, _, _, _ in loads)
    end = max(addr + filesz for addr, filesz, _, _ in loads)
    flat = bytearray(end - base)
    for vaddr, filesz, _memsz, offset in loads:
        rel = vaddr - base
        flat[rel:rel + filesz] = kernel_data[offset:offset + filesz]
    return bytes(flat), {"load_va": base, "size": end - base, "entry_va": e_entry}


def write_partition_table(image: bytearray, entries: list[tuple[int, int]]):
    """Lay out a 16-byte-per-entry partition table at NAND page 0.
    `entries` is a list of (start_sector, sector_count). Up to 8 entries."""
    pt = bytearray([NAND_FILL]) * 512
    for i, (start, count) in enumerate(entries):
        struct.pack_into("<II", pt, i * PARTITION_ENTRY_SIZE + 8, start, count)
    image[0:512] = pt


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--kernel", required=True, help="Path to kernel ELF (used for the kernel partition)")
    ap.add_argument("--spl", required=True, help="Path to spl.elf (stage-1 SPL ELF)")
    ap.add_argument("--rootfs", help="Optional path to rootfs blob (e.g. rootfs.jffs2) to place at NAND offset 0x{:X}".format(ROOTFS_OFFSET))
    ap.add_argument("--out", required=True, help="Output NAND image path")
    ap.add_argument("--chunk", type=lambda s: int(s, 0), default=DEFAULT_CHUNK,
                    help=f"Max bytes per SPL B000FF record (default: 0x{DEFAULT_CHUNK:X})")
    args = ap.parse_args()

    spl_elf = Path(args.spl).read_bytes()
    vmlinux = Path(args.kernel).read_bytes()
    rootfs = Path(args.rootfs).read_bytes() if args.rootfs else None

    # Build the SPL B000FF container.
    spl_container, spl_meta = build_b000ff(spl_elf, args.chunk)
    if SPL_OFFSET + len(spl_container) > KERNEL_OFFSET:
        raise SystemExit(
            f"SPL container ({len(spl_container)} bytes) overflows the SPL "
            f"partition slot (0x{KERNEL_OFFSET - SPL_OFFSET:X} bytes)"
        )

    # Build the kernel flat image (partition 2 payload, read raw by the SPL).
    kernel_flat, kernel_meta = kernel_flat_bytes(vmlinux)

    spl_sectors = (len(spl_container) + 511) // 512
    kernel_sectors = (len(kernel_flat) + 511) // 512

    if KERNEL_OFFSET + kernel_sectors * 512 > ROOTFS_OFFSET:
        raise SystemExit(
            f"kernel ({len(kernel_flat)} bytes) overflows the kernel partition; "
            f"rootfs starts at 0x{ROOTFS_OFFSET:X}"
        )

    image = bytearray([NAND_FILL]) * NAND_IMAGE_SIZE
    image[SPL_OFFSET:SPL_OFFSET + len(spl_container)] = spl_container
    image[KERNEL_OFFSET:KERNEL_OFFSET + len(kernel_flat)] = kernel_flat

    partitions = [
        (0,                          0x20),                  # entry 0: PT region
        (SPL_OFFSET // 512,          spl_sectors),            # entry 1: SPL
        (KERNEL_OFFSET // 512,       kernel_sectors),         # entry 2: kernel
    ]

    if rootfs is not None:
        if ROOTFS_OFFSET + len(rootfs) > NAND_IMAGE_SIZE:
            raise SystemExit(
                f"rootfs ({len(rootfs)} bytes) overflows NAND at offset "
                f"0x{ROOTFS_OFFSET:X}"
            )
        image[ROOTFS_OFFSET:ROOTFS_OFFSET + len(rootfs)] = rootfs
        rootfs_sectors = (len(rootfs) + 511) // 512
        partitions.append((ROOTFS_OFFSET // 512, rootfs_sectors))

    write_partition_table(image, partitions)

    Path(args.out).write_bytes(image)

    print(
        f"[mk_be300_nand] {args.out}\n"
        f"  SPL records  = {spl_meta['records']} (+ terminator)\n"
        f"  SPL entry    = 0x{spl_meta['e_entry']:08X}\n"
        f"  SPL image    = 0x{spl_meta['image_start']:08X} + 0x{spl_meta['image_length']:X} bytes\n"
        f"  SPL slot     = sector 0x{SPL_OFFSET // 512:X} count 0x{spl_sectors:X}\n"
        f"  Kernel load  = 0x{kernel_meta['load_va']:08X}\n"
        f"  Kernel entry = 0x{kernel_meta['entry_va']:08X}\n"
        f"  Kernel flat  = 0x{kernel_meta['size']:X} bytes\n"
        f"  Kernel slot  = sector 0x{KERNEL_OFFSET // 512:X} count 0x{kernel_sectors:X}"
    )
    if rootfs is not None:
        print(
            f"  Rootfs slot  = sector 0x{ROOTFS_OFFSET // 512:X} count 0x{rootfs_sectors:X}\n"
            f"  Rootfs size  = 0x{len(rootfs):X} bytes"
        )
    print(f"  NAND size    = 0x{NAND_IMAGE_SIZE:X} ({NAND_IMAGE_SIZE >> 20} MiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
