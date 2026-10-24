#!/usr/bin/env python3
"""Boot and memory benchmarks in the emulator, headless.

usage: tests/bench.py LABEL [--runs N] [--be300 PATH]

Builds two images in out/bench/ (a launch script and ktest), boots each N
times and writes out/bench/LABEL.txt with the medians:

  boot      host seconds from emulator start to SpringBoard ready (includes
            the SPL loading the kernel), and the guest ms at that point
  memory    free KiB after the kernel (the mm: line), at the lock screen, at
            the home screen after each app, and the romfs page cache
  launch    ms from tap to an app's first frame, cold (first launch since
            boot) and warm (second launch)
  ktest     total run time, file-system throughput and free memory at the end

SpringBoard and ktest print the "sb: ..." and "ktest: done ..." lines this
reads. Times are guest time except boot's host seconds, so they don't depend
much on the host, but run it on an idle machine.
"""

import argparse
import os
import re
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "out", "bench")

# Home-screen taps (screen coordinates) for the apps measured.
APPS = [
    ("bin/settings", 150, 285),
    ("bin/notes", 90, 285),
    ("bin/photos", 90, 48),
    ("bin/browser", 90, 120),
    ("bin/music", 150, 120),
    ("bin/calendar", 30, 48),
]
APP_SECS = 18          # per app: cold launch, Home, warm launch, Home


def launch_script():
    lines = ["# tests/bench.py: unlock, then launch each app twice.", "3000 drag 40 280 235 280 500"]
    t = 5000
    for _, x, y in APPS:
        lines += [f"{t} tap {x} {y}", f"{t + 7000} key home",
                  f"{t + 9000} tap {x} {y}", f"{t + 15000} key home"]
        t += APP_SECS * 1000
    return "\n".join(lines) + "\n", t / 1000 + 4


def build(script):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, "launch.txt")
    with open(path, "w") as f:
        f.write(script)
    jobs = ["-j8"]
    run = lambda *a: subprocess.run(["make", *jobs, *a], cwd=ROOT, check=True,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run(f"OUT=out/bench/ui", f"INPUT_SCRIPT={path}")
    run("OUT=out/bench/kt", "INIT=bin/ktest")


def boot(be300, image, secs, until=None):
    """Boots a copy of image; returns [(host_seconds, line)]."""
    work = os.path.join(OUT, "run")
    os.makedirs(work, exist_ok=True)
    nand = os.path.join(work, "glaze.bin")
    with open(image, "rb") as src, open(nand, "wb") as dst:
        dst.write(src.read())
    env = dict(os.environ, SDL_VIDEODRIVER="dummy")
    t0 = time.monotonic()
    p = subprocess.Popen([be300, "--nand", nand, "--rtc-host-time"], cwd=work, env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, errors="replace")
    lines = []
    try:
        for line in p.stdout:
            now = time.monotonic() - t0
            lines.append((now, line.rstrip()))
            if (until and until in line) or now > secs:
                break
    finally:
        p.terminate()
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            p.kill()
    return lines


def med(xs):
    return statistics.median(xs) if xs else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("label")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--be300", default=os.path.join(ROOT, "..", "be300", "build-host", "be300"))
    a = ap.parse_args()

    script, secs = launch_script()
    build(script)
    boot_s, boot_ms, mm_free, lock_free, home_free, cache = [], [], [], [], [], []
    cold = {p: [] for p, _, _ in APPS}
    warm = {p: [] for p, _, _ in APPS}
    kt_ms, kt_free, kt_cache, kt_write, kt_read = [], [], [], [], []

    for r in range(a.runs):
        seen = set()
        for t, line in boot(a.be300, os.path.join(OUT, "ui", "glaze.bin"), secs):
            if m := re.match(r"mm: .*, (\d+) KiB free", line):
                mm_free.append(int(m[1]))
            elif m := re.match(r"sb: ready (\d+) ms free (\d+) KiB", line):
                boot_s.append(t)
                boot_ms.append(int(m[1]))
                lock_free.append(int(m[2]))
            elif m := re.match(r"sb: launch (\S+) (\d+) ms", line):
                (warm if m[1] in seen else cold)[m[1]].append(int(m[2]))
                seen.add(m[1])
            elif m := re.match(r"sb: home .* free (\d+) KiB cache (\d+) KiB", line):
                home_free.append(int(m[1]))
                cache.append(int(m[2]))
        for _, line in boot(a.be300, os.path.join(OUT, "kt", "glaze.bin"), 400, until="ktest: ALL PASS"):
            if m := re.match(r"ktest: done in (\d+) ms, (\d+) KiB free, (\d+) KiB cached", line):
                kt_ms.append(int(m[1]))
                kt_free.append(int(m[2]))
                kt_cache.append(int(m[3]))
            elif m := re.match(r"ktest: fs wrote \d+ bytes in (\d+) ms, read in (\d+) ms", line):
                kt_write.append(int(m[1]))
                kt_read.append(int(m[2]))
            elif "ktest: FAIL" in line:
                print(line, file=sys.stderr)
        print(f"run {r + 1}/{a.runs} done", file=sys.stderr)

    out = [f"# {a.label}: medians of {a.runs} runs",
           f"boot.host_s       {med(boot_s):.2f}",
           f"boot.guest_ms     {med(boot_ms):.0f}",
           f"mem.after_kernel  {med(mm_free):.0f} KiB",
           f"mem.lock_screen   {med(lock_free):.0f} KiB",
           f"mem.home_min      {min(home_free, default=float('nan'))} KiB",
           f"mem.home_median   {med(home_free):.0f} KiB",
           f"mem.cache_max     {max(cache, default=0)} KiB"]
    for p, _, _ in APPS:
        out.append(f"launch.{p.split('/')[-1]:9s} cold {med(cold[p]):6.0f} ms  warm {med(warm[p]):6.0f} ms")
    out += [f"ktest.total_ms    {med(kt_ms):.0f}",
            f"ktest.fs_write_ms {med(kt_write):.0f} (20000 bytes)",
            f"ktest.fs_read_ms  {med(kt_read):.0f}",
            f"ktest.free_end    {med(kt_free):.0f} KiB",
            f"ktest.cache_end   {med(kt_cache):.0f} KiB"]
    text = "\n".join(out) + "\n"
    with open(os.path.join(OUT, a.label + ".txt"), "w") as f:
        f.write(text)
    print(text)


if __name__ == "__main__":
    main()
