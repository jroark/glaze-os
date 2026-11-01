#!/usr/bin/env python3
"""OTA update end to end in the emulator (docs/os/OTA.md).

Builds a ktest image (INIT=bin/ktest) that trusts a throwaway key, and an
update to "1.1" with the same kernel and a different romfs (NAND_DROP adds
calculator, so nearly every romfs block moves). The update goes into the
factory volume of a physical NAND image (tools/physical_nand.py, from a
synthetic backup), where ktest verifies it through the file server and arms
it; the emulator then resets, the SPL installs it, and ktest runs again on
the new romfs. Then the same from the armed state with the emulator stopped
at points during the SPL's install, each restart finishing it.

Never uses the release key or touches a device.

usage: tests/run_ota.py [--emulator PATH] [--cuts N] [--no-build]
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import mkota  # noqa: E402
import physical_nand as phys  # noqa: E402

ART = ROOT / "out/ota-test"
INSTALLED = "ota: the SPL installed 1.1"


def build(*args):
    subprocess.run([str(ROOT / "build.sh"), "INIT=bin/ktest", "OTA_PUBKEY=out/ota-test/pub.bin", *args],
                   check=True, stdout=subprocess.DEVNULL)


def synthetic_backup(path):
    """A CE-format backup with three bad blocks, as tests/run_physical_nand.py makes."""
    raw = bytearray(b"\xff" * phys.RAW_SIZE)
    bad, pages, where = {1, 321, 350}, [], []
    for block in range(1024):
        if block in bad:
            for page in (0, 1):
                raw[(block * 32 + page) * 528 + 517] = 0
        else:
            for page in range(5):
                pages.append(b"\xff" * 512 + b"\xaa\x55\x0f\xff" + block.to_bytes(4, "little"))
                where.append(block * 32 + page)
    encoded = phys.encode(pages)
    for i, page in enumerate(where):
        raw[page * 528:(page + 1) * 528] = encoded[i * 528:(i + 1) * 528]
    path.write_bytes(raw)


def boot(emulator, image, folder, until, timeout, stop_after=None, after_text=None):
    """Boots image until `until` appears (or, with stop_after, that many
    seconds after `after_text`), then stops the emulator, which writes the
    NAND back. Returns the log."""
    folder.mkdir(parents=True, exist_ok=True)
    log = folder / "stdout.log"
    with log.open("wb") as out, (folder / "stderr.log").open("wb") as err:
        p = subprocess.Popen([str(emulator), "--nand", str(image), "--rtc-host-time"], cwd=folder,
                             stdout=out, stderr=err, env={**os.environ, "SDL_VIDEODRIVER": "dummy"})
        end, seen = time.monotonic() + timeout, None
        while p.poll() is None and time.monotonic() < end:
            text = log.read_text(errors="replace")
            if stop_after is not None:
                if seen is None and after_text in text:
                    seen = time.monotonic()
                if seen is not None and time.monotonic() - seen >= stop_after:
                    break
            elif until in text:
                break
            time.sleep(0.05)
        if p.poll() is None:
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
    return log.read_text(errors="replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emulator", type=Path, default=ROOT.parent / "be300/build-host/be300")
    ap.add_argument("--cuts", type=int, default=4, help="interrupted installs to try")
    ap.add_argument("--timeout", type=float, default=240)
    ap.add_argument("--no-build", action="store_true", help="reuse out/ota-test's images")
    a = ap.parse_args()
    emulator = a.emulator.resolve()

    if not a.no_build:
        shutil.rmtree(ART, ignore_errors=True)
        ART.mkdir(parents=True)
        key = ART / "key.pem"
        subprocess.run([mkota.openssl(), "genpkey", "-algorithm", "ed25519", "-out", str(key)], check=True)
        (ART / "pub.bin").write_bytes(mkota.public_key(key))
        build("NAND_DROP=wince calculator", "out/romfs.img")
        shutil.copy(ROOT / "out/romfs.img", ART / "new-romfs.img")
        build("NAND_DROP=wince")
        for name in ("glaze.bin", "kernel.elf", "romfs.img"):
            shutil.copy(ROOT / "out" / name, ART / f"old-{name}")
        update = mkota.build_update((ART / "old-kernel.elf").read_bytes(), (ART / "new-romfs.img").read_bytes(),
                                    "1.1", "ota-test", key)
        userdata = ART / "userdata"
        shutil.copytree(ROOT / "userdata", userdata)
        (userdata / "Library/Updates").mkdir(parents=True, exist_ok=True)
        (userdata / "Library/Updates/ktest.gzu").write_bytes(update)
        synthetic_backup(ART / "backup.raw")
        (ART / "inventory.json").write_text(json.dumps(phys.inspect(ART / "backup.raw"), indent=2))
        subprocess.run([sys.executable, str(ROOT / "tools/physical_nand.py"), "pack",
                        "--logical", str(ART / "old-glaze.bin"), "--backup", str(ART / "backup.raw"),
                        "--inventory", str(ART / "inventory.json"), "--userdata", str(userdata),
                        "--out", str(ART / "candidate.raw"), "--plan", str(ART / "plan.json")], check=True)

    # Straight through: verify, arm, reset, install, ktest again.
    run = ART / "straight"
    shutil.rmtree(run, ignore_errors=True)
    run.mkdir()
    shutil.copy(ART / "candidate.raw", run / "nand.raw")
    t0 = time.monotonic()
    log = boot(emulator, run / "nand.raw", run, "ktest: ota installed 1.1", a.timeout)
    for want in ("ktest: PASS ota verified", "ktest: arming the OTA update", INSTALLED,
                 "ktest: ota installed 1.1"):
        if want not in log:
            sys.exit(f"run_ota: straight run: missing {want!r} (see {run}/stdout.log)")
    files = [l for l in log.splitlines() if l.startswith("romfs: ")]
    if len(files) < 2 or files[0] == files[-1]:
        sys.exit(f"run_ota: the romfs didn't change: {files}")
    log = boot(emulator, run / "nand.raw", run / "again", "ktest: ALL PASS", a.timeout)
    if "ktest: ALL PASS" not in log or INSTALLED in log:
        sys.exit(f"run_ota: the installed image didn't boot cleanly (see {run}/again/stdout.log)")
    print(f"run_ota: installed and rebooted ({files[0]} -> {files[-1]}) in {time.monotonic() - t0:.0f} s")

    # The armed state: stop as soon as ktest arms, before the SPL starts.
    armed = ART / "armed"
    shutil.rmtree(armed, ignore_errors=True)
    armed.mkdir()
    shutil.copy(ART / "candidate.raw", armed / "nand.raw")
    boot(emulator, armed / "nand.raw", armed, None, a.timeout, stop_after=0.0,
         after_text="ota: journal for 1.1")
    # How long the install takes: from the journal to the next kernel's banner.
    timing = ART / "timing"
    shutil.rmtree(timing, ignore_errors=True)
    timing.mkdir()
    shutil.copy(armed / "nand.raw", timing / "nand.raw")
    t0 = time.monotonic()
    log = boot(emulator, timing / "nand.raw", timing, INSTALLED, a.timeout)
    if INSTALLED not in log:
        sys.exit(f"run_ota: no install from the armed image (see {timing}/stdout.log)")
    span = time.monotonic() - t0
    full = int(re.search(r"installed 1\.1 \((\d+) blocks", log).group(1))
    print(f"run_ota: an uninterrupted install takes {span:.0f} s and writes {full} blocks")
    # Spread over the install: the first half checks every block, the rest writes.
    partial = 0
    for i in range(a.cuts):
        stop = span * (i + 1) / (a.cuts + 1)
        cut = ART / f"cut{i}"
        shutil.rmtree(cut, ignore_errors=True)
        cut.mkdir()
        shutil.copy(armed / "nand.raw", cut / "nand.raw")
        log = boot(emulator, cut / "nand.raw", cut, None, a.timeout, stop_after=stop, after_text="")
        if INSTALLED in log:
            sys.exit(f"run_ota: the install finished before the stop at {stop:.1f} s")
        log = boot(emulator, cut / "nand.raw", cut / "resume", "ktest: ota installed 1.1", a.timeout)
        found = re.search(r"installed 1\.1 \((\d+) blocks", log)
        if "ktest: ota installed 1.1" not in log or not found:
            sys.exit(f"run_ota: stopped {stop:.1f} s in, the install didn't resume (see {cut}/resume)")
        written = int(found.group(1))
        partial += written < full
        print(f"run_ota: stopped {stop:.1f} s into the install; the next boot finished it, writing {written} blocks")
    if a.cuts >= 3 and not partial:
        sys.exit("run_ota: no stop landed while blocks were being written")
    print("run_ota: PASS")


if __name__ == "__main__":
    main()
