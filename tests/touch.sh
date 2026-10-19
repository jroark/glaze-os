#!/bin/bash
# Drive real touch input through the emulator's PIU (unlike INPUT_SCRIPT,
# which bypasses the touch panel and its interrupts). Runs the emulator's SDL
# window on an X display (start one with `Xvfb :99 &`) and moves the pointer
# with xdotool.
# usage: DISPLAY=:99 tests/touch.sh tests/touch_unlock.act
#
# Action file lines (window coordinates):
#   sleep <s> | tap <x> <y> | drag <x0> <y0> <x1> <y1> [steps] [step_s]
#   shot <name>   (saves out/touch/<test>/<name>.png, needs ImageMagick)
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
acts=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=$root/out/touch/$(basename "$1" .act)
mkdir -p "$dir"
cp "$root/out/glaze.bin" "$dir/glaze.bin"
cd "$dir"
"${BE300:-$root/../be300/build-host/be300}" --nand glaze.bin --rtc-host-time --scale 1 \
    > stdout.log 2> stderr.log &
emu=$!
trap 'kill $emu 2>/dev/null' EXIT
for _ in $(seq 50); do
    win=$(xdotool search --pid $emu 2>/dev/null | tail -1)
    [ -n "$win" ] && break
    sleep 0.2
done
eval "$(xdotool getwindowgeometry --shell "$win")"
while read -r cmd a b c d n dt; do
    case $cmd in
    sleep) sleep "$a" ;;
    tap) xdotool mousemove $((X + a)) $((Y + b)) mousedown 1
         sleep 0.12
         xdotool mouseup 1 ;;
    drag) n=${n:-12}
          xdotool mousemove $((X + a)) $((Y + b)) mousedown 1
          for s in $(seq "$n"); do
              xdotool mousemove $((X + a + (c - a) * s / n)) $((Y + b + (d - b) * s / n))
              sleep "${dt:-0.03}"
          done
          xdotool mouseup 1 ;;
    shot) import -window root -crop "${WIDTH}x${HEIGHT}+$X+$Y" "$a.png"
          echo "$dir/$a.png" ;;
    esac
done < "$acts"
grep -E "fault|PANIC|exception" stdout.log || true
