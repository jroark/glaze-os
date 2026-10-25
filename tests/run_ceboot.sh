#!/bin/sh
# Boot Glaze through GlazeBoot from the emulator's stock Windows CE
# (docs/GLAZEBOOT.md): builds the loader with be300-dev, puts it on a CF
# card as \CE\R4100\AutoRun.exe with confirm=0, and boots a copy of the
# Windows CE restore image with the card inserted.
#
# usage: tests/run_ceboot.sh [seconds] [key=value ...] [make args...]
#   e.g. tests/run_ceboot.sh 60 mode=probe
#        tests/run_ceboot.sh 150 INIT=bin/ktest
# key=value arguments whose key is a glaze.cfg key (mode, ramfs_kb, nand,
# confirm, countdown) go to the card's glaze.cfg; the rest go to make.
# Output: out/shots/ceboot/{stdout.log,nand.img,cf.img,glazeboot.log} and
# out/shots/ceboot.png. Set BE300 to the emulator binary and BE300_NAND to
# the Windows CE image if they aren't under ../be300.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
secs=${1:-60}
[ $# -gt 0 ] && shift
cfg="confirm=0 nand=ro"
mk=
for a in "$@"; do
    case "$a" in
        mode=*|ramfs_kb=*|nand=*|confirm=*|countdown=*) cfg="$cfg $a" ;;
        *) mk="$mk $a" ;;
    esac
done
emu=$root/../be300
out=$root/out/shots/ceboot
mkdir -p "$out"

(cd "$root" && ./build.sh glazeboot $mk >/dev/null && make -s glazeboot-exe >/dev/null)
"$root/tools/mkceboot.sh" --autorun "$out/cf.img" 16 $cfg >/dev/null
cp "${BE300_NAND:-$emu/ce/restore_images/All_nand_300.bin}" "$out/nand.img"
cd "$out"
rm -f screenshot_*.bmp glazeboot.log
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} \
    gtimeout "${secs}s" "${BE300:-$emu/build-host/be300}" --nand nand.img --rtc-host-time --cf cf.img \
    > stdout.log 2> stderr.log || true
ls screenshot_*.bmp >/dev/null 2>&1 && sips -s format png screenshot_*.bmp --out "$out.png" >/dev/null
MTOOLS_SKIP_CHECK=1 mtype -i cf.img ::/CE/R4100/glazeboot.log 2>/dev/null | tr -d '\r' > glazeboot.log || true
cat glazeboot.log
grep -E "ramboot:|boot: |romfs:|fs: |PANIC|fault|ktest: (FAIL|ALL)" stdout.log || true
