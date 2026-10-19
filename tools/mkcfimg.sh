#!/bin/sh
# Makes a CompactFlash image for the emulator's --cf: a FAT volume with no
# partition table (a "superfloppy"), holding the given files and folders.
#
#   tools/mkcfimg.sh out/cf.img [size_mb [fat_bits]] [file_or_dir ...]
#
# Defaults: 16 MiB, FAT16, and userdata/Documents and userdata/Music.
# Needs mkfs.fat (dosfstools) and mcopy (mtools).
set -e

out=${1:?usage: $0 image [size_mb [fat_bits]] [file_or_dir ...]}
shift
size=16
bits=16
case "$1" in [0-9]*) size=$1; shift ;; esac
case "$1" in 12|16|32) bits=$1; shift ;; esac
[ $# -gt 0 ] || set -- userdata/Documents userdata/Music

mkdir -p "$(dirname "$out")"
rm -f "$out"
mkfs.fat -F "$bits" -n GLAZECF -C "$out" $((size * 1024)) >/dev/null
for f in "$@"; do
    mcopy -s -i "$out" "$f" ::/
done
mdir -/ -b -i "$out" ::
