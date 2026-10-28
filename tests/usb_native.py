#!/usr/bin/env python3
"""Run the native CLI against stock CE and Glaze in the emulator.
Build the test-only libusb facade with `make -C host glaze-usb-emulator`.
The normal glaze-usb binary always uses the real libusb library.
"""
import argparse
import os
from pathlib import Path
import select
import shutil
import socket
import struct
import subprocess
import time
from usb_runtime import Peer


def reply(peer, kind, ident, timeout):
    end = time.monotonic() + max(timeout / 1000, .001)
    while time.monotonic() < end:
        if not select.select([peer.sock], [], [], max(0, end - time.monotonic()))[0]:
            break
        k, i, data = peer.packet()
        if (k, i) == (kind, ident):
            return data
    peer.sock.sendall(struct.pack("<III", 21, 0, ident))
    return None


def serve(peer, client):
    def exact(n):
        out = bytearray()
        while len(out) < n:
            b = client.recv(n - len(out))
            if not b:
                raise EOFError
            out += b
        return bytes(out)
    while True:
        try:
            op, a, b, timeout = struct.unpack("<IIII", exact(16))
        except EOFError:
            return
        status, payload, done = 0, b"", 0
        if op == 1:
            rt, request, value = a & 255, a >> 8 & 255, a >> 16
            index, length = b & 65535, b >> 16
            body = exact(length) if not rt & 128 else b""
            ident = peer.send(100, struct.pack("<BBBBHHH", rt & 128, request, rt, 0, value, index, length) + body)
            data = reply(peer, 100, ident, timeout)
            status = -7 if data is None else -9 if data[3] else 0
            if data is not None and rt & 128 and not status:
                payload = data[10:]
                done = len(payload)
        elif op == 2:
            ep = a & 255
            body = exact(b) if not ep & 128 else b""
            # The emulator drops partially received IN data on cancellation.
            # Read one USB packet at a time so the facade never loses a frame
            # prefix when the guest takes longer than the host's read timeout.
            wire_count = min(b, 64) if ep & 128 else b
            ident = peer.send(101, struct.pack("<BBHIH", ep, 0, wire_count & 65535, 0, wire_count >> 16) + body)
            data = reply(peer, 101, ident, timeout)
            status = -7 if data is None else -9 if data[1] else 0
            if data is not None and not status:
                done = len(data) - 10 if ep & 128 else b
                if ep & 128:
                    payload = data[10:]
        elif op == 3:
            ident = peer.send(6, bytes([a]))
            data = reply(peer, 8, ident, timeout)
            status = -7 if data is None else -1 if data[0] else 0
        elif op == 4:
            peer.send(3)
        else:
            raise RuntimeError(f"unknown test RPC {op}")
        client.sendall(struct.pack("<ii", status, done) + payload)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", type=Path, required=True)
    ap.add_argument("--cf", type=Path, help="autorun GlazeBoot card; start CLI after Glaze is ready")
    ap.add_argument("--emulator", type=Path, default=Path("../be300/build-host/be300"))
    ap.add_argument("--cli", type=Path, default=Path("out/host/glaze-usb-emulator"))
    ap.add_argument("--speed", default="166", help="emulator target in million instructions/sec")
    ap.add_argument("--reboot-test", choices=["ce", "glaze"], help="verify reboot and CE recovery in one guest")
    ap.add_argument("command", nargs="*", default=["ce-self-test"])
    args = ap.parse_args()
    work = Path("out/usb-native").resolve()
    work.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.nand, work / "nand.img")
    if args.cf:
        shutil.copyfile(args.cf, work / "cf.img")
    socket_path = work / "control.sock"
    if socket_path.exists():
        socket_path.unlink()
    with socket.socket() as listener, socket.socket(socket.AF_UNIX) as control:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(15)
        control.bind(str(socket_path))
        control.listen(1)
        control.settimeout(1)
        with (work / "stdout.log").open("wb") as out, (work / "stderr.log").open("wb") as err:
            emulator = subprocess.Popen([str(args.emulator.resolve()), "--nand", str(work / "nand.img"),
                "--rtc-host-time", "--speed", args.speed, "--pcconnect-dock", "usb-sync", "--pcconnect-bridge",
                f"tcp:127.0.0.1:{listener.getsockname()[1]}"] + (["--cf",str(work/"cf.img")] if args.cf else []), cwd=work,
                env=dict(os.environ, SDL_VIDEODRIVER="dummy"), stdout=out, stderr=err)
            cli = None
            try:
                sock, _ = listener.accept()
                with sock:
                    sock.settimeout(120)
                    peer = Peer(sock)
                    peer.send(0, b"glaze-native-test".ljust(64, b"\0") + struct.pack("<I", 0x52))
                    peer.offer()
                    if args.cf:
                        end=time.monotonic()+120
                        while b"usb: development service ready" not in (work/"stdout.log").read_bytes():
                            if time.monotonic()>=end: raise RuntimeError("Glaze USB service did not start")
                            time.sleep(.02)
                    print("USB enumerated; running native CLI", flush=True)
                    commands = args.command and [args.command]
                    if args.reboot_test == "ce": commands = [["reboot"], ["ce-probe"]]
                    if args.reboot_test == "glaze": commands = [["boot"], ["reboot"], ["reboot", "--ce"], ["ce-probe"]]
                    for command in commands:
                        shell_before = (work/"stdout.log").read_bytes().count(b"CASIO Original Shell started")
                        reconnect_deadline=time.monotonic()+120
                        while True:
                            cli = subprocess.Popen([str(args.cli.resolve()), "--trace"] + command,
                                env=dict(os.environ, GLAZE_USB_TEST_SOCKET=str(socket_path)))
                            while cli.poll() is None:
                                try:
                                    client, _ = control.accept()
                                except socket.timeout:
                                    continue
                                with client:
                                    serve(peer, client)
                            if not cli.returncode: break
                            # CE's shell can start before its USB driver has
                            # re-enumerated. Retry only this read-only probe.
                            if args.reboot_test and command==["ce-probe"] and time.monotonic()<reconnect_deadline:
                                time.sleep(.2);continue
                            raise RuntimeError(f"native CLI failed ({cli.returncode}): {command}")
                        if args.reboot_test and (command == ["reboot", "--ce"] or
                                                  (args.reboot_test == "ce" and command == ["reboot"])):
                            deadline=time.monotonic()+120
                            while (work/"stdout.log").read_bytes().count(b"CASIO Original Shell started") <= shell_before:
                                if time.monotonic()>=deadline: raise RuntimeError("reboot did not return to CE shell")
                                time.sleep(.02)
                            print("Reboot returned to stock CE shell: PASS",flush=True)
                    print("usb-native: ALL PASS", flush=True)
            finally:
                if cli is not None and cli.poll() is None:
                    cli.terminate()
                    cli.wait(timeout=10)
                emulator.terminate()
                try:
                    emulator.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    emulator.kill()
                    emulator.wait()
                socket_path.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
