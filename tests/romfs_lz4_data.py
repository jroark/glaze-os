#!/usr/bin/env python3
"""Writes romfs-lz4-test's inputs: varied files in DIR/f, then DIR/plain.img
and DIR/lz4.img from tools/mkromfs.py (plain and --lz4).

    python3 tests/romfs_lz4_data.py out/host/romfs-lz4
"""
import random
import subprocess
import sys
from pathlib import Path

out = Path(sys.argv[1])
src = out / "f"
src.mkdir(parents=True, exist_ok=True)
r = random.Random(7)
files = {
    "random": bytes(r.randrange(256) for _ in range(10000)),      # incompressible: raw pages
    "zeros": bytes(8192),
    "text": b"".join(b"line %d of a repetitive text file\n" % i for i in range(900)),
    "page": bytes(r.randrange(256) for _ in range(4096)),
    "tiny": b"tiny!!!",
    "empty": b"",
    "mixed": bytes(20000) + bytes(r.randrange(4) for _ in range(30000)),
}
for name, data in files.items():
    (src / name).write_bytes(data)
tool = Path(__file__).resolve().parents[1] / "tools/mkromfs.py"
args = [f"f/{name}=f/{name}" for name in sorted(files)]
subprocess.run([sys.executable, "-I", str(tool), "plain.img", *args], cwd=out, check=True)
subprocess.run([sys.executable, "-I", str(tool), "--lz4", "lz4.img", *args], cwd=out, check=True)
