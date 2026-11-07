#!/usr/bin/env python3
"""Build a hardware-support package (lib/include/glaze/hwpkg.h).

  mkhwpkg.py --id ID --name NAME --version V --os GLAZE_VERSION
             [--key PEM | --unsigned] --out OUT.gzp PATH=FILE [PATH=FILE ...]

PATH is the romfs-style name the kernel looks up: drivers/<name>.ko,
bin/<service> or firmware/<file>. Kernel code (drivers/, bin/) must be signed
with the OTA release key (default ~/.config/glaze/ota-ed25519.pem), and only
loads on the OS version given by --os. A package of firmware alone may be
--unsigned: it is built from files the user supplies (never distributed).

  mkhwpkg.py verify --pub config/ota_pubkey.bin PKG.gzp

checks a package as the kernel would, natively.
"""

import argparse
import hashlib
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mkota  # noqa: E402

MAGIC = b"GZHWMF1\0"
FORMAT = 1
SIGNED = 1
CODE, DATA = 1, 2
HEADER = struct.Struct("<8sIII28s16s")
ENTRY = struct.Struct("<48sII32s")
SIGNATURE = 64
FILE_MAX = 512 * 1024
ENTRIES_MAX = 8
PKG_ENTRY = struct.Struct("<48sII")
ID_RE = re.compile(r"^[a-z0-9][a-z0-9.-]{0,23}$")
NAME_RE = re.compile(r"^[A-Za-z0-9._-]+$")


def path_kind(path: str) -> int:
    for prefix, kind in (("drivers/", CODE), ("bin/", CODE), ("firmware/", DATA)):
        if path.startswith(prefix):
            name = path[len(prefix):]
            if not name or name.startswith(".") or not NAME_RE.match(name):
                break
            if prefix == "drivers/" and not name.endswith(".ko"):
                break
            if len(path.encode()) >= 48:
                break
            return kind
    raise SystemExit(f"mkhwpkg.py: {path}: not drivers/<name>.ko, bin/<name> or firmware/<name>")


def manifest(pkg_id: str, os_version: str, files, key):
    code = any(kind == CODE for _, _, kind in files)
    if code and key is None:
        raise SystemExit("mkhwpkg.py: drivers and services must be signed (--key)")
    body = bytearray(HEADER.pack(MAGIC, FORMAT, SIGNED if key else 0, len(files),
                                 pkg_id.encode(), os_version.encode()))
    for path, data, kind in files:
        body += ENTRY.pack(path.encode(), len(data), kind, hashlib.sha256(data).digest())
    if key:
        body += mkota.sign(key, bytes(body))
    return bytes(body)


def pack(entries) -> bytes:
    """The GZPK container, as tools/mkromfs.py --package writes it."""
    entries = sorted(entries)
    header_len = 8 + PKG_ENTRY.size * len(entries)
    offset = (header_len + 3) // 4 * 4
    table = bytearray(b"GZPK" + struct.pack("<I", len(entries)))
    blobs = bytearray()
    for name, data in entries:
        if len(name.encode()) >= 48:
            raise SystemExit(f"mkhwpkg.py: entry name too long: {name}")
        table += PKG_ENTRY.pack(name.encode(), offset + len(blobs), len(data))
        blobs += data
        blobs += b"\0" * (-len(blobs) % 4)
    return bytes(table + b"\0" * (offset - len(table)) + blobs)


def cmd_build(a):
    if not ID_RE.match(a.id) or ".." in a.id:
        raise SystemExit(f"mkhwpkg.py: bad package id {a.id!r}")
    if not mkota.version_ok(a.os):
        raise SystemExit(f"mkhwpkg.py: bad OS version {a.os!r}")
    if "\n" in a.name or len(a.name.encode()) >= 32 or "\n" in a.version or len(a.version) >= 16:
        raise SystemExit("mkhwpkg.py: name or version too long")
    files = []
    for spec in a.files:
        path, _, src = spec.partition("=")
        data = Path(src).read_bytes()
        if not data or len(data) > FILE_MAX:
            raise SystemExit(f"mkhwpkg.py: {src}: {len(data)} bytes (1..{FILE_MAX})")
        files.append((path, data, path_kind(path)))
    if not 1 <= len(files) <= ENTRIES_MAX or len({p for p, _, _ in files}) != len(files):
        raise SystemExit(f"mkhwpkg.py: 1..{ENTRIES_MAX} distinct files")
    key = None if a.unsigned else Path(a.key)
    if key is not None and not key.exists():
        raise SystemExit(f"mkhwpkg.py: no signing key at {key} (or pass --unsigned for firmware)")
    info = f"id={a.id}\nname={a.name}\nversion={a.version}\ntype=hardware\n".encode()
    entries = [("Info", info), ("Manifest", manifest(a.id, a.os, files, key))]
    entries += [(f"hw/{path}", data) for path, data, _ in files]
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(pack(entries))
    how = f"signed by {mkota.public_key(key).hex()[:16]}..." if key else "unsigned"
    print(f"[mkhwpkg] {out}: {', '.join(p for p, _, _ in files)} for {a.os}, {how}")


def read_package(path: Path):
    data = path.read_bytes()
    if data[:4] != b"GZPK":
        raise SystemExit(f"{path}: not a package")
    (count,) = struct.unpack_from("<I", data, 4)
    entries = {}
    for i in range(count):
        name, off, size = PKG_ENTRY.unpack_from(data, 8 + i * PKG_ENTRY.size)
        entries[name.rstrip(b"\0").decode()] = data[off:off + size]
    return entries


def ed25519_verify(pub: bytes, message: bytes, signature: bytes) -> bool:
    # DER SubjectPublicKeyInfo for Ed25519, then OpenSSL's raw verify.
    der = bytes.fromhex("302a300506032b6570032100") + pub
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        (t / "pub.der").write_bytes(der)
        (t / "msg").write_bytes(message)
        (t / "sig").write_bytes(signature)
        r = subprocess.run([mkota.openssl(), "pkeyutl", "-verify", "-pubin", "-keyform", "DER",
                            "-inkey", str(t / "pub.der"), "-rawin", "-in", str(t / "msg"),
                            "-sigfile", str(t / "sig")], capture_output=True)
        return r.returncode == 0


def cmd_verify(a):
    entries = read_package(Path(a.package))
    m = entries.get("Manifest")
    if m is None or b"type=hardware" not in entries.get("Info", b""):
        raise SystemExit(f"{a.package}: not a hardware package")
    magic, fmt, flags, count, pkg_id, os_version = HEADER.unpack_from(m)
    if magic != MAGIC or fmt != FORMAT:
        raise SystemExit(f"{a.package}: bad manifest")
    want = HEADER.size + count * ENTRY.size + (SIGNATURE if flags & SIGNED else 0)
    if len(m) != want:
        raise SystemExit(f"{a.package}: manifest is {len(m)} bytes, not {want}")
    code = False
    for i in range(count):
        path, size, kind, digest = ENTRY.unpack_from(m, HEADER.size + i * ENTRY.size)
        path = path.rstrip(b"\0").decode()
        if path_kind(path) != kind:
            raise SystemExit(f"{a.package}: {path} has the wrong kind")
        code |= kind == CODE
        data = entries.get(f"hw/{path}")
        if data is None or len(data) != size or hashlib.sha256(data).digest() != digest:
            raise SystemExit(f"{a.package}: hw/{path} doesn't match the manifest")
    if code and not flags & SIGNED:
        raise SystemExit(f"{a.package}: code in an unsigned package")
    if flags & SIGNED:
        pub = Path(a.pub).read_bytes()
        if not ed25519_verify(pub, m[:-SIGNATURE], m[-SIGNATURE:]):
            raise SystemExit(f"{a.package}: bad signature for {a.pub}")
    print(f"{a.package}: {pkg_id.rstrip(chr(0).encode()).decode()} for "
          f"{os_version.rstrip(chr(0).encode()).decode()}, {count} files, "
          f"{'signed' if flags & SIGNED else 'unsigned'}: OK")


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "verify":
        p = argparse.ArgumentParser(prog="mkhwpkg.py verify")
        p.add_argument("--pub", default="config/ota_pubkey.bin")
        p.add_argument("package")
        cmd_verify(p.parse_args(sys.argv[2:]))
        return
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--id", required=True)
    p.add_argument("--name", required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--os", required=True, help="the GLAZE_VERSION its code was built for")
    p.add_argument("--key", default=str(mkota.DEFAULT_KEY))
    p.add_argument("--unsigned", action="store_true")
    p.add_argument("--out", required=True)
    p.add_argument("files", nargs="+", metavar="PATH=FILE")
    cmd_build(p.parse_args())


if __name__ == "__main__":
    main()
