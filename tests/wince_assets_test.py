#!/usr/bin/env python3
"""Asset parser tests contain synthetic data only, never vendor payloads."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from wince_assets import AssetError, CpkDecoder, Fat16, lzss, span
from import_wince import package, validate_runner


def put32(data, off, value):
    struct.pack_into("<I", data, off, value)


class Assets(unittest.TestCase):
    def setUp(self):
        driver = bytearray(1024)
        table = 4
        put32(driver, table, 0xffff)
        struct.pack_into("<7i", driver, table + 4, 0, 0, 0, 0, 0, -2, -7)
        put32(driver, table + 0x84 + 4, 2)
        struct.pack_into("<2H", driver, table + 0x104, 0x1234, 0xffff)
        self.decoder = CpkDecoder(bytes(driver))

    def test_span_and_lzss(self):
        for off, size in ((-1, 1), (0, 2), (2, 0), (1, 1)):
            with self.assertRaises(AssetError):
                span(b"a", off, size)
        self.assertEqual(lzss(b"\x07abc", 3), b"abc")
        self.assertEqual(lzss(b"\x00\x00\x00", 3), b"\0" * 3)
        with self.assertRaises(AssetError):
            lzss(b"\x00\x00", 3)
        with self.assertRaises(AssetError):
            lzss(b"\x00\x00\x0f", 3)

    def test_run_and_dictionary_modes(self):
        self.assertEqual(self.decoder.block(b"\x01\x01\x03\x02", 1, 6), b"\x01" * 5 + b"\x02")
        self.assertEqual(self.decoder.block(b"\xaa\xb2\x0c", 4, 3), b"\xaa\xaa\xcb")
        self.assertEqual(self.decoder.block(b"\x11\x0a\x22\x33", 9, 4), b"\x11\x22\x11\x33")
        self.assertEqual(self.decoder.block(b"\xa5\x21", 13, 2), b"\x15\x25")
        for kind in range(1, 17):
            with self.assertRaises(AssetError):
                self.decoder.block(b"", kind, 512)

    def test_huffman_and_container(self):
        bits = "01" + f"{0xabcd:016b}"  # dictionary word, then escaped literal
        packed = int(bits.ljust(24, "0"), 2).to_bytes(3, "big")
        self.assertEqual(self.decoder.block(packed, 15, 4), b"\x34\x12\xcd\xab")
        data = bytearray(56 + 512)
        data[:12] = b"CASIO.$&%-Z\x01"
        put32(data, 44, 512); put32(data, 48, 0x01000001)
        data[56:] = bytes(range(256)) * 2
        self.assertEqual(self.decoder.unpack(data), data[56:])
        put32(data, 52, 0xffffff)
        with self.assertRaises(AssetError):
            self.decoder.unpack(data)
        with self.assertRaises(AssetError):
            CpkDecoder(b"not a filesystem driver")

    def fat_fixture(self):
        image = bytearray(12 * 512)
        put32(image, 56, 1); put32(image, 60, 11)
        boot = memoryview(image)[512:1024]
        struct.pack_into("<H", boot, 11, 512); boot[13] = 1
        struct.pack_into("<H", boot, 14, 1); boot[16] = 1
        struct.pack_into("<H", boot, 17, 16); struct.pack_into("<H", boot, 19, 11)
        struct.pack_into("<H", boot, 22, 1); boot[54:62] = b"FAT16   "; boot[510:512] = b"\x55\xaa"
        struct.pack_into("<3H", image, 1024, 0xfff8, 0xffff, 0xffff)
        root = 1536
        image[root:root + 11] = b"CALC    EXE"
        struct.pack_into("<H", image, root + 26, 2); put32(image, root + 28, 3)
        image[2048:2051] = b"MZ!"
        return image

    def test_fat_readonly_and_bad_chains(self):
        image = self.fat_fixture(); before = bytes(image)
        self.assertEqual(Fat16(image).read("calc.exe"), b"MZ!")
        self.assertEqual(bytes(image), before)
        with self.assertRaises(AssetError):
            Fat16(image).read("absent.exe")
        struct.pack_into("<H", image, 1028, 2)
        with self.assertRaises(AssetError):
            Fat16(image).read("calc.exe")
        image[1022] = 0
        with self.assertRaises(AssetError):
            Fat16(image)

    def test_package_layout(self):
        data = package({"res/program.exe": b"MZ!", "Info": b"id=test\n"})
        self.assertEqual(data[:4], b"GZPK")
        self.assertEqual(struct.unpack_from("<I", data, 4)[0], 2)
        for i in range(2):
            name, off, size = struct.unpack_from("<48sII", data, 8 + i * 56)
            self.assertEqual(off % 4, 0)
            self.assertEqual(data[off:off + size], b"id=test\n" if name.startswith(b"Info") else b"MZ!")

    def test_installed_runner_requires_glaze_interpreter(self):
        elf = bytearray(128)
        elf[:7] = b"\x7fELF\x01\x01\x01"
        struct.pack_into("<HH", elf, 16, 3, 8)
        put32(elf, 28, 52)
        struct.pack_into("<HH", elf, 42, 32, 1)
        put32(elf, 52, 3); put32(elf, 56, 84); put32(elf, 68, 19)
        elf[84:103] = b"/lib/ld-glaze.so.1\0"
        validate_runner(elf)
        struct.pack_into("<H", elf, 16, 2)
        with self.assertRaises(AssetError):
            validate_runner(elf)
        struct.pack_into("<H", elf, 16, 3)
        elf[89] = ord("x")
        with self.assertRaises(AssetError):
            validate_runner(elf)


if __name__ == "__main__":
    unittest.main()
