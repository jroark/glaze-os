#!/usr/bin/env python3
"""Inspect local BE-300 ROM artifacts without changing an emulator or device.

--mips16-elf selects GNU objdump's MIPS16 decoder using a STO_MIPS16 symbol.
--mmio summarizes companion accesses before the first Glaze SPL MMIO access.
Payload/stream data is deliberately excluded from the JSON report.
"""
import argparse
import collections
import hashlib
import json
import re
import struct
from pathlib import Path


def mips16_elf(rom: bytes) -> bytes:
    if len(rom) != 0x4000:
        raise ValueError('expected the 16 KiB masked ROM dump')
    code = rom[0xC20:]
    names = b'\0.text\0.symtab\0.strtab\0.shstrtab\0'
    strings = b'\0rom_mips16\0'
    textoff = 52
    symoff = (textoff + len(code) + 3) & ~3
    stroff = symoff + 32
    shstroff = stroff + len(strings)
    shoff = (shstroff + len(names) + 3) & ~3
    out = bytearray(shoff + 5 * 40)
    struct.pack_into('<16sHHIIIIIHHHHHH', out, 0,
                     b'\x7fELF\x01\x01\x01' + bytes(9), 1, 8, 1, 0, 0,
                     shoff, 0x20000000, 52, 0, 0, 40, 5, 4)
    out[textoff:textoff + len(code)] = code
    struct.pack_into('<IIIBBH', out, symoff + 16, 1, 0, len(code), 0x12, 0xF0, 1)
    out[stroff:stroff + len(strings)] = strings
    out[shstroff:shstroff + len(names)] = names
    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (1, 1, 6, 0x9FC00C20, textoff, len(code), 0, 0, 2, 0),
        (7, 2, 0, 0, symoff, 32, 3, 1, 4, 16),
        (15, 3, 0, 0, stroff, len(strings), 0, 0, 1, 0),
        (23, 3, 0, 0, shstroff, len(names), 0, 0, 1, 0),
    ]
    for i, values in enumerate(sections):
        struct.pack_into('<10I', out, shoff + i * 40, *values)
    return bytes(out)


def summarize(lines):
    pattern = re.compile(r'\[MMIO\] pc=([0-9A-Fa-f]+) (wr|rd) (\w+) '
                         r'@([0-9A-Fa-f]+) len=(\d+) (?:<=|=>) 0x([0-9A-Fa-f]+)')
    events = collections.OrderedDict()
    final = {}
    handoff = None
    for line in lines:
        match = pattern.search(line)
        if not match:
            continue
        pc, op, device, off, width, value = match.groups()
        pc, off, width, value = int(pc, 16), int(off, 16), int(width), int(value, 16)
        physical_pc = pc & 0x1FFFFFFF
        if 0xF00000 <= physical_pc < 0x1000000:
            handoff = f'0x{pc:08x}'
            break
        if not 0x1FC00000 <= physical_pc < 0x1FC04000:
            continue
        # Keep counts for NAND stream/addresses/ECC, never their payloads.
        key = (pc, op, device, off, width)
        if key not in events:
            events[key] = {'pc': f'0x{pc:08x}', 'op': op, 'device': device,
                           'offset': f'0x{off:04x}', 'width': width, 'count': 0}
        events[key]['count'] += 1
        if op == 'wr' and off not in (0xB000, 0xA420, 0xA468):
            events[key]['last_value'] = f'0x{value:08x}'
            final[f'{device}:0x{off:04x}'] = f'0x{value:08x}'
    return {'first_spl_mmio_pc': handoff, 'companion_last_writes': final,
            'accesses': list(events.values()),
            'limit': 'Companion MMIO only; does not establish complete VR4131 reset state.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path)
    parser.add_argument('--mips16-elf', type=Path)
    parser.add_argument('--mmio', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    if not args.rom and not args.mmio:
        parser.error('supply --rom and/or --mmio')
    if args.mips16_elf and not args.rom:
        parser.error('--mips16-elf requires --rom')
    report = {}
    if args.rom:
        rom = args.rom.read_bytes()
        if len(rom) != 0x4000:
            parser.error('expected a 16 KiB ROM')
        report['rom_sha256'] = hashlib.sha256(rom).hexdigest()
        if args.mips16_elf:
            args.mips16_elf.write_bytes(mips16_elf(rom))
    if args.mmio:
        with args.mmio.open() as source:
            report.update(summarize(source))
        if report['first_spl_mmio_pc'] is None:
            parser.error('trace never reaches the SPL; handoff report is incomplete')
    text = json.dumps(report, indent=2) + '\n'
    if args.out:
        args.out.write_text(text)
    else:
        print(text, end='')


if __name__ == '__main__':
    main()
