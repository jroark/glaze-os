#!/usr/bin/env python3
"""Project generation and preflight checks for the Glaze native C SDK."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
EHDR = struct.Struct("<16sHHIIIIIHHHHHH")
PHDR = struct.Struct("<IIIIIIII")
ENTRY = struct.Struct("<48sII")
INTERP = b"/lib/ld-glaze.so.1\0"


def require(ok, message):
    if not ok:
        raise ValueError(message)


def entry_name(name):
    require(0 < len(name.encode("ascii")) < 48 and
            all(0x21 <= ord(c) <= 0x7e and c != "\\" for c in name) and
            all(p and not p.startswith(".") for p in name.split("/")),
            f"invalid package path: {name!r}")


def info_fields(data):
    require(0 < len(data) <= 512, "Info must be 1..512 bytes")
    fields = {}
    for line in data.decode("ascii").splitlines():
        require("=" in line, "Info must contain key=value lines")
        key, value = line.split("=", 1)
        require(key in ("id", "name", "version", "exec") and key not in fields,
                f"unknown or duplicate Info field: {key}")
        fields[key] = value
    for key, limit in (("id", 24), ("name", 31), ("version", 15), ("exec", 15)):
        value = fields.get(key, "")
        require(0 < len(value) <= limit and all(32 <= ord(c) <= 126 for c in value),
                f"Info {key} must be 1..{limit} printable ASCII characters")
    require(re.fullmatch(r"[a-z0-9][a-z0-9.-]*", fields["id"]) and ".." not in fields["id"],
            "Info id must start with a-z/0-9 and contain only a-z, 0-9, dot or hyphen (no '..')")
    entry_name(fields["exec"])
    require("/" not in fields["exec"] and fields["exec"] not in ("Info", "res", "data"),
            "Info exec must be one executable filename")
    return fields


def check_icon(data):
    require(len(data) >= 12 and data[:4] == b"GZI1", "icon must be a GZI1 image")
    w, h, flags, reserved = struct.unpack_from("<HHHH", data, 4)
    require((w, h) == (44, 44) and flags in (0, 1) and reserved == 0 and
            len(data) == 12 + w * h * (2 + flags), "icon must be a valid 44x44 RGB565/A8 GZI")


def check_elf(data):
    require(EHDR.size <= len(data) <= 2 * 1024 * 1024, "ELF must fit Glaze's 2 MiB file limit")
    ident, kind, machine, version, entry, phoff, _, flags, ehsize, phsize, count, *_ = EHDR.unpack_from(data)
    require(ident[:7] == b"\x7fELF\x01\x01\x01" and kind == 3 and machine == 8 and version == 1,
            "app must be an ELF32 little-endian MIPS PIE (not a host/Linux executable)")
    require((flags & 0xf0000000) <= 0x20000000 and not flags & 0x06000060 and
            flags & 0xf000 in (0, 0x1000), "unsupported MIPS ISA/ABI (use MIPS II, O32, soft float)")
    require(ehsize == EHDR.size and phsize == PHDR.size and 0 < count <= 32 and
            phoff % 4 == 0 and phoff + count * phsize <= len(data), "invalid ELF program headers")
    headers = [PHDR.unpack_from(data, phoff + i * phsize) for i in range(count)]
    loads, interpreters, dynamics = [], [], []
    for typ, off, va, _, filesz, memsz, perms, align in headers:
        require(off + filesz <= len(data), "ELF segment extends past file")
        require(typ != 7, "compiler TLS is unsupported; use glaze/thread.h")
        if typ == 1:
            require(filesz <= memsz <= 0x04000000 and va + memsz <= 0x80000000 and
                    off % 4096 == va % 4096 and (perms & 3) != 3 and
                    (align == 0 or (align <= 65536 and not align & (align - 1) and off % align == va % align)),
                    "invalid ELF load segment (alignment, size or writable executable text)")
            if memsz:
                lo, hi = va // 4096, (va + memsz + 4095) // 4096
                require(all(hi <= p[0] or lo >= p[1] for p in loads), "ELF load pages overlap")
                loads.append((lo, hi))
        elif typ == 3:
            interpreters.append(data[off:off + filesz])
        elif typ == 2:
            require(filesz % 8 == 0, "invalid ELF dynamic table")
            dynamics.append(data[off:off + filesz])
        elif typ == 0x70000003:
            require(filesz >= 24 and data[off + 7] == 3, "ELF must use soft-float ABI")
    require(interpreters == [INTERP], "app must use /lib/ld-glaze.so.1")
    require(loads and any(t == 1 and p & 1 and va <= entry < va + ms
                          for t, o, va, pa, fs, ms, p, a in headers), "entry point is outside executable text")
    require(len(dynamics) == 1, "app must have one dynamic table")
    tags = []
    for off in range(0, len(dynamics[0]), 8):
        tag, value = struct.unpack_from("<II", dynamics[0], off)
        if tag == 0:
            break
        tags.append((tag, value))
    else:
        raise ValueError("unterminated ELF dynamic table")
    dt = dict(tags)
    require(4 in dt and 5 in dt and 10 in dt, "ELF requires SysV hash and dynamic string table")
    require(not any(t in (7, 8, 9, 22) for t, v in tags) and not dt.get(30, 0) & 4,
            "ELF uses unsupported RELA or text relocations")
    strings = None
    for typ, off, va, _, filesz, _, _, _ in headers:
        if typ == 1 and va <= dt[5] and dt[5] + dt[10] <= va + filesz:
            strings = data[off + dt[5] - va:off + dt[5] - va + dt[10]]
    require(strings is not None, "dynamic strings are outside loadable file bytes")
    needed = []
    for tag, value in tags:
        if tag != 1:
            continue
        require(value < len(strings) and b"\0" in strings[value:], "invalid dependency name")
        name = strings[value:].split(b"\0", 1)[0].decode("ascii")
        require(name and "/" not in name and (ROOT / "sysroot/usr/lib" / name).is_file(),
                f"dependency is not in this SDK's runtime: {name}")
        needed.append(name)
    return needed


def check_package(path):
    data = path.read_bytes()
    require(len(data) >= 8 and data[:4] == b"GZPK", "not a Glaze .gzp package")
    count, = struct.unpack_from("<I", data, 4)
    require(0 < count <= 64 and 8 + count * ENTRY.size <= len(data), "invalid package entry count/table")
    entries, ranges = {}, []
    for i in range(count):
        raw, off, size = ENTRY.unpack_from(data, 8 + i * ENTRY.size)
        require(b"\0" in raw, "unterminated package entry name")
        name = raw.split(b"\0", 1)[0].decode("ascii")
        entry_name(name)
        require(name not in entries and (name == "Info" or name.startswith("res/") or "/" not in name)
                and name not in ("data", "res"), f"duplicate or reserved entry: {name}")
        require(off >= 8 + count * ENTRY.size and off % 4 == 0 and off + size <= len(data),
                f"invalid package entry range: {name}")
        require(all(off + size <= lo or off >= hi for lo, hi in ranges), "package entries overlap")
        ranges.append((off, off + size))
        entries[name] = data[off:off + size]
    require("Info" in entries and "res/icon.gzi" in entries, "package needs Info and res/icon.gzi")
    info = info_fields(entries["Info"])
    require(info["exec"] in entries, "package is missing its executable")
    require(all(n in ("Info", info["exec"]) or n.startswith("res/") for n in entries),
            "package may contain only Info, its executable and res/ files")
    for name in entries:
        require(not any(other.startswith(name + "/") for other in entries), "package file/directory conflict")
    check_icon(entries["res/icon.gzi"])
    needed = check_elf(entries[info["exec"]])
    print(f"{path}: OK ({info['id']} {info['version']}, {count} files; {', '.join(needed)})")
    return info


def create_project(args):
    info = f"id={args.id}\nname={args.name}\nversion=1.0\nexec=app\n".encode("ascii")
    info_fields(info)
    dest = args.directory
    require(not dest.exists(), f"destination already exists: {dest}")
    shutil.copytree(ROOT / "share/templates/app", dest)
    (dest / "Info").write_bytes(info)
    (dest / "app.h").write_text("#define APP_TITLE " + json.dumps(args.name) + "\n", encoding="ascii")
    (dest / "res").mkdir()
    # Original procedural icon: no platform artwork or external dependencies.
    pixels = bytearray()
    for y in range(44):
        for x in range(44):
            border = min(x, y, 43 - x, 43 - y) < 2
            cross = (19 <= x <= 24 and 10 <= y <= 33) or (10 <= x <= 33 and 19 <= y <= 24)
            pixels += struct.pack("<H", 0xffff if cross else 0x1949 if border else 0x351b)
    (dest / "res/icon.gzi").write_bytes(b"GZI1" + struct.pack("<HHHH", 44, 44, 0, 0) + pixels)
    print(f"Created {dest}. Set GLAZE_SDK={ROOT} and run make in that directory.")


def package(args):
    project = args.project.resolve()
    info = info_fields((project / "Info").read_bytes())
    files = [("Info", project / "Info"), (info["exec"], args.elf.resolve())]
    require(not (project / "res").is_symlink(), "resource directories must not be symlinks")
    for path in sorted((project / "res").rglob("*")):
        require(not path.is_symlink(), f"resource symlinks are unsupported: {path}")
        if path.is_file():
            files.append((path.relative_to(project).as_posix(), path))
    require(2 < len(files) <= 64, "package needs an icon and at most 64 total files")
    for name, _ in files:
        entry_name(name)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.output.parent) as tmp:
        output = Path(tmp) / "app.gzp"
        subprocess.run([sys.executable, str(ROOT / "tools/mkromfs.py"), "--package", str(output)] +
                       [f"{name}={path}" for name, path in files], check=True)
        check_package(output)
        os.replace(output, args.output)
    print(f"Wrote {args.output}")


def doctor(args):
    manifest = json.loads((ROOT / "sdk.json").read_text())
    print(f"Glaze SDK {manifest['revision']} (C / MIPS II / O32 / soft float)")
    missing = []
    for command in (args.cross + "gcc", args.cross + "ld", args.cross + "objcopy", "make"):
        path = shutil.which(command)
        print(f"  {command}: {path or 'MISSING'}")
        if not path:
            missing.append(command)
    require(not missing, "install the cross toolchain and GNU make; see docs/SDK.md")
    result = subprocess.run([args.cross + "gcc", "-dumpmachine"], check=True, text=True, capture_output=True)
    require(result.stdout.strip().startswith("mipsel"), "cross compiler must target little-endian MIPS")
    print("Optional: Pillow for PNG conversion; mtools/dosfstools for CF; libusb/pkg-config for device deployment.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("doctor", help="check the external cross toolchain")
    p.add_argument("--cross", default=os.environ.get("CROSS", "mipsel-linux-gnu-"))
    p.set_defaults(func=doctor)
    p = sub.add_parser("new", help="create a C app project without overwriting files")
    p.add_argument("directory", type=Path)
    p.add_argument("--id", required=True)
    p.add_argument("--name", required=True)
    p.set_defaults(func=create_project)
    p = sub.add_parser("package", help="package and check a project's stripped ELF and resources")
    p.add_argument("--project", type=Path, required=True)
    p.add_argument("--elf", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.set_defaults(func=package)
    p = sub.add_parser("check", help="check package structure, metadata, icon and ELF compatibility")
    p.add_argument("package", type=Path)
    p.set_defaults(func=lambda args: check_package(args.package))
    args = parser.parse_args()
    try:
        args.func(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"glaze-sdk: {exc}\n")


if __name__ == "__main__":
    main()
