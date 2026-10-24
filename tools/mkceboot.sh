#!/bin/sh
# Makes a CompactFlash image for booting Glaze from Windows CE with
# GlazeBoot (docs/GLAZEBOOT.md): a FAT16 volume with \Glaze holding the
# loader, glaze.cfg, kernel.bin and romfs.img from `make glazeboot`.
#
#   tools/mkceboot.sh out/ceboot.img [size_mb] [key=value ...]
#
# Extra key=value arguments are appended to the card's glaze.cfg (later
# lines win), e.g. confirm=0 or mode=probe for tests.
set -e

out=${1:?usage: $0 image [size_mb] [key=value ...]}
shift
size=16
case "$1" in [0-9]*) size=$1; shift ;; esac
dir=${GB_DIR:-out/glazeboot}

for f in kernel.bin romfs.img glaze.cfg; do
    [ -f "$dir/$f" ] || { echo "$0: no $dir/$f; run make glazeboot" >&2; exit 1; }
done
[ -f "$dir/GlazeBoot.exe" ] ||
    echo "$0: warning: no $dir/GlazeBoot.exe (build $dir/evc/GlazeBoot.vcp with eVC 3.0 and copy it there)" >&2

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir "$stage/Glaze"
cp "$dir/kernel.bin" "$dir/romfs.img" "$stage/Glaze/"
[ -f "$dir/GlazeBoot.exe" ] && cp "$dir/GlazeBoot.exe" "$stage/Glaze/"
cp "$dir/glaze.cfg" "$stage/Glaze/glaze.cfg"
for kv in "$@"; do
    printf '%s\n' "$kv" >> "$stage/Glaze/glaze.cfg"
done

"$(dirname "$0")/mkcfimg.sh" "$out" "$size" 16 "$stage/Glaze"
