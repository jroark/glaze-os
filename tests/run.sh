#!/bin/sh
# Build Glaze with a scripted input file, boot it, and save the final screen.
# usage: tests/run.sh tests/t_x.txt [seconds]
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
gtimeout "${secs}s" "${BE300:-../../../../be300/build-host/be300}" --nand glaze.bin --rtc-host-time \
    > stdout.log 2> stderr.log || true
sips -s format png screenshot_*.bmp --out "$out/$name.png" >/dev/null
grep -E "fault|PANIC|exception" stdout.log || true
echo "$out/$name.png"
