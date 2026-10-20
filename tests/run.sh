#!/bin/sh
# Build Glaze with a scripted input file, boot it, and save the final screen.
# usage: tests/run.sh tests/t_x.txt [seconds]
# CF=<image> boots with a copy of that CompactFlash image (tools/mkcfimg.sh);
# NET=1 inserts the NE2000 Ethernet card instead.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
script=$1
secs=${2:-14}
name=$(basename "$script" .txt)
out=$root/out/shots
mkdir -p "$out" "$out/$name"
(cd "$root" && ./build.sh INPUT_SCRIPT="$script" >/dev/null)
cp "$root/out/glaze.bin" "$out/$name/glaze.bin"
cd "$out/$name"
rm -f screenshot_*.bmp
cf=
if [ -n "$CF" ]; then
    case $CF in /*) ;; *) CF=$root/$CF ;; esac
    cp "$CF" cf.img
    cf="--cf cf.img"
elif [ -n "$NET" ]; then
    cf=--ne2000
fi
gtimeout "${secs}s" "${BE300:-../../../../be300/build-host/be300}" --nand glaze.bin --rtc-host-time $cf \
    > stdout.log 2> stderr.log || true
sips -s format png screenshot_*.bmp --out "$out/$name.png" >/dev/null
grep -E "fault|PANIC|exception" stdout.log || true
echo "$out/$name.png"
