#!/bin/sh
# Makes a CompactFlash image for booting Glaze from Windows CE with
# GlazeBoot (docs/GLAZEBOOT.md): a FAT16 volume with \Glaze holding the
# loader, glaze.cfg, kernel.bin and romfs.img from `make glazeboot` and
# `make glazeboot-exe`.
#
#   tools/mkceboot.sh [--autorun] out/ceboot.img [size_mb] [key=value ...]
#
# --autorun puts them in \CE\R4100 instead, with the loader as AutoRun.exe,
# which Windows CE starts when the card is inserted or present at boot.
# Extra key=value arguments are appended to the card's glaze.cfg (later
# lines win), e.g. confirm=0 or mode=probe for tests.
set -e

folder=Glaze
exe=GlazeBoot.exe
if [ "$1" = --autorun ]; then
    folder=CE/R4100
    exe=AutoRun.exe
    shift
fi
out=${1:?usage: $0 [--autorun] image [size_mb] [key=value ...]}
shift
size=16
case "$1" in [0-9]*) size=$1; shift ;; esac
dir=${GB_DIR:-out/glazeboot}

for f in kernel.bin romfs.img glaze.cfg; do
    [ -f "$dir/$f" ] || { echo "$0: no $dir/$f; run make glazeboot" >&2; exit 1; }
done
[ -f "$dir/GlazeBoot.exe" ] ||
    echo "$0: warning: no $dir/GlazeBoot.exe (run make glazeboot-exe)" >&2

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$folder"
cp "$dir/kernel.bin" "$dir/romfs.img" "$stage/$folder/"
[ -f "$dir/GlazeBoot.exe" ] && cp "$dir/GlazeBoot.exe" "$stage/$folder/$exe"
cp "$dir/glaze.cfg" "$stage/$folder/glaze.cfg"
# 32 bytes from the host's CSPRNG: the Wi-Fi supplicant's random seed in a
# RAM boot, ratcheted on the card at every boot (servers/net/netif.c).
head -c 32 /dev/urandom > "$stage/$folder/glaze.seed"
for kv in "$@"; do
    printf '%s\n' "$kv" >> "$stage/$folder/glaze.cfg"
done

"$(dirname "$0")/mkcfimg.sh" "$out" "$size" 16 "$stage/${folder%%/*}"
