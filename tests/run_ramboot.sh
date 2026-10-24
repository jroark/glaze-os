#!/bin/sh
# Boot Glaze from RAM the way GlazeBoot hands over from Windows CE
# (docs/GLAZEBOOT.md), in the emulator: the ramboot SPL scatters the kernel
# and romfs over RAM and runs GlazeBoot's trampoline. The NAND's file-system
# partition holds a FAT16 volume (userdata/Documents), which Glaze mounts as
# /NAND, read/write.
#
# usage: tests/run_ramboot.sh [seconds] [make args...]
#   e.g. tests/run_ramboot.sh 30 INIT=bin/ktest
# Output: out/shots/ramboot/{stdout.log,nand.img} and out/shots/ramboot.png.
# Set BE300 to the emulator binary if it isn't at ../be300/build-host/be300.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
secs=${1:-20}
[ $# -gt 0 ] && shift
out=$root/out/shots/ramboot
mkdir -p "$out"

"$root/tools/mkcfimg.sh" "$root/out/nandfat.img" 10 16 "$root/userdata/Documents" >/dev/null
(cd "$root" && ./build.sh ramboot RAMBOOT_FS=out/nandfat.img "$@" >/dev/null)
cp "$root/out/glaze-ramboot.bin" "$out/nand.img"
cd "$out"
rm -f screenshot_*.bmp
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} \
    gtimeout "${secs}s" "${BE300:-$root/../be300/build-host/be300}" --nand nand.img --rtc-host-time \
    > stdout.log 2> stderr.log || true
ls screenshot_*.bmp >/dev/null 2>&1 && sips -s format png screenshot_*.bmp --out "$out.png" >/dev/null
grep -E "ramboot:|boot: |romfs:|fs: |PANIC|fault|ktest: (FAIL|ALL)" stdout.log || true
