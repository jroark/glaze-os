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
                if (request, value, length) == (6, 0x100, 18) and len(payload) == 18 and \
                        payload[12:14] == b"\x00\x01" and time.monotonic() < peer.stale_until:
                    payload = payload[:12] + b"\x00\x00" + payload[14:]
                    peer.masked_descriptors += 1
                done = len(payload)
        elif op == 2:
            ep = a & 255
            body = exact(b) if not ep & 128 else b""
            if ep == 2 and len(body) == 56 and struct.unpack_from("<II", body) == (0x42555a47, 1 | 23 << 16):
                peer.host_seeds.append(body[24:])
            if ep == 2 and len(body) == 24 and struct.unpack_from("<II", body) == (0x42555a47, 1 | 10 << 16):
                peer.stale_until = time.monotonic() + peer.descriptor_delay
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
            if peer.descriptor_delay:
                status = -5  # libusb: old descriptor/handle disappeared
                peer.reset_descriptor_changes += 1
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
    ap.add_argument("--descriptor-delay", type=float, default=0, help="simulate stale CE descriptors after a RAM reload")
    ap.add_argument("--reboot-test", choices=["ce", "glaze"], help="verify reboot and CE recovery in one guest")
    ap.add_argument("--work",type=Path,default=Path("out/usb-native"))
    ap.add_argument("--no-trace",action="store_true")
    ap.add_argument("--nand-transaction-test",nargs=3,type=Path,metavar=("TARGET","INSTALL_PLAN","RESTORE_PLAN"),
                    help="install then restore disposable emulator NAND using the native writer")
    ap.add_argument("command", nargs="*", default=["ce-self-test"])
    args = ap.parse_args()
    work = args.work.resolve()
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
        with (work / "stdout.log").open("wb") as out, (work / "stderr.log").open("wb") as err, \
                (work / "cli.log").open("wb") as cli_output:
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
                    peer.host_seeds = []
                    peer.descriptor_delay = args.descriptor_delay
                    peer.stale_until = 0
                    peer.masked_descriptors = 0
                    peer.reset_descriptor_changes = 0
                    commands = args.command and [args.command]
                    if args.reboot_test == "ce": commands = [["reboot"], ["ce-probe"]]
                    if args.reboot_test == "glaze": commands = [["boot"], ["reboot"], ["reboot", "--ce"], ["ce-probe"]]
                    if args.nand_transaction_test:
                        target,install,restore=[str(p.resolve()) for p in args.nand_transaction_test]
                        source=str(args.nand.resolve())
                        commands=[["nand-apply",source,target,install,"--execute"],
                                  ["nand-apply",target,source,restore,"--execute"]]
                    for command in commands:
                        shell_before = (work/"stdout.log").read_bytes().count(b"CASIO Original Shell started")
                        reconnect_deadline=time.monotonic()+120
                        while True:
                            cli = subprocess.Popen([str(args.cli.resolve())]+([] if args.no_trace else ["--trace"]) + command,
                                env=dict(os.environ, GLAZE_USB_TEST_SOCKET=str(socket_path)),
                                stdout=cli_output, stderr=subprocess.STDOUT)
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
                    if args.cf or args.reboot_test == "glaze":
                        assert peer.host_seeds
                    if args.descriptor_delay:
                        assert peer.masked_descriptors
                        assert peer.reset_descriptor_changes
                        assert "USB reset:" not in (work / "cli.log").read_text()
                        print("Late Glaze descriptors: automatic reconnect PASS", flush=True)
                    if peer.host_seeds:
                        trace = (work / "cli.log").read_text()
                        assert len(set(peer.host_seeds)) == len(peer.host_seeds)
                        for seed in peer.host_seeds:
                            assert len(seed) == 32 and any(seed)
                            assert " ".join(f"{b:02x}" for b in seed) not in trace
                        print("Native OS CSPRNG: fresh seeds; --trace excludes seed bytes PASS", flush=True)
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
    if args.nand_transaction_test:
        assert (work/"nand.img").read_bytes()==args.nand.read_bytes(),"NAND restore changed original data/OOB"
        print("Native USB install/restore: every physical byte restored PASS",flush=True)


if __name__ == "__main__":
    main()
