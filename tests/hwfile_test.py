#!/usr/bin/env python3
"""Hardware packages (glaze/hwpkg.h): builds packages with tools/mkhwpkg.py
and a throwaway Ed25519 key, then runs kernel/hwfile.c natively over them
(tests/native/hwfile.c). Needs OpenSSL 3, so run it natively, not in the
container:

  make -C host hwfile-test
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import mkota  # noqa: E402

OS = "9.0"      # the GLAZE_VERSION host/Makefile builds the test with


def mkhwpkg(*args):
    subprocess.run([sys.executable, str(ROOT / "tools/mkhwpkg.py"), *map(str, args)], check=True,
                   stdout=subprocess.DEVNULL)


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        for name in ("key", "other"):
            subprocess.run([mkota.openssl(), "genpkey", "-algorithm", "ed25519", "-out", str(t / f"{name}.pem")],
                           check=True, stdout=subprocess.DEVNULL)
        (t / "pub.bin").write_bytes(mkota.public_key(t / "key.pem"))
        (t / "a.ko").write_bytes(os.urandom(4000))
        (t / "net").write_bytes(os.urandom(9000))
        (t / "fw.bin").write_bytes(os.urandom(700))
        common = ["--version", "1.0", "--name", "Test"]
        mkhwpkg(*common, "--os", OS, "--key", t / "key.pem", "--id", "hw-test", "--out", t / "good.gzp",
                f"drivers/a.ko={t / 'a.ko'}", f"bin/net={t / 'net'}")
        mkhwpkg(*common, "--os", OS, "--unsigned", "--id", "hw-fw", "--out", t / "fw.gzp",
                f"firmware/fw.bin={t / 'fw.bin'}")
        mkhwpkg(*common, "--os", OS, "--key", t / "other.pem", "--id", "hw-wrong", "--out", t / "wrong.gzp",
                f"drivers/b.ko={t / 'a.ko'}")
        mkhwpkg(*common, "--os", "8.0", "--key", t / "key.pem", "--id", "hw-old", "--out", t / "old.gzp",
                f"drivers/c.ko={t / 'a.ko'}")
        mkhwpkg(*common, "--os", OS, "--key", t / "key.pem", "--id", "hw-dup", "--out", t / "dup.gzp",
                f"drivers/a.ko={t / 'a.ko'}")
        # mkhwpkg.py's own checks, and verify against the right and wrong keys.
        for bad in (["--unsigned", f"drivers/a.ko={t / 'a.ko'}"], ["--key", t / "key.pem", f"lib/x={t / 'a.ko'}"],
                    ["--key", t / "key.pem", f"drivers/x={t / 'a.ko'}"]):
            r = subprocess.run([sys.executable, str(ROOT / "tools/mkhwpkg.py"), *common, "--os", OS, "--id", "hw-x",
                                "--out", str(t / "x.gzp"), *map(str, bad)], capture_output=True)
            assert r.returncode, bad
        verify = [sys.executable, str(ROOT / "tools/mkhwpkg.py"), "verify", "--pub", str(t / "pub.bin")]
        assert subprocess.run(verify + [str(t / "good.gzp")], capture_output=True).returncode == 0
        assert subprocess.run(verify + [str(t / "fw.gzp")], capture_output=True).returncode == 0
        assert subprocess.run(verify + [str(t / "wrong.gzp")], capture_output=True).returncode != 0
        subprocess.run([binary, t / "pub.bin", t / "good.gzp", t / "fw.gzp", t / "wrong.gzp", t / "old.gzp",
                        t / "dup.gzp"], check=True)
        print("mkhwpkg: signing, path checks and verify PASS")


if __name__ == "__main__":
    main()
