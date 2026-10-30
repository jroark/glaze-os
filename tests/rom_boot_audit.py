#!/usr/bin/env python3
"""Audit tooling must select MIPS16 and must never publish stream payloads."""
import importlib.util
import struct
import unittest
from pathlib import Path
spec = importlib.util.spec_from_file_location('audit', Path(__file__).parents[1] / 'tools/rom_boot_audit.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class RomAudit(unittest.TestCase):
    def test_mips16_symbol_and_addresses(self):
        rom = bytes(i & 255 for i in range(0x4000))
        elf = audit.mips16_elf(rom)
        shoff = struct.unpack_from('<I', elf, 32)[0]
        text = struct.unpack_from('<10I', elf, shoff + 40)
        symbols = struct.unpack_from('<10I', elf, shoff + 80)
        symbol = struct.unpack_from('<IIIBBH', elf, symbols[4] + 16)
        self.assertEqual(text[3], 0x9FC00C20)
        self.assertEqual(elf[text[4]:text[4]+text[5]], rom[0xC20:])
        self.assertEqual(symbol[4:], (0xF0, 1))
        with self.assertRaises(ValueError):
            audit.mips16_elf(b'bad')

    def test_handoff_and_payload_redaction(self):
        lines = [
            '[MMIO] pc=9FC01D8E wr be300_vrc4173 @1134 len=4 <= 0x00000001',
            '[MMIO] pc=BFC01A98 rd be300_nand @B000 len=4 => 0x12345678',
            '[MMIO] pc=9FC01E14 rd be300_nand @A430 len=4 => 0x00000000',
            '[MMIO] pc=A0F00224 wr be300_vrc4173 @1134 len=4 <= 0x00000001',
            '[MMIO] pc=9FC01D8E wr be300_vrc4173 @1134 len=4 <= 0x00000000',
        ]
        report = audit.summarize(lines)
        self.assertEqual(report['first_spl_mmio_pc'], '0xa0f00224')
        self.assertEqual(len(report['accesses']), 3)
        self.assertEqual(report['companion_last_writes']['be300_vrc4173:0x1134'], '0x00000001')
        self.assertNotIn('12345678', str(report))
        self.assertNotIn('last_value', report['accesses'][1])
        self.assertIsNone(audit.summarize(lines[:3])['first_spl_mmio_pc'])


if __name__ == '__main__':
    unittest.main()
