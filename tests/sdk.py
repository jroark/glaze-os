#!/usr/bin/env python3
"""Integration tests against a relocated SDK, with real cross-built apps."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest


class SDKTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="glaze-sdk-test-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.root = Path(cls.tmp.name)
        cls.sdk = cls.root / "relocated-sdk"
        shutil.copytree(ARGS.sdk, cls.sdk)
        for line in (cls.sdk / "SHA256SUMS").read_text().splitlines():
            digest, name = line.split("  ", 1)
            assert hashlib.sha256((cls.sdk / name).read_bytes()).hexdigest() == digest, name
        # No suffix on the installed CLI: select the Python loader explicitly.
        from importlib.machinery import SourceFileLoader
        spec = importlib.util.spec_from_loader("glaze_sdk", SourceFileLoader("glaze_sdk", str(cls.sdk / "bin/glaze-sdk")))
        cls.api = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.api)
        cls.app = cls.root / "external-app"
        cls.cli("doctor", "--cross", ARGS.cross)
        cls.cli("new", cls.app, "--id", "sdk-test", "--name", "SDK Test")
        cls.make("-j4", "check")
        cls.elf = (cls.app / "build/app").read_bytes()

    @classmethod
    def cli(cls, *args, ok=True):
        result = subprocess.run([str(cls.sdk / "bin/glaze-sdk"), *map(str, args)],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if (result.returncode == 0) != ok:
            raise AssertionError(result.stdout)
        return result

    @classmethod
    def make(cls, *args, ok=True):
        result = subprocess.run(["make", "-C", str(cls.app), f"GLAZE_SDK={cls.sdk}",
                                 f"CROSS={ARGS.cross}", *args], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if (result.returncode == 0) != ok:
            raise AssertionError(result.stdout)
        return result

    def test_generation_preserves_existing_files(self):
        before = (self.app / "main.c").read_bytes()
        self.cli("new", self.app, "--id", "oops", "--name", "Oops", ok=False)
        self.assertEqual((self.app / "main.c").read_bytes(), before)
        self.cli("new", self.root / "bad-id", "--id", "../escape", "--name", "Oops", ok=False)
        self.assertFalse((self.root / "bad-id").exists())

    def test_elf_preflight_rejects_incompatible_inputs(self):
        self.assertIn("libui.so.1", self.api.check_elf(self.elf))
        for offset, fmt, value in ((4, "B", 2), (5, "B", 2), (16, "H", 2), (18, "H", 62),
                                   (28, "I", 0xfffffffc), (36, "I", 0x70001000), (44, "H", 33)):
            bad = bytearray(self.elf)
            struct.pack_into("<" + fmt, bad, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                self.api.check_elf(bad)
        bad = self.elf.replace(b"/lib/ld-glaze.so.1", b"/lib/ld-linux.so.1")
        with self.assertRaises(ValueError):
            self.api.check_elf(bad)
        with self.assertRaises(ValueError):
            self.api.check_elf(self.elf[:80])

    def test_package_ranges_and_paths(self):
        good = (self.app / "build/app.gzp").read_bytes()
        self.api.check_package(self.app / "build/app.gzp")
        for name, offset, value in (("traversal", 8, b"../escape\0"),
                                     ("reserved", 8, b"data/save\0"),
                                     ("range", 56, struct.pack("<I", len(good) + 4)),
                                     ("table", 4, struct.pack("<I", 65))):
            bad = bytearray(good)
            bad[offset:offset + len(value)] = value
            path = self.root / (name + ".gzp")
            path.write_bytes(bad)
            self.cli("check", path, ok=False)

    def test_metadata_and_icon_limits(self):
        good = b"id=sdk-test\nname=SDK Test\nversion=1.0\nexec=app\n"
        self.assertEqual(self.api.info_fields(good)["exec"], "app")
        for bad in (good + b"id=duplicate\n", good.replace(b"sdk-test", b"a" * 25),
                    good.replace(b"SDK Test", b"n" * 32), good.replace(b"1.0", b"v" * 16),
                    good.replace(b"exec=app", b"exec=res/app"), good.replace(b"sdk-test", b"a..b")):
            with self.subTest(info=bad), self.assertRaises(ValueError):
                self.api.info_fields(bad)
        icon = (self.app / "res/icon.gzi").read_bytes()
        self.api.check_icon(icon)
        for bad in (icon[:-1], icon.replace(b"GZI1", b"PNG!", 1), icon[:4] + b"\x2b\0" + icon[6:]):
            with self.assertRaises(ValueError):
                self.api.check_icon(bad)

    def test_resource_rebuilds_and_atomic_failed_packaging(self):
        resource = self.app / "res/nested/text.txt"
        resource.parent.mkdir(exist_ok=True)
        resource.write_text("first")
        self.make("package")
        first = (self.app / "build/app.gzp").read_bytes()
        resource.write_text("changed, same build command")
        self.make("package")
        self.assertNotEqual((self.app / "build/app.gzp").read_bytes(), first)
        resource.unlink()
        self.make("package")
        before = (self.app / "build/app.gzp").read_bytes()
        resource.symlink_to(self.app / "main.c")
        self.make("package", ok=False)
        self.assertEqual((self.app / "build/app.gzp").read_bytes(), before)
        resource.unlink()
        self.make("package")
        self.assertEqual((self.app / "build/app.gzp").read_bytes(), before)
        res = self.app / "res"
        res.rename(self.app / "saved-res")
        res.symlink_to(self.app / "saved-res", target_is_directory=True)
        self.make("package", ok=False)
        self.assertEqual((self.app / "build/app.gzp").read_bytes(), before)
        res.unlink()
        (self.app / "saved-res").rename(res)

    def test_multi_source_headers_and_no_host_libc(self):
        extra = self.app / "extra.c"
        extra.write_text('#include "extra.h"\nint extra(void) { return VALUE; }\n')
        header = self.app / "extra.h"
        header.write_text("#define VALUE 1\n")
        self.make("SOURCES=main.c extra.c")
        first = (self.app / "build/extra.o").read_bytes()
        header.write_text("#define VALUE 999\n")
        self.make("SOURCES=main.c extra.c")
        self.assertNotEqual((self.app / "build/extra.o").read_bytes(), first)
        extra.write_text("#include <features.h>\n")
        self.make("SOURCES=main.c extra.c", ok=False)
        extra.unlink()
        header.unlink()
        self.make("check")

    def test_corresponding_source_rebuild(self):
        result = subprocess.run(["make", "-C", str(self.sdk / "src"), "-j4", "OUT=rebuilt", f"CROSS={ARGS.cross}"],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, 0, result.stdout)
        for name in ("libc.so.1", "libgfx.so.1", "libui.so.1", "libmedia.so.1", "ld-glaze.so.1", "crt0.o", "start.o"):
            self.assertTrue((self.sdk / "src/rebuilt" / name).is_file(), name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--cross", default="mipsel-linux-gnu-")
    ARGS = parser.parse_args()
    unittest.main(argv=[__file__], verbosity=2)
