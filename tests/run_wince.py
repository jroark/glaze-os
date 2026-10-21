#!/usr/bin/env python3
"""Build and boot one original app using only developer-supplied local assets.

Preinstallation saves time during ABI bring-up. Installation through Files is
tested separately by t_wince_install.txt. All test images are disposable copies.
"""
import argparse
import json
import os
import re
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from import_wince import APPLICATIONS


def read_package(path):
    """Validate the package before writing any of its entries to disk."""
    archive = path.read_bytes()
    if len(archive) < 8 or archive[:4] != b"GZPK":
        raise ValueError("invalid package header")
    count = struct.unpack_from("<I", archive, 4)[0]
    table_end = 8 + 56 * count
    if not 1 <= count <= 64 or table_end > len(archive):
        raise ValueError("invalid package table")
    entries = {}
    for i in range(count):
        raw, off, size = struct.unpack_from("<48sII", archive, 8 + 56 * i)
        if b"\0" not in raw:
            raise ValueError("unterminated package path")
        name = raw.split(b"\0", 1)[0].decode("ascii")
        parts = name.split("/")
        if (any(p in ("", ".", "..") for p in parts) or "\\" in name
                or any(ord(c) < 33 or ord(c) > 126 for c in name)
                or parts[0] in ("data", ".pkg-ready") or name in entries
                or off < table_end or off > len(archive) or size > len(archive) - off):
            raise ValueError("invalid package entry")
        entries[name] = archive[off:off + size]
    info = dict(line.split("=", 1) for line in entries.get("Info", b"").decode("ascii").splitlines() if "=" in line)
    app_id, executable = info.get("id", ""), info.get("exec", "app")
    if (not re.fullmatch(r"[a-z0-9][a-z0-9.-]{0,23}", app_id) or ".." in app_id
            or not info.get("name") or "/" in executable or executable not in entries):
        raise ValueError("invalid package metadata")
    return app_id, executable, entries


def read_registry(data):
    """Read the private registry for assertions independently of the guest."""
    if len(data) < 8 or data[:4] != b"GZCE":
        raise ValueError("invalid registry header")
    pos, values, keys = 8, {}, set()
    for _ in range(struct.unpack_from("<I", data, 4)[0]):
        key_size, name_size, kind, size = struct.unpack_from("<4I", data, pos)
        pos += 16
        end = pos + key_size + name_size + size
        if end > len(data) or not key_size or not name_size:
            raise ValueError("invalid registry value")
        key = data[pos:pos + key_size].rstrip(b"\0").decode().casefold()
        pos += key_size
        name = data[pos:pos + name_size].rstrip(b"\0").decode().casefold()
        pos += name_size
        values[key, name] = kind, data[pos:pos + size]
        keys.add(key)
        pos += size
    if data[pos:pos + 4] == b"GZCK":
        count = struct.unpack_from("<I", data, pos + 4)[0]
        pos += 8
        for _ in range(count):
            size = struct.unpack_from("<I", data, pos)[0]
            pos += 4
            if not size or pos + size > len(data):
                raise ValueError("invalid registry key")
            keys.add(data[pos:pos + size].rstrip(b"\0").decode().casefold())
            pos += size
    if pos != len(data):
        raise ValueError("trailing registry data")
    return keys, values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--image", type=Path)
    source.add_argument("--package", type=Path, help="test an existing GZP with the freshly built runtime")
    parser.add_argument("--app", choices=APPLICATIONS)
    parser.add_argument("--net", action="store_true", help="insert the emulator's NE2000 network card")
    parser.add_argument("--no-build", action="store_true", help="reuse out/glaze.bin and its existing input script")
    parser.add_argument("--headless", action="store_true", help="run with SDL's dummy video and audio drivers")
    parser.add_argument("--seconds", type=int, default=24)
    parser.add_argument("--input", type=Path)
    parser.add_argument("--reuse-fs", type=Path, help="reuse a previous test NAND's installed apps and saved documents")
    parser.add_argument("--check-document", action="store_true", help="assert original Notepad saved Hello from Glaze")
    parser.add_argument("--check-owner", action="store_true", help="assert original Owner saved both tabs")
    parser.add_argument("--check-regedit", action="store_true", help="assert original RegEdit saved its test key and string")
    parser.add_argument("--regedit-value", default="Hello from RegEdit", help="expected Greeting text with --check-regedit")
    parser.add_argument("--check-tweak", action="store_true", help="assert PalmTweak saved animation and battery timeout settings")
    parser.add_argument("--expect-exit", type=int, help="assert the original app exited with this status")
    parser.add_argument("--check-key", action="append", default=[], help="assert a persisted private registry key")
    parser.add_argument("--check-string", nargs=3, action="append", default=[], metavar=("KEY", "NAME", "TEXT"),
                        help="assert a persisted REG_SZ value")
    args = parser.parse_args()
    if args.package and (args.app or any((args.check_document, args.check_owner, args.check_regedit, args.check_tweak))):
        parser.error("--package cannot be combined with NAND app selection or app-specific checks")
    args.app = args.app or "calculator"
    package = read_package(args.package) if args.package else None
    app_id = package[0] if package else f"wince.{args.app}"
    root = Path(__file__).resolve().parent.parent
    os.chdir(root)
    (root / "out").mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=f"{app_id}-", dir=root / "out"))
    print(work, flush=True)
    script = args.input or Path("tests/t_wince_package.txt" if package else f"tests/t_wince_{args.app}.txt")
    if not args.no_build:
        with (work / "build.log").open("w") as log:
            subprocess.run(["./build.sh", f"INPUT_SCRIPT={script}"], stdout=log, stderr=subprocess.STDOUT, check=True)
    nand = bytearray((root / "out/glaze.bin").read_bytes())
    shutil.copyfile(root / "out/romfs/bin/wince", work / "runner")
    if not package:
        subprocess.run([sys.executable, "tools/import_wince.py", "--image", str(args.image), "--app", args.app, "--output", str(work / "assets"), "--runner", str(work / "runner")], check=True)
        package = read_package(work / "assets" / f"{app_id}.gzp")
    shutil.copytree(root / "userdata", work / "userdata")
    _, executable, entries = package
    entries[executable] = (work / "runner").read_bytes()
    bundle = work / "userdata" / "Applications" / f"{app_id}.app"
    bundle.mkdir(parents=True)
    for name, data in entries.items():
        target = bundle / name
        if not target.resolve().is_relative_to(bundle.resolve()):
            raise ValueError("unsafe test package path")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    subprocess.run(["make", "OUT=out/wince-native", "out/wince-native/host/mkfsimg"], check=True)
    blocks = re.search(r"#define GZ_FLASH_BLOCKS\s+(\d+)", (root / "lib/include/glaze/abi.h").read_text()).group(1)
    subprocess.run([str(root / "out/wince-native/host/mkfsimg"), str(work / "userdata"), blocks, str(work / "fs.img")], check=True)
    fs = args.reuse_fs.read_bytes()[0x500000:] if args.reuse_fs else (work / "fs.img").read_bytes()
    if len(fs) != len(nand) - 0x500000:
        raise ValueError("test filesystem does not match flash geometry")
    nand[0x500000:] = fs
    (work / "glaze.bin").write_bytes(nand)
    if args.reuse_fs:
        subprocess.run(["make", "-C", "host", "../out/host/wince-fs-read"], check=True)
        previous = subprocess.run([str(root / "out/host/wince-fs-read"), str(work / "glaze.bin"),
            f"/Applications/{app_id}.app/{executable}"], capture_output=True, check=True).stdout
        if previous != (work / "runner").read_bytes():
            raise RuntimeError("reuse filesystem contains an older runner; first make a fresh save run with the current sources")
    emulator = os.environ.get("BE300", str(root.parent / "be300/build-host/be300"))
    with (work / "stdout.log").open("w") as stdout, (work / "stderr.log").open("w") as stderr:
        command = [emulator, "--nand", "glaze.bin", "--rtc-host-time"]
        if args.net:
            command.append("--ne2000")
        env = dict(os.environ)
        if args.headless:
            env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        process = subprocess.Popen(command, cwd=work, stdout=stdout, stderr=stderr, env=env)
        try:
            process.wait(timeout=args.seconds)
        except subprocess.TimeoutExpired:
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=10)
    print(work)
    lines = (work / "stdout.log").read_text(errors="replace").splitlines()
    report = {
        "app_id": app_id,
        "entered": any("wince: entering original executable" in line for line in lines),
        "loaded": [line for line in lines if line.startswith("wince: loaded ")],
        "unresolved": [line for line in lines if "wince: unresolved import" in line],
        "failures": [line for line in lines if any(token in line for token in ("fault:", "PANIC", "wince: error:"))],
        "memory": [line for line in lines if line.startswith("sb: launch ")],
    }
    (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    for line in lines:
        if any(token in line for token in ("wince:", "fault:", "PANIC", "sb: launch")):
            print(line)
    if args.check_key or args.check_string:
        subprocess.run(["make", "-C", "host", "../out/host/wince-fs-read"], check=True)
        result = subprocess.run([str(root / "out/host/wince-fs-read"), str(work / "glaze.bin"),
            f"/Applications/{app_id}.app/data/registry"], capture_output=True, check=True)
        keys, values = read_registry(result.stdout)
        for key in args.check_key:
            if key.casefold() not in keys:
                raise RuntimeError(f"missing persisted registry key: {key}")
        for key, name, expected in args.check_string:
            actual = values.get((key.casefold(), name.casefold()))
            if not actual or actual[0] != 1 or actual[1].decode("utf-16le").rstrip("\0") != expected:
                raise RuntimeError(f"incorrect persisted registry string: {key}\\{name}")
        print("wince: persisted registry assertions verified")
    if args.check_document:
        subprocess.run(["make", "-C", "host", "../out/host/wince-fs-read"], check=True)
        result = subprocess.run([str(root / "out/host/wince-fs-read"), str(work / "glaze.bin"), "/Documents/compatibility.txt"], capture_output=True, check=True)
        encoding = "utf-16" if result.stdout.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
        if result.stdout.decode(encoding) != "Hello from Glaze":
            raise RuntimeError("original Notepad did not preserve the expected document text")
        print("wince: persisted document content verified")
    if args.check_owner:
        subprocess.run(["make", "-C", "host", "../out/host/wince-fs-read"], check=True)
        path = f"/Applications/wince.{args.app}.app/data/registry"
        data = subprocess.run([str(root / "out/host/wince-fs-read"), str(work / "glaze.bin"), path], capture_output=True, check=True).stdout
        if data[:4] != b"GZCE":
            raise RuntimeError("invalid Owner registry")
        values, pos = {}, 8
        for _ in range(struct.unpack_from("<I", data, 4)[0]):
            key, name, kind, size = struct.unpack_from("<4I", data, pos)
            pos += 16
            name_text = data[pos + key:pos + key + name].rstrip(b"\0").decode()
            values[name_text] = data[pos + key + name:pos + key + name + size]
            pos += key + name + size
        for key, expected in (("Owner", "Alex from Glaze"), ("Notes", "Original app compatibility")):
            if values.get(key, b"").decode("utf-16le").split("\0")[0] != expected:
                raise RuntimeError(f"original Owner did not preserve {key}")
        if len(values["Notes"]) < 387 or values["Notes"][386] != 1:
            raise RuntimeError("original Owner did not preserve the Notes checkbox")
        print("wince: persisted Owner identification and notes verified")
    if args.check_regedit:
        subprocess.run(["make", "-C", "host", "../out/host/wince-fs-read"], check=True)
        data = subprocess.run([str(root / "out/host/wince-fs-read"), str(work / "glaze.bin"),
            "/Applications/wince.regedit.app/data/registry"], capture_output=True, check=True).stdout
        if data[:4] != b"GZCE":
            raise RuntimeError("invalid RegEdit registry")
        values, pos = {}, 8
        for _ in range(struct.unpack_from("<I", data, 4)[0]):
            key, name, kind, size = struct.unpack_from("<4I", data, pos)
            pos += 16
            key_text = data[pos:pos + key].rstrip(b"\0").decode()
            name_text = data[pos + key:pos + key + name].rstrip(b"\0").decode()
            values[key_text.casefold(), name_text.casefold()] = (kind, data[pos + key + name:pos + key + name + size])
            pos += key + name + size
        value = values.get(("hkcu\\glazecompat", "greeting"))
        if not value or value[0] != 1 or value[1].decode("utf-16le").rstrip("\0") != args.regedit_value:
            raise RuntimeError("original RegEdit did not preserve its key and string value")
        if data[pos:pos + 4] != b"GZCK":
            raise RuntimeError("RegEdit did not persist its key directory")
        print("wince: persisted RegEdit key and string value verified")
    if args.check_tweak:
        subprocess.run(["make", "-C", "host", "../out/host/wince-fs-read"], check=True)
        data = subprocess.run([str(root / "out/host/wince-fs-read"), str(work / "glaze.bin"),
            "/Applications/wince.tweak.app/data/registry"], capture_output=True, check=True).stdout
        if data[:4] != b"GZCE":
            raise RuntimeError("invalid PalmTweak registry")
        values, pos = {}, 8
        for _ in range(struct.unpack_from("<I", data, 4)[0]):
            key, name, kind, size = struct.unpack_from("<4I", data, pos)
            pos += 16
            key_text = data[pos:pos + key].rstrip(b"\0").decode().casefold()
            name_text = data[pos + key:pos + key + name].rstrip(b"\0").decode().casefold()
            payload = data[pos + key + name:pos + key + name + size]
            values[key_text, name_text] = (kind, payload)
            pos += key + name + size
        expected = [(r"hklm\system\gwe", "animate", 0),
            (r"hklm\system\currentcontrolset\control\power", "battpoweroff", 360)]
        for key, name, value in expected:
            if values.get((key, name)) != (4, struct.pack("<I", value)):
                raise RuntimeError(f"original PalmTweak did not preserve {key}\\{name}={value}")
        print("wince: persisted PalmTweak animation and six-minute battery timeout verified")
    if not any("wince: entering original executable" in line for line in lines):
        raise RuntimeError("original executable did not start; inspect test logs")
    if any(any(token in line for token in ("fault:", "PANIC", "Unsupported WinCE call:", "wince: error:")) for line in lines):
        raise RuntimeError("compatibility runtime failed; inspect test logs")
    if args.expect_exit is not None:
        name = re.escape(app_id[:15])
        if not any(re.search(rf"proc: {name} pid \d+ exited \({args.expect_exit}\)", line) for line in lines):
            raise RuntimeError("original app did not exit with the expected status")
        print(f"wince: original app exit status {args.expect_exit} verified")


if __name__ == "__main__":
    main()
