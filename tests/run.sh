#!/bin/sh
# Build Glaze with a scripted input file, boot it, and save the final screen.
# usage: tests/run.sh tests/t_x.txt [seconds]
# CF=<image> boots with a copy of that CompactFlash image (tools/mkcfimg.sh);
# NET=1 (or ne2000) inserts Ethernet; NET=wl54cf or xi825 selects Wi-Fi.
# FS_FROM=<nand image> boots with that image's file system (from 0x500000),
# e.g. a previous run's out/shots/<name>/glaze.bin with apps installed.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
script=$1
secs=${2:-14}
name=$(basename "$script" .txt)
out=$root/out/shots
mkdir -p "$out" "$out/$name"
if [ "$NET" = wl54cf ]; then
    (cd "$root" && ./build.sh INPUT_SCRIPT="$script" WL54CF=1 >/dev/null)
else
    (cd "$root" && ./build.sh INPUT_SCRIPT="$script" >/dev/null)
fi
cp "$root/out/glaze.bin" "$out/$name/glaze.bin"
if [ -n "$FS_FROM" ]; then
    case $FS_FROM in /*) ;; *) FS_FROM=$root/$FS_FROM ;; esac
    python3 -c 'import sys
src = open(sys.argv[1], "rb").read()
dst = bytearray(open(sys.argv[2], "rb").read())
dst[0x500000:] = src[0x500000:len(dst)]
open(sys.argv[2], "wb").write(dst)' "$FS_FROM" "$out/$name/glaze.bin"
fi
cd "$out/$name"
rm -f screenshot_*.bmp
cf=
if [ -n "$CF" ]; then
    case $CF in /*) ;; *) CF=$root/$CF ;; esac
    cp "$CF" cf.img
    cf="--cf cf.img"
elif [ -n "$NET" ]; then
    case "$NET" in
        1|ne2000) cf=--ne2000 ;;
        wl54cf) cf=--wl54cf ;;
        xi825) cf=--xi825 ;;
        *) echo "unknown NET=$NET" >&2; exit 1 ;;
    esac
fi
gtimeout "${secs}s" "${BE300:-../../../../be300/build-host/be300}" --nand glaze.bin --rtc-host-time $cf \
    > stdout.log 2> stderr.log || true
sips -s format png screenshot_*.bmp --out "$out/$name.png" >/dev/null
grep -E "fault|PANIC|exception" stdout.log || true
echo "$out/$name.png"
