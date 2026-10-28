#!/usr/bin/env python3
"""Exercise the actual Glaze USB service through the emulator's usbredir
bridge. Supply a stock CE NAND and an autorun CF image containing GlazeBoot.
Only disposable copies are booted; logs and images are kept under out/.
"""
import argparse
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import time
import zlib


class Peer:
    def __init__(self, sock):
        self.sock = sock
        self.ident = 0
        self.request = 0
        self.stream = bytearray()
        self.offered = False

    def send(self, kind, data=b""):
        self.ident += 1
        self.sock.sendall(struct.pack("<III", kind, len(data), self.ident) + data)
        return self.ident

    def exact(self, count):
        data = bytearray()
        while len(data) < count:
            b = self.sock.recv(count - len(data))
            if not b:
                raise RuntimeError("usbredir connection closed")
            data += b
        return bytes(data)

    def packet(self):
        kind, count, ident = struct.unpack("<III", self.exact(12))
        if count > 4 * 1024 * 1024:
            raise RuntimeError("invalid usbredir length")
        data = self.exact(count)
        if kind == 1:
            self.offered = True
        elif kind == 2:
            self.offered = False
        return kind, ident, data

    def response(self, kind, ident):
        while True:
            k, i, data = self.packet()
            if (k, i) == (kind, ident):
                return data

    def offer(self):
        end = time.monotonic() + 120
        while not self.offered and time.monotonic() < end:
            self.packet()
        if not self.offered:
            raise RuntimeError("device did not enumerate")

    def configure(self):
        i = self.send(6, b"\x01")
        assert self.response(8, i)[0] == 0
        h = struct.pack("<BBBBHHH", 0, 0x22, 0x21, 0, 3, 0, 0)
        i = self.send(100, h)
        assert self.response(100, i)[3] == 0

    def write(self, data):
        h = struct.pack("<BBHIH", 2, 0, len(data), 0, 0)
        i = self.send(101, h + data)
        assert self.response(101, i)[1] == 0

    def read(self):
        i = self.send(101, struct.pack("<BBHIH", 0x81, 0, 4096, 0, 0))
        data = self.response(101, i)
        assert data[1] == 0
        return data[10:]

    def call(self, op, data=b"", replay=False):
        if not replay:
            self.request += 1
        h = struct.pack("<IIIII", 0x42555A47, 1 | op << 16, self.request, len(data), 0)
        frame = h + struct.pack("<I", zlib.crc32(h + data)) + data
        self.write(frame)
        while True:
            while len(self.stream) < 24:
                self.stream += self.read()
            magic, version_op, ident, count, status, checksum = struct.unpack_from("<IIIIiI", self.stream)
            assert magic == 0x42555A47 and count <= 4096
            while len(self.stream) < count + 24:
                self.stream += self.read()
            f = bytes(self.stream[:count + 24])
            del self.stream[:count + 24]
            assert checksum == zlib.crc32(f[:20] + f[24:])
            if version_op >> 16 == 7 and ident == 0:
                continue
            assert version_op == (1 | (op | 0x8000) << 16) and ident == self.request
            return status, f[24:]

    def hello(self):
        status, data = self.call(1)
        assert status == 0 and len(data) == 44 and struct.unpack_from("<I", data)[0] == 1
        print("USB hello:", data[12:].split(b"\0")[0].decode(), flush=True)

    def files(self):
        for count in (0, 1, 40, 63, 64, 65, 4096, 5000):
            data = bytes(i % 251 for i in range(count))
            path = f"/usb-{count}.dat".encode()
            assert self.call(2, struct.pack("<II", count, zlib.crc32(data)) + path)[0] == 0
            for off in range(0, count, 4092):
                block = struct.pack("<I", off) + data[off:off + 4092]
                assert self.call(3, block)[0] == 0
                assert self.call(3, block, replay=True)[0] == 0
            assert self.call(4)[0] == 0
            result = bytearray()
            while True:
                status, b = self.call(5, struct.pack("<I", len(result)) + path)
                assert status == 0
                result += b
                if len(b) < 4096:
                    break
            assert result == data
        # A bad upload must leave the preceding file intact.
        path = b"/usb-65.dat"
        assert self.call(2, struct.pack("<II", 1, 0) + path)[0] == 0
        assert self.call(3, struct.pack("<I", 0) + b"x")[0] == 0
        assert self.call(4)[0] < 0
        assert self.call(11)[0] == 0
        assert self.call(5, struct.pack("<I", 0) + path)[1] == bytes(range(65))
        assert self.call(8, b"\xff" * 24)[0] < 0
        print("USB file transfer, replay and failed-upload preservation: PASS", flush=True)

    def reload(self, directory):
        kernel = (directory / "kernel.bin").read_bytes()
        romfs = (directory / "romfs.img").read_bytes()
        data = kernel + bytes(-len(kernel) % 4096) + romfs
        manifest = struct.pack("<IIIIII", len(kernel), len(romfs), 2048, 2,
                               zlib.crc32(kernel), zlib.crc32(romfs))
        assert self.call(8, manifest)[0] == 0
        for off in range(0, len(data), 4092):
            assert self.call(9, struct.pack("<I", off) + data[off:off + 4092])[0] == 0
        assert self.call(10)[0] == 0
        print("USB reload accepted", flush=True)
        time.sleep(2)
        self.stream.clear()
        self.configure()
        self.hello()

    def automation(self, output):
        status, meta = self.call(13)
        assert status == 0 and struct.unpack("<IIII", meta) == (240, 320, 1, 153600)
        image = bytearray()
        while len(image) < 153600:
            status, block = self.call(14, struct.pack("<I", len(image)))
            assert status == 0 and 0 < len(block) <= 4096
            image += block
        assert len(image) == 153600 and len(set(image)) > 4
        assert self.call(14, struct.pack("<I", 153600))[0] < 0
        flags = struct.unpack("<II", self.call(21)[1])[0]
        assert flags & 1 and flags & 2  # connected and copying
        assert self.call(15)[0] == 0
        (output / "screen.rgb565").write_bytes(image)

        events = []
        events.append((0, 1, 0, 40, 280))
        for i in range(1, 18):
            events.append((500*i//17, 2, 0, 40+195*i//17, 280))
        events.append((520, 3, 0, 235, 280))
        assert self.call(16, struct.pack("<I", len(events)))[0] == 0
        encoded = b"".join(struct.pack("<IHHhh", *e) for e in events)
        assert self.call(17, struct.pack("<I", 0) + encoded)[0] == 0
        assert self.call(18)[0] == 0
        end = time.monotonic() + 20
        while time.monotonic() < end:
            state, elapsed, done, count = struct.unpack("<IIII", self.call(19)[1])
            if state == 4:
                assert done == count == len(events)
                break
            time.sleep(.02)
        else:
            raise RuntimeError("input script did not finish")
        # Let the unlock animation finish before launching through SpringBoard.
        time.sleep(.2)
        assert self.call(6, b"bin/settings")[0] == 0
        flags, pid = struct.unpack("<II", self.call(21)[1])
        assert flags & 1 and flags & 4 and pid > 0
        # Sleep explicitly, then start another script. Its first gesture must
        # reach the lock screen rather than being swallowed as a wake touch.
        for sequence in ([(0, 4, 8, 0, 0), (60, 5, 8, 0, 0)], events):
            assert self.call(16, struct.pack("<I", len(sequence)))[0] == 0
            block = b"".join(struct.pack("<IHHhh", *e) for e in sequence)
            assert self.call(17, struct.pack("<I", 0) + block)[0] == 0
            assert self.call(18)[0] == 0
            end = time.monotonic() + 20
            while time.monotonic() < end:
                if struct.unpack("<IIII", self.call(19)[1])[0] == 4:
                    break
                time.sleep(.02)
            else:
                raise RuntimeError("sleep/wake input script did not finish")
            time.sleep(.3)
        assert self.call(6, b"bin/settings")[0] == 0
        print("USB screenshot, timed unlock script and debugger app status: PASS", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emulator", type=Path, default=Path("../be300/build-host/be300"))
    ap.add_argument("--nand", type=Path, required=True)
    ap.add_argument("--cf", type=Path, required=True)
    ap.add_argument("--artifacts", type=Path, default=Path("out/glazeboot"))
    ap.add_argument("--reloads", type=int, default=3)
    args = ap.parse_args()
    work = Path("out/usb-runtime").resolve()
    work.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.nand, work / "nand.img")
    shutil.copyfile(args.cf, work / "cf.img")
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(15)
        with (work / "stdout.log").open("wb") as out, (work / "stderr.log").open("wb") as err:
            proc = subprocess.Popen([str(args.emulator.resolve()), "--nand", str(work / "nand.img"),
                "--cf", str(work / "cf.img"), "--rtc-host-time", "--speed", "0",
                "--pcconnect-dock", "usb-sync", "--pcconnect-bridge", f"tcp:127.0.0.1:{listener.getsockname()[1]}"],
                cwd=work, env=dict(os.environ, SDL_VIDEODRIVER="dummy"), stdout=out, stderr=err)
            try:
                sock, _ = listener.accept()
                with sock:
                    sock.settimeout(120)
                    peer = Peer(sock)
                    peer.send(0, b"glaze-usb-test".ljust(64, b"\0") + struct.pack("<I", 0x52))
                    peer.offer()
                    # Stock CE can enumerate first; wait for GlazeBoot to finish.
                    end = time.monotonic() + 120
                    while time.monotonic() < end:
                        if b"usb: development service ready" in (work / "stdout.log").read_bytes():
                            break
                        time.sleep(.2)
                    else:
                        raise RuntimeError("GlazeBoot did not reach the USB service")
                    peer.configure()
                    peer.hello()
                    peer.files()
                    peer.automation(work)
                    for _ in range(args.reloads):
                        peer.reload(args.artifacts)
                        peer.files()
                    print("usb-runtime: ALL PASS", flush=True)
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == "__main__":
    main()
