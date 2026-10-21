#!/usr/bin/env python3
"""Build and boot one original app using only developer-supplied local assets.

Preinstallation saves time during ABI bring-up. Installation through Files is
tested separately by t_wince_install.txt. All test images are disposable copies.
"""
import argparse
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

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--app", choices=APPLICATIONS, default="calculator")
    parser.add_argument("--seconds", type=int, default=24)
    parser.add_argument("--input", type=Path)
    parser.add_argument("--reuse-fs", type=Path, help="reuse a previous test NAND's installed apps and saved documents")
    parser.add_argument("--check-document", action="store_true", help="assert original Notepad saved Hello from Glaze")
    parser.add_argument("--check-owner", action="store_true", help="assert original Owner saved both tabs")
    parser.add_argument("--check-regedit", action="store_true", help="assert original RegEdit saved its test key and string")
    parser.add_argument("--regedit-value", default="Hello from RegEdit", help="expected Greeting text with --check-regedit")
    parser.add_argument("--check-tweak", action="store_true", help="assert PalmTweak saved animation and battery timeout settings")
    parser.add_argument("--expect-exit", type=int, help="assert the original app exited with this status")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    os.chdir(root)
    (root / "out").mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=f"wince-{args.app}-", dir=root / "out"))
    script = args.input or Path(f"tests/t_wince_{args.app}.txt")
    with (work / "build.log").open("w") as log:
        subprocess.run(["./build.sh", f"INPUT_SCRIPT={script}"], stdout=log, stderr=subprocess.STDOUT, check=True)
    nand = bytearray((root / "out/glaze.bin").read_bytes())
    shutil.copyfile(root / "out/romfs/bin/wince", work / "runner")
    subprocess.run([sys.executable, "tools/import_wince.py", "--image", str(args.image), "--app", args.app, "--output", str(work / "assets"), "--runner", str(work / "runner")], check=True)
    shutil.copytree(root / "userdata", work / "userdata")
    archive = (work / "assets" / f"wince.{args.app}.gzp").read_bytes()
    bundle = work / "userdata" / "Applications" / f"wince.{args.app}.app"
    bundle.mkdir(parents=True)
    for i in range(struct.unpack_from("<I", archive, 4)[0]):
        name, off, size = struct.unpack_from("<48sII", archive, 8 + 56 * i)
        name = name.split(b"\0")[0].decode()
        target = bundle / name
        if not target.resolve().is_relative_to(bundle.resolve()):
            raise ValueError("unsafe test package path")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(archive[off:off + size])
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
            f"/Applications/wince.{args.app}.app/app"], capture_output=True, check=True).stdout
        if previous != (work / "runner").read_bytes():
            raise RuntimeError("reuse filesystem contains an older runner; first make a fresh save run with the current sources")
    emulator = os.environ.get("BE300", str(root.parent / "be300/build-host/be300"))
    with (work / "stdout.log").open("w") as stdout, (work / "stderr.log").open("w") as stderr:
        process = subprocess.Popen([emulator, "--nand", "glaze.bin", "--rtc-host-time"], cwd=work, stdout=stdout, stderr=stderr)
        try:
            process.wait(timeout=args.seconds)
        except subprocess.TimeoutExpired:
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=10)
    print(work)
    lines = (work / "stdout.log").read_text(errors="replace").splitlines()
    for line in lines:
        if any(token in line for token in ("wince:", "fault:", "PANIC", "sb: launch")):
            print(line)
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
        name = re.escape(f"wince.{args.app}"[:15])
        if not any(re.search(rf"proc: {name} pid \d+ exited \({args.expect_exit}\)", line) for line in lines):
            raise RuntimeError("original app did not exit with the expected status")
        print(f"wince: original app exit status {args.expect_exit} verified")


if __name__ == "__main__":
    main()
