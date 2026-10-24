#!/usr/bin/env python3
"""Write the flat kernel file GlazeBoot loads (struct gz_kernel_bin in
lib/include/glaze/bootinfo.h): a 4 KiB header page, then the image from the
lowest PT_LOAD address for its file size, as the SPL copies it.

usage: tools/mkkernelbin.py kernel.elf kernel.bin
"""

import struct
import sys

MAGIC = 0x424B5A47      # "GZKB"
HEADER = 4096
PAGE = 4096

data = open(sys.argv[1], "rb").read()
e_entry = struct.unpack_from("<I", data, 24)[0]
e_phoff = struct.unpack_from("<I", data, 28)[0]
e_phnum = struct.unpack_from("<H", data, 44)[0]
loads = []
for i in range(e_phnum):
    p_type, p_off, p_vaddr, _pa, p_filesz, p_memsz, *_ = struct.unpack_from("<IIIIIIII", data, e_phoff + i * 32)
    if p_type == 1 and p_memsz:
        loads.append((p_vaddr, p_off, p_filesz, p_memsz))
base = min(v for v, _, f, _ in loads if f)
size = max(v + f for v, _, f, _ in loads if f) - base
end = (max(v + m for v, _, _, m in loads) + PAGE - 1) & ~(PAGE - 1)

image = bytearray(size)
for vaddr, off, filesz, _ in loads:
    if filesz:
        image[vaddr - base:vaddr - base + filesz] = data[off:off + filesz]

header = struct.pack("<8I", MAGIC, base, e_entry, size, end, 0, 0, 0).ljust(HEADER, b"\0")
with open(sys.argv[2], "wb") as f:
    f.write(header + image)
