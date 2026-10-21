#!/usr/bin/env python3
"""Asset parser tests contain synthetic data only, never vendor payloads."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from wince_assets import (
    AssetError,
    CbeaArchive,
    CpkDecoder,
    Fat16,
    canonicalize_ce_path,
    encode_gzce_registry,
    lzss,
    parse_cbea_inf,
    span,
)
from import_wince import package, pe_icon, validate_runner, pe_report
from cbea2gzp import convert_cbea


def put32(data, off, value):
    struct.pack_into("<I", data, off, value)


class Assets(unittest.TestCase):
    def test_library_qualified_ordinal_imports(self):
        data = bytearray(1024)
        data[:2] = b"MZ"; put32(data, 60, 128); data[128:132] = b"PE\0\0"
        struct.pack_into("<HH", data, 132, 0x166, 1)
        struct.pack_into("<H", data, 148, 224)
        struct.pack_into("<H", data, 152, 0x10b)
        struct.pack_into("<H", data, 220, 9)
        put32(data, 256, 4096); put32(data, 260, 40)
        struct.pack_into("<IIII", data, 384, 512, 4096, 512, 512)
        put32(data, 512, 4160); put32(data, 524, 4200); put32(data, 528, 4180)
        put32(data, 576, 0x80000001)
        data[616:629] = b"commctrl.dll\0"
        before = bytes(data)
        report = pe_report(data, {1: "CoreOnly"}, {"commctrl.dll": {1: "InitCommonControls"}})
        self.assertEqual(report["imports"][0]["symbols"], [{"ordinal": 1, "name": "InitCommonControls"}])
        self.assertEqual(data, before)
        with self.assertRaises(AssetError):
            pe_report(data, {1: "CoreOnly"})
        data[616:629] = b"COREDLL.dll\0\0"
        report = pe_report(data, {1: "CoreOnly"})
        self.assertEqual(report["imports"][0]["symbols"][0]["name"], "CoreOnly")

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

    def test_pe_icon_extraction(self):
        pe = bytearray(2048)
        pe[:2] = b"MZ"
        put32(pe, 60, 128)
        pe[128:132] = b"PE\0\0"
        struct.pack_into("<HH", pe, 132, 0x166, 1)
        struct.pack_into("<HH", pe, 148, 224, 2)
        struct.pack_into("<H", pe, 152, 0x10b)
        struct.pack_into("<H", pe, 220, 9)
        put32(pe, 264, 0x1000); put32(pe, 268, 512)
        pe[376:382] = b".rsrc\0"
        put32(pe, 384, 512); put32(pe, 388, 0x1000)
        put32(pe, 392, 512); put32(pe, 396, 512)
        rsrc = 512
        # Root dir: 2 entries (type 3 @ +0x20, type 14 @ +0x58)
        struct.pack_into("<H", pe, rsrc + 14, 2)
        put32(pe, rsrc + 16, 3); put32(pe, rsrc + 20, 0x80000020)
        put32(pe, rsrc + 24, 14); put32(pe, rsrc + 28, 0x80000058)
        # Type 3 -> ID 1 -> Lang -> DataEntry @ +0x90
        struct.pack_into("<H", pe, rsrc + 0x20 + 14, 1)
        put32(pe, rsrc + 0x20 + 16, 1); put32(pe, rsrc + 0x20 + 20, 0x80000038)
        struct.pack_into("<H", pe, rsrc + 0x38 + 14, 1)
        put32(pe, rsrc + 0x38 + 16, 1033); put32(pe, rsrc + 0x38 + 20, 0x90)
        # Type 14 -> ID 101 -> Lang -> DataEntry @ +0xa0
        struct.pack_into("<H", pe, rsrc + 0x58 + 14, 1)
        put32(pe, rsrc + 0x58 + 16, 101); put32(pe, rsrc + 0x58 + 20, 0x80000070)
        struct.pack_into("<H", pe, rsrc + 0x70 + 14, 1)
        put32(pe, rsrc + 0x70 + 16, 1033); put32(pe, rsrc + 0x70 + 20, 0xa0)
        # 2x2 32bpp DIB icon at +0xb0 (40 + 16 + 8 = 64 bytes)
        put32(pe, rsrc + 0x90, 0x10b0); put32(pe, rsrc + 0x94, 64)
        struct.pack_into("<IiiHHI", pe, rsrc + 0xb0, 40, 2, 4, 1, 32, 0)
        pe[rsrc + 0xb0 + 40:rsrc + 0xb0 + 56] = b"\x00\x00\xff\xff" * 4
        # GRPICONDIR at +0xf0 (6 + 14 = 20 bytes)
        put32(pe, rsrc + 0xa0, 0x10f0); put32(pe, rsrc + 0xa4, 20)
        struct.pack_into("<HHHBBBBHHIH", pe, rsrc + 0xf0, 0, 1, 1, 2, 2, 0, 0, 1, 32, 64, 1)
        gzi = pe_icon(bytes(pe))
        self.assertEqual(gzi[:12], b"GZI1" + struct.pack("<HHHH", 44, 44, 1, 0))
        self.assertEqual(len(gzi), 12 + 44 * 44 * 3)
        center = struct.unpack_from("<H", gzi, 12 + (22 * 44 + 22) * 2)[0]
        self.assertEqual(center, 0xf800)
        put32(pe, 268, 0)
        with self.assertRaises(AssetError):
            pe_icon(bytes(pe))

    @staticmethod
    def _build_cbea(entries, target="Nand Disk\\Program Files\\TestApp"):
        header = bytearray(0x100)
        struct.pack_into("<H", header, 0, 1)
        struct.pack_into("<I", header, 2, len(entries))
        encoded_target = target.encode("utf-16le") + b"\x00\x00"
        header[8:8 + len(encoded_target)] = encoded_target
        body = bytearray()
        for kind, name, payload in entries:
            raw_name = name.encode("cp1252")
            next_off = 4 + len(raw_name) + 1 + 4 + len(payload)
            body.extend(struct.pack("<I3sB", next_off, kind.encode("ascii"), len(raw_name)))
            body.extend(raw_name + b"\x00")
            body.extend(struct.pack("<I", len(payload)))
            body.extend(payload)
        return bytes(header + body)

    @staticmethod
    def _synthetic_pe(dll=False, base=0x00010000, export_name=None):
        pe = bytearray(1024)
        pe[:2] = b"MZ"
        put32(pe, 60, 128)
        pe[128:132] = b"PE\0\0"
        struct.pack_into("<HH", pe, 132, 0x166, 1)
        struct.pack_into("<HH", pe, 148, 224, 0x2002 if dll else 0x0002)
        struct.pack_into("<H", pe, 152, 0x10b)
        put32(pe, 168, 0x1000)
        put32(pe, 180, base)
        struct.pack_into("<H", pe, 220, 9)
        put32(pe, 224, 0x2000)
        pe[376:382] = b".text\0"
        struct.pack_into("<IIII", pe, 384, 512, 0x1000, 512, 512)
        put32(pe, 412, 0x60000020)
        if export_name:
            put32(pe, 248, 0x1100)
            put32(pe, 252, 128)
            exp = 512 + 0x100
            put32(pe, exp + 16, 1)  # ordinal base = 1
            put32(pe, exp + 20, 1)  # func count = 1
            put32(pe, exp + 24, 1)  # name count = 1
            put32(pe, exp + 28, 0x1140)  # functions RVA
            put32(pe, exp + 32, 0x1144)  # names RVA
            put32(pe, exp + 36, 0x1148)  # ordinals RVA
            put32(pe, 512 + 0x140, 0x1000)
            put32(pe, 512 + 0x144, 0x1150)
            struct.pack_into("<H", pe, 512 + 0x148, 0)
            raw = export_name.encode("ascii") + b"\x00"
            pe[512 + 0x150:512 + 0x150 + len(raw)] = raw
        return bytes(pe)

    def test_cbea_archive_inf_and_conversion(self):
        self.assertEqual(
            canonicalize_ce_path(r"?Drive?\Program Files\AIM\aim.exe", r"\Nand Disk\Program Files\AIM"),
            "/Program Files/AIM/aim.exe",
        )
        self.assertEqual(
            canonicalize_ce_path(r"%INSTALL_DIR%\data\book.pdb", r"\Nand Disk\Program Files\Reader"),
            "/Program Files/Reader/data/book.pdb",
        )
        inf_text = (
            "[App Information]\r\n"
            "Maker=Test Corp.\r\n"
            "Program=Sample Suite\r\n"
            "Caption=Sample Suite\r\n"
            "Version=01.02.03\r\n"
            "InstallDir=\\Nand Disk\\Program Files\\Suite\r\n"
            "ExeFile=%INSTALL_DIR%\\Alpha.exe\r\n"
            "[Make Dirs]\r\n"
            "\"%INSTALL_DIR%\\Data\"\r\n"
            "[Copy Files]\r\n"
            "\"%INSTALL_DIR%\\Alpha.exe\",\"Alpha.exe\",\r\n"
            "\"%INSTALL_DIR%\\Beta.exe\",\"Beta.exe\",\r\n"
            "\"\\Windows\\helper.dll.cpk\",\"helper.dll.cpk\",\r\n"
            "\"%INSTALL_DIR%\\Data\\readme.txt\",\"readme.txt\",\r\n"
            "[Add Registry]\r\n"
            "HKEY_LOCAL_MACHINE,\"Software\\Suite\",\"InstallDir\",REG_SZ,0,\"\\Nand Disk\\Program Files\\Suite\\\\\"\r\n"
            "HKEY_CURRENT_USER,\"Software\\Suite\",\"Mode\",REG_DWORD,0,1a\r\n"
        )
        parsed = parse_cbea_inf(inf_text)
        self.assertEqual(parsed["program"], "Sample Suite")
        self.assertEqual(parsed["registry"][0]["data"], (r"\Nand Disk\Program Files\Suite" + "\\\0").encode("utf-16le"))
        self.assertEqual(parsed["registry"][1]["data"], struct.pack("<I", 0x1A))
        reg_blob = encode_gzce_registry(parsed["registry"])
        self.assertEqual(reg_blob[:4], b"GZCE")
        self.assertIn(b"GZCK", reg_blob)

        exe_a = self._synthetic_pe(dll=False)
        exe_b = self._synthetic_pe(dll=False)
        dll_raw = self._synthetic_pe(dll=True, base=0x40000000, export_name="HelperInit")
        # Build a full uncompressed CPK with 2 blocks (1024 bytes) for dll_raw
        cpk_dll = bytearray(60 + 1024)
        cpk_dll[:12] = b"CASIO.$&%-Z\x01"
        put32(cpk_dll, 44, 1024)
        put32(cpk_dll, 48, 0x01000002)
        put32(cpk_dll, 56, 512)
        cpk_dll[60:] = dll_raw

        archive_bytes = self._build_cbea([
            ("INF", "Install.inf", inf_text.encode("cp1252")),
            ("CAB", "Alpha.exe", exe_a),
            ("CAB", "Beta.exe", exe_b),
            ("CAB", "helper.dll.cpk", bytes(cpk_dll)),
            ("CAB", "readme.txt", b"hello world\n"),
        ])
        archive = CbeaArchive(archive_bytes)
        self.assertEqual(len(archive.entries), 5)

        runner = bytearray(128)
        runner[:7] = b"\x7fELF\x01\x01\x01"
        struct.pack_into("<HH", runner, 16, 3, 8)
        put32(runner, 28, 52)
        struct.pack_into("<HH", runner, 42, 32, 1)
        put32(runner, 52, 3); put32(runner, 56, 84); put32(runner, 68, 19)
        runner[84:103] = b"/lib/ld-glaze.so.1\0"

        packages, report, resolved_files = convert_cbea(
            archive_bytes,
            cbea_name="suite.cbea",
            runner=bytes(runner),
            rom={
                "decoder": self.decoder,
                "ordinals": {},
                "dll_ordinals": {},
                "ordinal_text": b"coredll.dll 0 none\n",
            },
        )
        self.assertEqual(len(resolved_files), 4)
        self.assertEqual(sorted(packages.keys()), ["cbea.alpha.gzp", "cbea.beta.gzp"])
        self.assertEqual([p["id"] for p in report["packages"]], ["cbea.alpha", "cbea.beta"])
        for gzp in packages.values():
            self.assertEqual(gzp[:4], b"GZPK")
            self.assertIn(b"helper.dll /Windows/helper.dll\n", gzp)
            self.assertIn(b"readme.txt /Program Files/Suite/Data/readme.txt\n", gzp)
            self.assertIn(b"GZCE", gzp)


if __name__ == "__main__":
    unittest.main()
