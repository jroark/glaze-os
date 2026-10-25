#!/bin/sh
# Build Glaze with a scripted input file, boot it, and save the final screen.
# usage: tests/run.sh tests/t_x.txt [seconds]
# CF=<image> boots with a copy of that CompactFlash image (tools/mkcfimg.sh);
# NET=1 inserts NE2000; XI825=1 inserts the XI-825 Wi-Fi card.
# NO_BUILD=1 reuses the existing image; BE300 overrides the emulator path.
# FS_FROM=<nand image> boots with that image's file system (from 0x500000),
# e.g. a previous run's out/shots/<name>/glaze.bin with apps installed.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
# Resolve the sibling emulator relative to the original checkout, including
# when this script runs inside a /tmp worktree.
common=$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)
checkout=$(dirname "$common")
be300=${BE300:-$(dirname "$checkout")/be300/build-host/be300}
script=$1
secs=${2:-14}
name=$(basename "$script" .txt)
out=$root/out/shots
mkdir -p "$out" "$out/$name"
if [ -z "$NO_BUILD" ]; then
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
if [ -n "$XI825" ]; then
    if [ -n "$CF$NET" ]; then echo "XI825 cannot be combined with CF or NET" >&2; exit 1; fi
    cf=--xi825
elif [ -n "$CF" ]; then
    case $CF in /*) ;; *) CF=$root/$CF ;; esac
    cp "$CF" cf.img
    cf="--cf cf.img"
elif [ -n "$NET" ]; then
    cf=--ne2000
fi
gtimeout -k 5s "${secs}s" "$be300" --nand glaze.bin --rtc-host-time $cf \
    > stdout.log 2> stderr.log || true
sips -s format png screenshot_*.bmp --out "$out/$name.png" >/dev/null
grep -E "fault|PANIC|exception" stdout.log || true
echo "$out/$name.png"
