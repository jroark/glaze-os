#!/usr/bin/env python3
"""Drive the native OTA test (make -C host ota-test, tests/ota.c).

Makes throwaway Ed25519 keys, a stand-in kernel ELF and romfs for an old
(1.0) and a new (1.1) OS, signs update files with tools/mkota.py, builds a
684-block physical file system holding the new one with host/mkfsimg, and
runs the C harness on them. Nothing here touches the release key.

usage: ota_test.py MKFSIMG OTA_TEST
"""
import os
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import mkota  # noqa: E402

STAGED = "/Library/Updates/glaze-1.1.gzu"
FS_BLOCKS = 684


def kernel_elf(rng, size):
    """A one-segment MIPS ELF at the kernel's load address."""
    load = 0x80010000
    body = bytes(rng.getrandbits(8) for _ in range(size))
    ehdr = bytearray(52)
    ehdr[0:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHIIIIIHHHHHH", ehdr, 16, 2, 8, 1, load, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    phdr = struct.pack("<8I", 1, 84, load, load, size, size + 4096, 7, 4096)
    return bytes(ehdr) + phdr + body


def romfs(rng, sizes):
    count = len(sizes)
    table = bytearray(b"GZRF" + struct.pack("<I", count))
    off = (8 + count * 56 + 4095) & ~4095
    files = bytearray()
    for i, n in enumerate(sizes):
        name = f"bin/f{i}".encode().ljust(48, b"\0")
        table += name + struct.pack("<II", off + len(files), n)
        files += bytes(rng.getrandbits(8) for _ in range(n))
        files += b"\0" * (-len(files) % 4096)
    return bytes(table.ljust(off, b"\0") + files)


def main():
    mkfsimg, harness = sys.argv[1], sys.argv[2]
    rng = random.Random(300)
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        key, wrong = t / "key.pem", t / "wrong.pem"
        for k in (key, wrong):
            subprocess.run([mkota.openssl(), "genpkey", "-algorithm", "ed25519", "-out", str(k)], check=True)
        (t / "pub.bin").write_bytes(mkota.public_key(key))

        old = mkota.payload(kernel_elf(rng, 120000), romfs(rng, [200000, 900000, 3000]), "1.0", "old")
        # Some of the new romfs compresses, some doesn't (stored blocks).
        new_kernel, new_romfs = kernel_elf(rng, 131000), romfs(rng, [1500000, 70000, 2200000, 9000])
        new_romfs = new_romfs[:300000] + bytes(400000) + new_romfs[700000:]
        new = mkota.build_update(new_kernel, new_romfs, "1.1", "new", key)
        bad_key = mkota.build_update(new_kernel, new_romfs, "1.1", "new", wrong)
        downgrade = mkota.build_update(new_kernel, new_romfs, "0.9", "old", key)
        for name, data in (("old.bin", old), ("new.gzu", new), ("new.bin", mkota.payload(new_kernel, new_romfs, "1.1", "new")),
                           ("bad.gzu", bad_key), ("down.gzu", downgrade)):
            (t / name).write_bytes(data)

        # Some user files around the staged one, as on a used device.
        userdata = t / "userdata"
        (userdata / "Documents").mkdir(parents=True)
        (userdata / "Documents" / "notes.txt").write_bytes(os.urandom(70000))
        (userdata / "Pictures").mkdir()
        (userdata / "Pictures" / "a.jpg").write_bytes(os.urandom(300000))
        subprocess.run([mkfsimg, str(userdata), str(FS_BLOCKS), str(t / "fs.img"),
                        f"{STAGED}={t / 'new.gzu'}"], check=True)
        subprocess.run([harness, str(t / "fs.img"), STAGED, str(t / "old.bin"), str(t / "new.gzu"),
                        str(t / "new.bin"), str(t / "pub.bin"), str(t / "bad.gzu"), str(t / "down.gzu")], check=True)


if __name__ == "__main__":
    main()
