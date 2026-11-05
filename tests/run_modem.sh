#!/bin/sh
# Dial-up end to end in the emulator: a ktest NAND built with MODEM=1 boots
# with be300's --cf-modem (a Rockwell RC56LD CF modem model) whose calls
# connect to tests/ppp_peer.py, a userspace PPP access server. ktest dials,
# is refused a wrong PAP password, gets 10.64.0.2 by IPCP, pings the peer,
# resolves a name through it, hangs up and redials from the saved settings.
# No pppd, root or kernel PPP support is needed.
#
# usage: tests/run_modem.sh [seconds] [make args...]
# Output: out/shots/modem/{stdout.log,stderr.log,peer.log}.
# Set BE300 to the emulator binary if it isn't at ../be300/build-host/be300.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
secs=${1:-240}
[ $# -gt 0 ] && shift
port=${PPP_PORT:-5640}
out=$root/out/shots/modem
mkdir -p "$out"
timeout_cmd=$(command -v gtimeout || command -v timeout)

(cd "$root" && ./build.sh MODEM=1 INIT=bin/ktest NAND_DROP=wince "$@" >/dev/null)
cp "$root/out/glaze.bin" "$out/nand.img"
python3 "$root/tests/ppp_peer.py" --port "$port" --user glaze --password ktest > "$out/peer.log" 2>&1 &
peer=$!
trap 'kill $peer 2>/dev/null || true' EXIT
sleep 1
cd "$out"
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} \
    "$timeout_cmd" "${secs}s" "${BE300:-$root/../be300/build-host/be300}" --nand nand.img --rtc-host-time \
    --cf-modem "tcp:127.0.0.1:$port" > stdout.log 2> stderr.log || true
grep -E "^modem: (CONNECT|PPP|dialing failed|hung up)|ktest: (FAIL|ALL|FAILED|SKIP modem)" stdout.log || true
grep -q "ktest: ALL PASS" stdout.log && ! grep -q "ktest: SKIP modem" stdout.log
