#!/usr/bin/env python3
"""Build and sign Glaze OTA updates (docs/OTA.md, lib/include/glaze/ota.h).

  mkota.py keygen [--key PEM] [--pub config/ota_pubkey.bin]
      Make the release key: a private Ed25519 key outside the source tree
      (default ~/.config/glaze/ota-ed25519.pem) and the 32-byte public key the
      OS trusts. Never overwrites a key.
  mkota.py key-header PUB OUT.h
      The public key as the C array kernel/ota.c includes.
  mkota.py build --kernel kernel.elf --romfs romfs.img --version V --key PEM --out DIR
      DIR/os/glaze-V.gzu (a signed header, a block table, then the NAND's
      16 KiB blocks from the kernel slot through romfs, each deflated on its
      own) and DIR/os/catalog.txt (GZOS1) for the update host.

Signing uses an OpenSSL 3 command line (`openssl pkeyutl -rawin`); set
OPENSSL if the first one on PATH is older (macOS's LibreSSL is).
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mk_nand  # noqa: E402

MAGIC = b"GZOTA1\0\0"
HEADER_SIZE = 512
SIGNED = 448
KERNEL_SLOT = mk_nand.ROMFS_OFFSET - mk_nand.KERNEL_OFFSET
PAYLOAD_MAX = mk_nand.FS_OFFSET - mk_nand.KERNEL_OFFSET
BLOCK = 16384
SPL_MIN = 2                 # GZ_SPL_VERSION: the first SPL that applies updates
DEFAULT_KEY = Path.home() / ".config/glaze/ota-ed25519.pem"
CANDIDATES = ["openssl", "/opt/homebrew/bin/openssl", "/usr/local/opt/openssl@3/bin/openssl",
              "/opt/homebrew/opt/openssl@3/bin/openssl"]


def openssl():
    names = [os.environ["OPENSSL"]] if os.environ.get("OPENSSL") else CANDIDATES
    for name in names:
        path = shutil.which(name)
        if not path:
            continue
        probe = subprocess.run([path, "version"], capture_output=True, text=True)
        if probe.returncode == 0 and probe.stdout.startswith("OpenSSL") and \
                not probe.stdout.startswith(("OpenSSL 0.", "OpenSSL 1.0")):
            return path
    raise SystemExit("mkota.py: needs OpenSSL 1.1.1 or later for Ed25519; set OPENSSL")


def public_key(pem: Path) -> bytes:
    der = subprocess.run([openssl(), "pkey", "-in", str(pem), "-pubout", "-outform", "DER"],
                         capture_output=True, check=True).stdout
    if len(der) != 44 or not der.startswith(bytes.fromhex("302a300506032b6570032100")):
        raise SystemExit(f"mkota.py: {pem} is not an Ed25519 key")
    return der[-32:]


def sign(pem: Path, message: bytes) -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        msg = Path(tmp) / "msg"
        msg.write_bytes(message)
        sig = subprocess.run([openssl(), "pkeyutl", "-sign", "-rawin", "-inkey", str(pem), "-in", str(msg)],
                             capture_output=True, check=True).stdout
    if len(sig) != 64:
        raise SystemExit("mkota.py: unexpected signature length")
    return sig


def version_ok(v: str) -> bool:
    parts = v.split(".")
    return 0 < len(v) <= 15 and all(p.isdigit() and len(p) <= 6 for p in parts)


def payload(kernel_elf: bytes, romfs: bytes, version: str, build: str) -> bytes:
    slot, _ = mk_nand.kernel_slot(kernel_elf, version, build)
    if romfs[:4] != b"GZRF":
        raise ValueError("not a romfs image")
    if len(romfs) > mk_nand.FS_OFFSET - mk_nand.ROMFS_OFFSET:
        raise ValueError(f"romfs ({len(romfs)} bytes) overflows its slot")
    return slot + b"\xff" * (KERNEL_SLOT - len(slot)) + romfs


def blocks(data: bytes) -> bytes:
    """The block table and the blocks (struct gz_ota_block): each 16 KiB as
    raw deflate, or stored when that isn't smaller."""
    count = (len(data) + BLOCK - 1) // BLOCK
    table, packed = bytearray(), bytearray()
    at = HEADER_SIZE + 12 * count
    for i in range(count):
        block = data[i * BLOCK:(i + 1) * BLOCK].ljust(BLOCK, b"\xff")
        c = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
        z = c.compress(block) + c.flush()
        if len(z) >= BLOCK:
            z = block
        table += struct.pack("<3I", at + len(packed), len(z), zlib.crc32(block))
        packed += z
    return bytes(table + packed), count


def header(body: bytes, count: int, version: str, build: str, spl_min: int) -> bytes:
    h = bytearray(HEADER_SIZE)
    h[0:8] = MAGIC
    struct.pack_into("<4I", h, 8, HEADER_SIZE, HEADER_SIZE + len(body), count, spl_min)
    h[24:24 + len(version)] = version.encode()
    b = build.encode()[:31]
    h[40:40 + len(b)] = b
    h[72:104] = hashlib.sha256(body).digest()
    return bytes(h)


def build_update(kernel_elf, romfs, version, build, key, spl_min=SPL_MIN):
    """The .gzu file's bytes."""
    if not version_ok(version):
        raise ValueError(f"version {version!r} must be dotted numeric, at most 15 characters")
    data = payload(kernel_elf, romfs, version, build)
    if len(data) > PAYLOAD_MAX:
        raise ValueError("payload too large")
    body, count = blocks(data)
    h = bytearray(header(body, count, version, build, spl_min))
    h[SIGNED:] = sign(key, bytes(h[:SIGNED]))
    return bytes(h) + body


def romfs_names(romfs: bytes):
    count = struct.unpack_from("<I", romfs, 4)[0]
    return {romfs[8 + i * 56:8 + i * 56 + 48].split(b"\0")[0].decode() for i in range(count)}


def one_line(s: str) -> str:
    s = " ".join(s.split())
    if any(not 32 <= ord(c) <= 126 for c in s):
        raise ValueError("notes must be printable ASCII")
    return s[:200]


def cmd_build(a):
    kernel_elf, romfs = a.kernel.read_bytes(), a.romfs.read_bytes()
    names = romfs_names(romfs)
    # ktest is in romfs only in INIT=bin/ktest builds (Makefile).
    if not a.allow_test_image and ("bin/springboard" not in names or "bin/ktest" in names):
        raise SystemExit("mkota.py: not a release image (INIT=bin/ktest, or no SpringBoard); "
                         "rebuild without INIT, or pass --allow-test-image")
    try:
        data = build_update(kernel_elf, romfs, a.version, a.build, a.key, a.spl_min)
        notes = one_line(a.notes)
    except ValueError as e:
        raise SystemExit(f"mkota.py: {e}")
    os_dir = a.out / "os"
    os_dir.mkdir(parents=True, exist_ok=True)
    name = f"glaze-{a.version}.gzu"
    (os_dir / name).write_bytes(data)
    catalog = ("GZOS1\n[os]\n"
               f"version={a.version}\nbuild={a.build[:31]}\npackage=os/{name}\n"
               f"size={len(data)}\nsha256={hashlib.sha256(data).hexdigest()}\n"
               f"spl_min={a.spl_min}\nnotes={notes}\n")
    (os_dir / "catalog.txt").write_text(catalog)
    print(f"[mkota] {os_dir / name}: {len(data)} bytes, version {a.version}, signed by "
          f"{public_key(a.key).hex()[:16]}...")


def cmd_keygen(a):
    if a.key.exists():
        raise SystemExit(f"mkota.py: {a.key} exists; not replacing a release key")
    a.key.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    tmp = a.key.with_suffix(".tmp")
    subprocess.run([openssl(), "genpkey", "-algorithm", "ed25519", "-out", str(tmp)], check=True)
    os.chmod(tmp, 0o600)
    os.rename(tmp, a.key)
    a.pub.parent.mkdir(parents=True, exist_ok=True)
    a.pub.write_bytes(public_key(a.key))
    print(f"[mkota] private key {a.key} (keep it out of the repository); public key {a.pub}")


def cmd_key_header(a):
    pub = a.pub.read_bytes()
    if len(pub) != 32:
        raise SystemExit(f"mkota.py: {a.pub} must hold a 32-byte Ed25519 public key")
    body = ", ".join(f"0x{b:02x}" for b in pub)
    a.out.write_text("/* Generated by tools/mkota.py from config/ota_pubkey.bin: the OTA release key. */\n"
                     f"static const uint8_t ota_release_key[32] = {{ {body} }};\n")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keygen")
    k.add_argument("--key", type=Path, default=DEFAULT_KEY)
    k.add_argument("--pub", type=Path, default=Path("config/ota_pubkey.bin"))
    h = sub.add_parser("key-header")
    h.add_argument("pub", type=Path)
    h.add_argument("out", type=Path)
    b = sub.add_parser("build")
    b.add_argument("--kernel", type=Path, required=True)
    b.add_argument("--romfs", type=Path, required=True)
    b.add_argument("--version", required=True)
    b.add_argument("--build", default="dev")
    b.add_argument("--key", type=Path, default=Path(os.environ.get("GLAZE_OTA_KEY", DEFAULT_KEY)))
    b.add_argument("--spl-min", type=int, default=SPL_MIN)
    b.add_argument("--notes", default="")
    b.add_argument("--allow-test-image", action="store_true")
    b.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    {"build": cmd_build, "keygen": cmd_keygen, "key-header": cmd_key_header}[a.cmd](a)


if __name__ == "__main__":
    main()
