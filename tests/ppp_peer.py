#!/usr/bin/env python3
"""A minimal PPP peer (an ISP's access server) for the emulated CF modem.

The be300 emulator's --cf-modem tcp:127.0.0.1:PORT connects a dialed call to
this listener. It speaks PPP in HDLC-like framing (RFC 1662): LCP (RFC 1661)
with optional PAP (RFC 1334), IPCP (RFC 1332) assigning 10.64.0.2 with this
peer as router and DNS server (RFC 1877), and answers ICMP echo requests and
DNS A queries (every name is 10.64.0.1). No root, no pppd and no kernel PPP
support are needed. Each call is one TCP connection.

    tests/ppp_peer.py [--port 5640] [--user glaze --password ktest]
"""
import argparse
import socket
import struct
import sys

LOCAL = bytes([10, 64, 0, 1])
REMOTE = bytes([10, 64, 0, 2])
LCP, PAP, IPCP, IP = 0xC021, 0xC023, 0x8021, 0x0021


def log(*args):
    print("ppp_peer:", *args, flush=True)


def fcs16(data):
    fcs = 0xFFFF
    for b in data:
        fcs ^= b
        for _ in range(8):
            fcs = (fcs >> 1) ^ 0x8408 if fcs & 1 else fcs >> 1
    return fcs ^ 0xFFFF


def frame(protocol, payload):
    body = bytes([0xFF, 0x03]) + struct.pack(">H", protocol) + payload
    body += struct.pack("<H", fcs16(body))
    out = bytearray([0x7E])
    for b in body:
        # Escape the flag, the escape and every control character (the
        # default ACCM; the peer may ask for less, but more is always fine).
        if b < 0x20 or b in (0x7D, 0x7E):
            out += bytes([0x7D, b ^ 0x20])
        else:
            out.append(b)
    out.append(0x7E)
    return bytes(out)


def checksum(data):
    if len(data) & 1:
        data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def options(data):
    i = 0
    while i + 2 <= len(data):
        kind, length = data[i], data[i + 1]
        if length < 2 or i + length > len(data):
            return
        yield kind, data[i + 2:i + length]
        i += length


def pack_options(opts):
    return b"".join(bytes([k, len(v) + 2]) + v for k, v in opts)


class Call:
    def __init__(self, conn, user, password):
        self.conn, self.user, self.password = conn, user, password
        self.ident = 0
        self.lcp_up = self.lcp_acked = False
        self.authed = user is None
        self.ipcp_acked = self.ipcp_peer_acked = False
        self.announced = False

    def send(self, protocol, payload):
        self.conn.sendall(frame(protocol, payload))

    def control(self, protocol, code, ident, data=b""):
        self.send(protocol, struct.pack(">BBH", code, ident, len(data) + 4) + data)

    def next_id(self):
        self.ident = (self.ident + 1) & 0xFF
        return self.ident

    def start(self):
        opts = [(5, struct.pack(">I", 0x47A5E300))]
        if self.user is not None:
            opts.append((3, struct.pack(">H", PAP)))
        self.control(LCP, 1, self.next_id(), pack_options(opts))

    def start_ipcp(self):
        if self.lcp_up and self.authed:
            self.control(IPCP, 1, self.next_id(), pack_options([(3, LOCAL)]))

    def lcp(self, code, ident, data):
        if code == 1:                       # Configure-Request: take it as offered
            rejected = [(k, v) for k, v in options(data) if k not in (1, 2, 5, 7, 8)]
            if rejected:
                self.control(LCP, 4, ident, pack_options(rejected))
                return
            self.control(LCP, 2, ident, data)
            self.lcp_acked = True
        elif code == 2:                     # our request acknowledged
            self.lcp_up = True
            log("LCP up")
            self.start_ipcp()
        elif code in (3, 4):
            log("LCP options refused; retrying without them")
            self.control(LCP, 1, self.next_id(), pack_options([(5, struct.pack(">I", 0x47A5E301))]))
        elif code == 5:
            log("LCP terminate")
            self.control(LCP, 6, ident)
        elif code == 9:                     # Echo-Request
            self.control(LCP, 10, ident, struct.pack(">I", 0x47A5E300) + data[4:])

    def pap(self, code, ident, data):
        if code != 1:
            return
        n = data[0]
        user = data[1:1 + n].decode(errors="replace")
        password = data[2 + n:2 + n + data[1 + n]].decode(errors="replace")
        ok = self.user is None or (user == self.user and password == self.password)
        log("PAP %s for %r" % ("accepted" if ok else "refused", user))
        self.control(PAP, 2 if ok else 3, ident, b"\0")
        if ok:
            self.authed = True
            self.start_ipcp()

    def ipcp(self, code, ident, data):
        if code == 1:
            rejected, naks = [], []
            for k, v in options(data):
                if k == 3:
                    if v != REMOTE:
                        naks.append((3, REMOTE))
                elif k == 129:
                    if v != LOCAL:
                        naks.append((129, LOCAL))
                else:
                    rejected.append((k, v))     # VJ compression, secondary DNS, ...
            if rejected:
                self.control(IPCP, 4, ident, pack_options(rejected))
            elif naks:
                self.control(IPCP, 3, ident, pack_options(naks))
            else:
                self.control(IPCP, 2, ident, data)
                self.ipcp_peer_acked = True
        elif code == 2:
            self.ipcp_acked = True
        elif code == 5:
            self.control(IPCP, 6, ident)
        if self.ipcp_acked and self.ipcp_peer_acked and not self.announced:
            self.announced = True
            log("IPCP up: peer %s, us %s" % (socket.inet_ntoa(REMOTE), socket.inet_ntoa(LOCAL)))

    def ip(self, packet):
        if len(packet) < 20 or packet[0] >> 4 != 4:
            return
        ihl = (packet[0] & 15) * 4
        proto, src, dst = packet[9], packet[12:16], packet[16:20]
        body = packet[ihl:]
        if proto == 1 and body[:1] == b"\x08":          # ICMP echo request
            reply = b"\x00\x00\x00\x00" + body[4:]
            reply = reply[:2] + struct.pack("!H", checksum(reply)) + reply[4:]
            log("ping from %s" % socket.inet_ntoa(src))
            self.send_ip(1, dst, src, reply)
        elif proto == 17 and len(body) >= 8 and struct.unpack("!H", body[2:4])[0] == 53:
            sport = struct.unpack("!H", body[0:2])[0]
            query = body[8:]
            end = 12
            while end < len(query) and query[end]:
                end += query[end] + 1
            question = query[12:end + 5]
            answer = b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 60, 4) + LOCAL
            dns = query[:2] + b"\x81\x80" + struct.pack("!HHHH", 1, 1, 0, 0) + question + answer
            udp = struct.pack("!HHHH", 53, sport, 8 + len(dns), 0) + dns
            log("DNS query answered")
            self.send_ip(17, dst, src, udp)

    def send_ip(self, proto, src, dst, payload):
        header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 0, 0, 64, proto, 0, src, dst)
        header = header[:10] + struct.pack("!H", checksum(header)) + header[12:]
        self.send(IP, header + payload)

    def packet(self, data):
        if len(data) < 4 or fcs16(data[:-2]) != struct.unpack("<H", data[-2:])[0]:
            return
        data = data[:-2]
        if data[:2] == b"\xff\x03":
            data = data[2:]
        if data[0] & 1:                     # compressed protocol field
            protocol, data = data[0], data[1:]
        else:
            protocol, data = struct.unpack(">H", data[:2])[0], data[2:]
        if protocol in (LCP, PAP, IPCP):
            if len(data) < 4:
                return
            code, ident, length = struct.unpack(">BBH", data[:4])
            body = data[4:length]
            {LCP: self.lcp, PAP: self.pap, IPCP: self.ipcp}[protocol](code, ident, body)
        elif protocol == IP:
            self.ip(data)
        else:                               # IPv6CP, CCP, ...: Protocol-Reject
            self.control(LCP, 8, self.next_id(), struct.pack(">H", protocol) + data)

    def run(self):
        self.start()
        buf, escape = bytearray(), False
        while True:
            chunk = self.conn.recv(4096)
            if not chunk:
                return
            for b in chunk:
                if b == 0x7E:
                    if buf:
                        self.packet(bytes(buf))
                    buf, escape = bytearray(), False
                elif b == 0x7D:
                    escape = True
                else:
                    buf.append(b ^ 0x20 if escape else b)
                    escape = False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=5640)
    ap.add_argument("--user", help="require PAP with this user name")
    ap.add_argument("--password", default="")
    ap.add_argument("--calls", type=int, default=0, help="exit after this many calls (0: forever)")
    args = ap.parse_args()
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", args.port))
    server.listen(1)
    log("listening on 127.0.0.1:%d" % args.port)
    calls = 0
    while not args.calls or calls < args.calls:
        conn, _ = server.accept()
        calls += 1
        log("call %d connected" % calls)
        try:
            Call(conn, args.user, args.password).run()
        except (ConnectionError, OSError) as e:
            log("call ended: %s" % e)
        conn.close()
        log("call %d hung up" % calls)


if __name__ == "__main__":
    sys.exit(main())
