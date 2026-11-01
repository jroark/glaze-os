#!/usr/bin/env python3
"""Print the SPL's -DKERNEL_LOAD_VA / KERNEL_ENTRY_VA / KERNEL_SIZE flags for a kernel ELF,
and KERNEL_TEXT_LEN / KERNEL_TEXT_SUM: its text and read-only data up to __ro_end, and
their gz_resume_sum (kernel/resume.h), which the SPL checks before resuming a HIBERNATE."""

import struct
import sys

data = open(sys.argv[1], "rb").read()
e_entry = struct.unpack_from("<I", data, 24)[0]
e_phoff = struct.unpack_from("<I", data, 28)[0]
e_phnum = struct.unpack_from("<H", data, 44)[0]
loads = []
for i in range(e_phnum):
    p_type, p_off, p_vaddr, _pa, p_filesz, *_ = struct.unpack_from("<IIIIIIII", data, e_phoff + i * 32)
    if p_type == 1 and p_filesz:
        loads.append((p_vaddr, p_off, p_filesz))
base = min(a for a, _, _ in loads)
end = max(a + s for a, _, s in loads)

# The flat image the SPL loads, as tools/mkkernelbin.py lays it out.
image = bytearray(end - base)
for vaddr, off, size in loads:
    image[vaddr - base:vaddr - base + size] = data[off:off + size]

# __ro_end from the symbol table.
e_shoff = struct.unpack_from("<I", data, 32)[0]
e_shentsize, e_shnum = struct.unpack_from("<HH", data, 46)
sections = [struct.unpack_from("<IIIIIIIIII", data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
ro_end = None
for sh in sections:
    if sh[1] != 2:      # SHT_SYMTAB
        continue
    strtab = sections[sh[6]]
    for k in range(sh[5] // 16):
        st_name, st_value = struct.unpack_from("<II", data, sh[4] + k * 16)
        name_at = strtab[4] + st_name
        name = data[name_at:data.index(b"\0", name_at)]
        if name == b"__ro_end":
            ro_end = st_value
if ro_end is None or not base < ro_end <= end:
    sys.exit("elf_consts.py: no __ro_end in the kernel")
text_len = (ro_end - base) & ~3
total = 0
for (word,) in struct.iter_unpack("<I", bytes(image[:text_len])):
    total = (((total << 1) | (total >> 31)) + word) & 0xFFFFFFFF

print(f"-DKERNEL_LOAD_VA=0x{base:08X}u -DKERNEL_ENTRY_VA=0x{e_entry:08X}u -DKERNEL_SIZE=0x{end - base:X}u "
      f"-DKERNEL_TEXT_LEN=0x{text_len:X}u -DKERNEL_TEXT_SUM=0x{total:08X}u")
