#!/usr/bin/env python3
"""Export a relocatable Glaze C SDK; build artifacts must already exist."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent.parent
LIBS = ("libc.so.1", "libgfx.so.1", "libui.so.1", "libmedia.so.1", "ld-glaze.so.1")


def copy(source, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    if source.is_dir():
        shutil.copytree(source, dest, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    else:
        shutil.copyfile(source, dest)


def archive(directory, path):
    """Stable archive metadata, including gzip header (no host paths/owners)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=path.parent) as tmp:
        output = Path(tmp) / "sdk.tar.gz"
        with output.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as gz:
            with tarfile.open(fileobj=gz, mode="w", format=tarfile.PAX_FORMAT) as tar:
                for p in [directory] + sorted(directory.rglob("*")):
                    name = "glaze-sdk" + ("/" + p.relative_to(directory).as_posix() if p != directory else "")
                    item = tar.gettarinfo(str(p), arcname=name)
                    item.uid = item.gid = item.mtime = 0
                    item.uname = item.gname = ""
                    item.mode = 0o755 if p.is_dir() or p.stat().st_mode & 0o111 else 0o644
                    if p.is_file():
                        with p.open("rb") as source:
                            tar.addfile(item, source)
                    else:
                        tar.addfile(item)
        output.replace(path)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    path.with_name(path.name + ".sha256").write_text(f"{digest}  {path.name}\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-dir", type=Path)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--archive", type=Path)
    ap.add_argument("--archive-only", action="store_true", help="archive an already exported SDK")
    ap.add_argument("--revision")
    ap.add_argument("--cross", default="mipsel-linux-gnu-")
    args = ap.parse_args()
    dest = args.output.resolve()
    if args.archive and (args.archive.resolve() == dest or dest in args.archive.resolve().parents):
        ap.error("the archive must be outside the exported SDK directory")
    if args.archive_only:
        if not args.archive or not (dest / "sdk.json").is_file():
            ap.error("--archive-only requires --archive and an exported SDK")
        archive(dest, args.archive)
        print(f"SDK archive: {args.archive}")
        return
    if not args.build_dir or not args.revision:
        ap.error("export requires --build-dir and --revision")
    if dest.exists() and not (dest / "sdk.json").is_file():
        ap.error("refusing to replace a directory without an sdk.json marker")
    dest.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".glaze-sdk-", dir=dest.parent) as tmp:
        stage = Path(tmp) / "glaze-sdk"
        copy(ROOT / "lib/include", stage / "sysroot/usr/include")
        # copytree into an existing include directory needs individual children.
        for path in (ROOT / "lib/libc/include").iterdir():
            copy(path, stage / "sysroot/usr/include" / path.name)
        for name in LIBS:
            copy(args.build_dir / "shared" / name, stage / "sysroot/usr/lib" / name)
        for name in ("crt0.o", "start.o"):
            copy(args.build_dir / "pic" / name, stage / "sysroot/usr/lib" / name)
        copy(ROOT / "sdk/glaze-sdk.py", stage / "bin/glaze-sdk")
        (stage / "bin/glaze-sdk").chmod(0o755)
        copy(ROOT / "sdk/app.mk", stage / "share/app.mk")
        copy(ROOT / "sdk/templates", stage / "share/templates")
        for name in ("mkromfs.py", "mkcfimg.sh", "mkgzi.py"):
            copy(ROOT / "tools" / name, stage / "tools" / name)
        copy(ROOT / "host/glaze_usb", stage / "host/glaze_usb")
        copy(ROOT / "sdk/host.mk", stage / "host/Makefile")
        for name in ("usb_wire.h", "usb_boot.h", "abi.h", "types.h"):
            copy(ROOT / "lib/include/glaze" / name, stage / "host/include/glaze" / name)
        for name in ("SDK.md", "USB.md", "DYNAMIC_LIBRARIES.md"):
            copy(ROOT / "docs" / name, stage / "docs" / name)
        (stage / "README.md").write_text(
            "# Glaze application SDK\n\n"
            "Start with the [SDK guide](docs/SDK.md) for host setup, building,\n"
            "packaging, emulator testing and USB deployment.\n\n"
            "`bin/glaze-sdk new my-app --id my-app --name 'My App'` creates\n"
            "a project. Set `GLAZE_SDK` to this directory before running make.\n"
            "`examples/hello` is a complete starter app.\n\n"
            "Use a Glaze OS image matching the revision in `sdk.json`.\n"
            "Library sources and rebuild rules are in `src/`; see `LICENSE`.\n")
        copy(ROOT / "LICENSE", stage / "LICENSE")
        # Corresponding source and an independent rebuild recipe accompany all
        # Glaze binaries in the SDK. No firmware, games or toolchain binaries.
        for name in ("include", "libc", "gfx", "ui", "media", "ldso", "crt0_pic.S"):
            copy(ROOT / "lib" / name, stage / "src/lib" / name)
        copy(ROOT / "sdk/libraries.mk", stage / "src/Makefile")
        compiler = subprocess.run([args.cross + "gcc", "--version"], text=True, capture_output=True, check=True)
        manifest = {"format": 1, "revision": args.revision, "target": "mipsel-glaze-o32-softfloat",
                    "compiler": compiler.stdout.splitlines()[0], "runtime_libraries": list(LIBS),
                    "runtime_match": "Use Glaze built from the same source revision; no stable SDK ABI is promised."}
        (stage / "sdk.json").write_text(json.dumps(manifest, indent=2) + "\n")
        subprocess.run([str(stage / "bin/glaze-sdk"), "new", str(stage / "examples/hello"),
                        "--id", "sdk-hello", "--name", "SDK Hello"], check=True, capture_output=True)
        sums = []
        for path in sorted(stage.rglob("*")):
            if path.is_file():
                sums.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(stage).as_posix()}\n")
        (stage / "SHA256SUMS").write_text("".join(sums))
        if dest.exists():
            # Move the previous SDK aside only once the new SDK is complete.
            dest.rename(Path(tmp) / "previous")
        stage.rename(dest)
    if args.archive:
        archive(dest, args.archive)
    print(f"SDK: {dest} ({args.revision})")


if __name__ == "__main__":
    main()
