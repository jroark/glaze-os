#!/usr/bin/env python3
"""Compare the actual packaged ELF bytes and page padding in romfs images."""
import argparse
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import romfs  # noqa: E402  (tools/romfs.py)


def measure(path):
    data = path.read_bytes()
    files = romfs.entries(data)     # decompressed, for an LZ4 archive
    result = dict(romfs_bytes=len(data), binaries=0, binary_bytes=0,
                  libraries=0, library_bytes=0, elf_load_bytes=0,
                  elf_page_padding=0)
    for name, elf in sorted(files.items()):
        size = len(elf)
        if not name.startswith(("bin/", "lib/")):
            continue
        group = "binary" if name.startswith("bin/") else "library"
        result["binaries" if group == "binary" else "libraries"] += 1
        result[group + "_bytes"] += size
        result["elf_page_padding"] += -size % 4096
        if elf[:7] != b"\x7fELF\x01\x01\x01":
            raise ValueError(f"{path}: {name}: not ELF32 little endian")
        phoff, = struct.unpack_from("<I", elf, 28)
        phsize, phnum = struct.unpack_from("<HH", elf, 42)
        for j in range(phnum):
            kind, _, _, _, filesz = struct.unpack_from("<IIIII", elf, phoff + phsize * j)
            if kind == 1:
                result["elf_load_bytes"] += filesz
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("romfs", type=Path, nargs="+")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    results = {str(p): measure(p) for p in args.romfs}
    if args.json:
        print(json.dumps(results, indent=2))
        return
    for path, r in results.items():
        print(path)
        print(f"  romfs: {r['romfs_bytes'] // 1024} KiB")
        print(f"  {r['binaries']} binaries: {r['binary_bytes']} bytes; "
              f"{r['libraries']} libraries: {r['library_bytes']} bytes")
        print(f"  ELF PT_LOAD file bytes: {r['elf_load_bytes']}; "
              f"archive ELF page padding: {r['elf_page_padding']}")


if __name__ == "__main__":
    main()
