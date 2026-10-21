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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--app", choices=("calculator", "notepad"), default="calculator")
    parser.add_argument("--seconds", type=int, default=18)
    parser.add_argument("--input", type=Path)
    parser.add_argument("--reuse-fs", type=Path, help="reuse a previous test NAND's installed apps and saved documents")
    parser.add_argument("--check-document", action="store_true", help="assert original Notepad saved Hello from Glaze")
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
    subprocess.run([sys.executable, "tools/import_wince.py", "--image", str(args.image), "--output", str(work / "assets"), "--runner", str(work / "runner")], check=True)
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
    if not any("wince: entering original executable" in line for line in lines):
        raise RuntimeError("original executable did not start; inspect test logs")
    if any(any(token in line for token in ("fault:", "PANIC", "Unsupported WinCE call:", "wince: error:")) for line in lines):
        raise RuntimeError("compatibility runtime failed; inspect test logs")


if __name__ == "__main__":
    main()
