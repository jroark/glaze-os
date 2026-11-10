#!/usr/bin/env python3
"""Boot, memory and launch benchmarks for stock Windows CE in the emulator,
the counterpart of tests/bench.py's for Glaze.

usage: tests/bench_ce.py [LABEL] [--runs N] [--be300 PATH] [--nand IMAGE]

Puts wince/ce_bench (CeBench.exe) on a CF card as \\CE\\R4100\\AutoRun.exe,
cold boots a fresh copy of the Windows CE restore image N times with it
(BE300_NAND or --nand, default ../be300/ce/restore_images/All_nand_300.bin),
taps through the setup wizard with --touch-script, and writes
out/bench/LABEL.txt (default wince) in bench.py's format:

  boot      host seconds from emulator start to "CASIO Original Shell
            started", leaving out the extra pass of the watchdog reset the
            emulated cold boot makes first (see below)
  memory    free program memory once the wizard is done, at the shell after
            each app (still running in the background), and how CE divides
            RAM between programs and the object store
  launch    ms from CreateProcess until the app's window is in front and its
            message loop answers, cold (first launch) and warm (launched
            again while running in the background, which brings back the
            same window)

The emulated cold boot is a battery pull: Windows CE starts, is reset once by
the watchdog ("System was rebooted by WDT") and then shows its setup wizard,
screen alignment first. The reset's extra pass is subtracted from the boot
time and counted in boot.resets. The wizard's taps are on emulated time.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench import OUT, ROOT, boot, med  # noqa: E402

APPS = ["calculator", "notes", "calendar", "clock", "settings", "browser"]

# The setup wizard on a cold boot, in emulated ms and LCD pixels: the five
# screen-alignment targets (centre, then 10% and 80% across and 20% and 80%
# down, as Windows CE places them), each held 1 s, as a short tap isn't
# taken; then OK on the "Restart" notice the watchdog reset leaves, and OK
# on the Home city and time page.
WIZARD = [(20000, 120, 160, 1000), (23000, 24, 64, 1000), (26000, 24, 256, 1000),
          (29000, 216, 256, 1000), (32000, 216, 64, 1000),
          (37000, 186, 69, 300), (40000, 228, 13, 300)]


def card(work):
    exe = os.path.join(ROOT, "out", "glazeboot", "CeBench.exe")
    subprocess.run(["make", "ce-bench-exe", "WL54CF=0"], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
    img = os.path.join(work, "cf-clean.img")
    subprocess.run([os.path.join(ROOT, "tools", "mkcfimg.sh"), img, "16", "16"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["mmd", "-i", img, "::CE", "::CE/R4100"], check=True)
    subprocess.run(["mcopy", "-i", img, exe, "::CE/R4100/AutoRun.exe"], check=True)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("label", nargs="?", default="wince")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--be300", default=os.path.join(ROOT, "..", "be300", "build-host", "be300"))
    ap.add_argument("--nand", default=os.environ.get("BE300_NAND", os.path.join(
        ROOT, "..", "be300", "ce", "restore_images", "All_nand_300.bin")))
    a = ap.parse_args()

    work = os.path.join(OUT, "ce")
    os.makedirs(work, exist_ok=True)
    clean = card(work)
    touch = os.path.join(work, "wizard.txt")
    with open(touch, "w") as f:
        f.write("# tests/bench_ce.py: the cold boot's setup wizard\n")
        for ms, x, y, hold in WIZARD:
            f.write(f"{ms} down {x} {y}\n{ms + hold} up\n")

    boot_s, resets, idle, home, end = [], [], [], [], []
    programs = store = phys = None
    cold = {app: [] for app in APPS}
    warm = {app: [] for app in APPS}
    for r in range(a.runs):
        cf = os.path.join(work, "cf.img")
        shutil.copyfile(clean, cf)
        banners, shell, done = [], None, False
        log = []
        for t, line in boot(a.be300, a.nand, 300, until=["cebench: done", "giving up"],
                            args=("--cf", cf, "--touch-script", touch), work=work):
            log.append(f"{t:7.2f} {line}")
            if "Windows CE Kernel for MIPS" in line:
                banners.append(t)
            elif "CASIO Original Shell started" in line and shell is None:
                shell = t
            elif m := re.match(r"cebench: mem (\w+) phys (\d+) KiB free (\d+) KiB programs (\d+) KiB "
                               r"store (\d+) KiB", line):
                free = int(m[3])
                phys, programs, store = int(m[2]), int(m[4]), int(m[5])
                {"idle": idle, "home": home, "end": end}.get(m[1], []).append(free)
            elif m := re.match(r"cebench: launch (\S+) (cold|warm) (\d+) ms", line):
                (cold if m[2] == "cold" else warm)[m[1]].append(int(m[3]))
            elif "cebench: done" in line:
                done = True
            elif "cebench:" in line and ("failed" in line or "can't" in line or "giving up" in line):
                print(line, file=sys.stderr)
        with open(os.path.join(work, f"run{r + 1}.log"), "w") as f:
            f.write("\n".join(log) + "\n")
        if not done or shell is None or not banners:
            sys.exit(f"run {r + 1}: Windows CE didn't finish (see {work}/run{r + 1}.log)")
        boot_s.append(banners[0] + shell - banners[-1])
        resets.append(len(banners) - 1)
        print(f"run {r + 1}/{a.runs} done", file=sys.stderr)

    out = [f"# {a.label}: Windows CE, medians of {a.runs} runs",
           f"boot.host_s       {med(boot_s):.2f}",
           f"boot.resets       {med(resets):.0f}",
           f"mem.physical      {phys} KiB (programs {programs} KiB, object store {store} KiB)",
           f"mem.idle          {med(idle):.0f} KiB",
           f"mem.home_min      {min(home, default=float('nan'))} KiB",
           f"mem.home_median   {med(home):.0f} KiB",
           f"mem.end           {med(end):.0f} KiB"]
    for app in APPS:
        out.append(f"launch.{app:9s} cold {med(cold[app]):6.0f} ms  warm {med(warm[app]):6.0f} ms")
    text = "\n".join(out) + "\n"
    with open(os.path.join(OUT, a.label + ".txt"), "w") as f:
        f.write(text)
    print(text)


if __name__ == "__main__":
    main()
