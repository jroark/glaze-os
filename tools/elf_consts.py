#!/usr/bin/env python3
"""Print the SPL's -DKERNEL_LOAD_VA / KERNEL_ENTRY_VA / KERNEL_SIZE flags for a kernel ELF."""

import struct
import sys

data = open(sys.argv[1], "rb").read()
e_entry = struct.unpack_from("<I", data, 24)[0]
e_phoff = struct.unpack_from("<I", data, 28)[0]
e_phnum = struct.unpack_from("<H", data, 44)[0]
loads = []
for i in range(e_phnum):
    p_type, _off, p_vaddr, _pa, p_filesz, *_ = struct.unpack_from("<IIIIIIII", data, e_phoff + i * 32)
    if p_type == 1 and p_filesz:
        loads.append((p_vaddr, p_filesz))
base = min(a for a, _ in loads)
end = max(a + s for a, s in loads)
print(f"-DKERNEL_LOAD_VA=0x{base:08X}u -DKERNEL_ENTRY_VA=0x{e_entry:08X}u -DKERNEL_SIZE=0x{end - base:X}u")
