#!/usr/bin/env python3
"""A minimal PPP server for dial-up tests: the be300 emulator's --modem
HOST:PORT connects here after ATD (be300 src/hw/modem.c).

It speaks PPP in HDLC-like framing (RFC 1662), LCP (RFC 1661), PAP
(RFC 1334) or CHAP-MD5 (RFC 1994) and IPCP with DNS addresses (RFC 1332,
RFC 1877). It gives the guest 10.64.0.2, is itself 10.64.0.1, answers ICMP
echo for any address and DNS A queries (localhost -> 127.0.0.1, anything
else NXDOMAIN). It is not a router: other IP traffic is dropped.

  tests/ppp_peer.py [--port 5555] [--auth pap|chap] [--user glaze --password secret]
"""
import argparse
import hashlib
import os
import socket
import struct
import sys

PEER, GUEST, DNS = bytes([10, 64, 0, 1]), bytes([10, 64, 0, 2]), bytes([10, 64, 0, 1])
LCP, PAP, CHAP, IPCP, IP = 0xC021, 0xC023, 0xC223, 0x8021, 0x0021


def fcs16(data):
    fcs = 0xFFFF
    for b in data:
        fcs ^= b
        for _ in range(8):
            fcs = (fcs >> 1) ^ 0x8408 if fcs & 1 else fcs >> 1
    return fcs ^ 0xFFFF


def log(*a):
    print('ppp-peer:', *a, flush=True)


class Peer:
    def __init__(self, conn, args):
        self.conn, self.args = conn, args
        self.buf, self.ident = bytearray(), 1
        self.lcp_acked = self.lcp_ours_acked = False
        self.authed = False
        self.ipcp_started = False
        self.challenge = b''

    # ---- framing -------------------------------------------------------
    def send(self, proto, payload):
        body = b'\xff\x03' + struct.pack('>H', proto) + payload
        body += struct.pack('<H', fcs16(body))
        out = bytearray(b'\x7e')
        for b in body:
            # Escape flag, escape and every control character (default ACCM).
            if b in (0x7E, 0x7D) or b < 0x20:
                out += bytes([0x7D, b ^ 0x20])
            else:
                out.append(b)
        out.append(0x7E)
        self.conn.sendall(bytes(out))

    def frames(self, data):
        for b in data:
            if b == 0x7E:
                if len(self.buf) >= 4:
                    yield bytes(self.buf)
                self.buf.clear()
                self.escape = False
            elif b == 0x7D:
                self.escape = True
            else:
                self.buf.append(b ^ 0x20 if getattr(self, 'escape', False) else b)
                self.escape = False

    def control(self, proto, code, ident, data=b''):
        self.send(proto, struct.pack('>BBH', code, ident, 4 + len(data)) + data)

    def next_id(self):
        self.ident = (self.ident + 1) & 0xFF
        return self.ident

    # ---- LCP -----------------------------------------------------------
    def lcp_request(self):
        opts = struct.pack('>BBI', 5, 6, 0x5A5A1234)        # magic number
        if self.args.auth == 'pap':
            opts += struct.pack('>BBH', 3, 4, PAP)
        elif self.args.auth == 'chap':
            opts += struct.pack('>BBHB', 3, 5, CHAP, 5)        # MD5
        self.control(LCP, 1, self.next_id(), opts)

    def options(self, data):
        i = 0
        while i + 2 <= len(data):
            t, n = data[i], data[i + 1]
            if n < 2 or i + n > len(data):
                return
            yield t, data[i:i + n]
            i += n

    def lcp(self, code, ident, data):
        if code == 1:                                         # Configure-Request
            reject = b''.join(o for t, o in self.options(data) if t not in (1, 2, 5))
            if reject:
                self.control(LCP, 4, ident, reject)
                return
            self.control(LCP, 2, ident, data)
            self.lcp_acked = True
            if not self.lcp_ours_acked:
                self.lcp_request()
        elif code == 2:                                       # Configure-Ack
            self.lcp_ours_acked = True
        elif code in (3, 4):
            log('guest refused our LCP options', code)
            self.lcp_request()
        elif code == 5:                                       # Terminate-Request
            self.control(LCP, 6, ident)
            log('guest terminated the link')
        elif code == 9:                                       # Echo-Request
            self.control(LCP, 10, ident, struct.pack('>I', 0x5A5A1234) + data[4:])
        if self.lcp_acked and self.lcp_ours_acked and not self.authed:
            if self.args.auth == 'none':
                self.authed = True
            elif self.args.auth == 'chap' and not self.challenge:
                self.challenge = os.urandom(16)
                self.control(CHAP, 1, self.next_id(), bytes([16]) + self.challenge + b'peer')

    def pap(self, code, ident, data):
        if code != 1 or not data:
            return
        n = data[0]
        user, rest = data[1:1 + n], data[1 + n:]
        password = rest[1:1 + rest[0]] if rest else b''
        ok = user == self.args.user.encode() and password == self.args.password.encode()
        log('PAP', user.decode(errors='replace'), 'accepted' if ok else 'refused')
        self.control(PAP, 2 if ok else 3, ident, b'\x00')
        self.authed = ok

    def chap(self, code, ident, data):
        if code != 2 or not data:
            return
        value = data[1:1 + data[0]]
        want = hashlib.md5(bytes([ident]) + self.args.password.encode() + self.challenge).digest()
        ok = value == want
        log('CHAP-MD5', 'accepted' if ok else 'refused')
        self.control(CHAP, 3 if ok else 4, ident)
        self.authed = ok

    # ---- IPCP ----------------------------------------------------------
    def ipcp(self, code, ident, data):
        if not self.authed:
            return
        if not self.ipcp_started:
            self.ipcp_started = True
            self.control(IPCP, 1, self.next_id(), struct.pack('>BB', 3, 6) + PEER)
        if code == 1:
            reject, nak = b'', b''
            for t, o in self.options(data):
                if t == 3 and o[2:] != GUEST:
                    nak += bytes([3, 6]) + GUEST
                elif t == 129 and o[2:] != DNS:
                    nak += bytes([129, 6]) + DNS
                elif t == 131 and o[2:] != DNS:
                    nak += bytes([131, 6]) + DNS
                elif t not in (3, 129, 131):
                    reject += o
            if reject:
                self.control(IPCP, 4, ident, reject)
            elif nak:
                self.control(IPCP, 3, ident, nak)
            else:
                self.control(IPCP, 2, ident, data)
                log('IPCP up: guest', '.'.join(map(str, GUEST)))

    # ---- IP --------------------------------------------------------------
    @staticmethod
    def checksum(data):
        if len(data) & 1:
            data += b'\0'
        s = sum(struct.unpack('>%dH' % (len(data) // 2), data))
        while s >> 16:
            s = (s & 0xFFFF) + (s >> 16)
        return ~s & 0xFFFF

    def ip_reply(self, src, dst, proto, payload):
        hdr = struct.pack('>BBHHHBBH4s4s', 0x45, 0, 20 + len(payload), 0, 0, 64, proto, 0, src, dst)
        hdr = hdr[:10] + struct.pack('>H', self.checksum(hdr)) + hdr[12:]
        self.send(IP, hdr + payload)

    def ip(self, pkt):
        if len(pkt) < 20 or pkt[0] >> 4 != 4:
            return
        hl = (pkt[0] & 15) * 4
        proto, src, dst, body = pkt[9], pkt[12:16], pkt[16:20], pkt[hl:]
        if proto == 1 and body[:1] == b'\x08':                # ICMP echo request
            reply = b'\x00\x00\x00\x00' + body[4:]
            reply = reply[:2] + struct.pack('>H', self.checksum(reply)) + reply[4:]
            self.ip_reply(dst, src, 1, reply)
        elif proto == 17 and len(body) >= 8 and struct.unpack('>H', body[2:4])[0] == 53:
            sport = struct.unpack('>H', body[0:2])[0]
            answer = self.dns(body[8:])
            if answer:
                udp = struct.pack('>HHHH', 53, sport, 8 + len(answer), 0) + answer
                self.ip_reply(dst, src, 17, udp)

    def dns(self, q):
        if len(q) < 12:
            return None
        i, labels = 12, []
        while i < len(q) and q[i]:
            labels.append(q[i + 1:i + 1 + q[i]].decode(errors='replace'))
            i += 1 + q[i]
        question = q[12:i + 5]
        name = '.'.join(labels).lower()
        qtype = struct.unpack('>H', q[i + 1:i + 3])[0] if i + 3 <= len(q) else 0
        log('DNS', name, qtype)
        if name == 'localhost' and qtype == 1:
            return (q[:2] + struct.pack('>HHHHH', 0x8180, 1, 1, 0, 0) + question +
                    struct.pack('>HHHIH', 0xC00C, 1, 1, 60, 4) + bytes([127, 0, 0, 1]))
        return q[:2] + struct.pack('>HHHHH', 0x8183, 1, 0, 0, 0) + question

    # ---- dispatch -------------------------------------------------------
    def frame(self, f):
        if len(f) < 6 or fcs16(f[:-2]) != struct.unpack('<H', f[-2:])[0]:
            log('bad frame', f.hex())
            return
        f = f[:-2]
        if f[:2] == b'\xff\x03':
            f = f[2:]
        proto = struct.unpack('>H', f[:2])[0]
        data = f[2:]
        if proto == IP:
            self.ip(data)
            return
        if len(data) < 4:
            return
        code, ident, length = struct.unpack('>BBH', data[:4])
        body = data[4:length]
        handler = {LCP: self.lcp, PAP: self.pap, CHAP: self.chap, IPCP: self.ipcp}.get(proto)
        if handler:
            handler(code, ident, body)
        elif self.lcp_acked:
            self.control(LCP, 8, self.next_id(), struct.pack('>H', proto) + data[:64])

    def run(self):
        while True:
            data = self.conn.recv(4096)
            if not data:
                log('guest hung up')
                return
            for f in self.frames(data):
                self.frame(f)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', type=int, default=5555)
    parser.add_argument('--auth', choices=('pap', 'chap', 'none'), default='pap')
    parser.add_argument('--user', default='glaze')
    parser.add_argument('--password', default='secret')
    parser.add_argument('--once', action='store_true', help='exit after the first call')
    args = parser.parse_args()
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('127.0.0.1', args.port))
    server.listen(1)
    log('listening on', args.port, 'auth', args.auth)
    while True:
        conn, _ = server.accept()
        log('call connected')
        try:
            Peer(conn, args).run()
        except (ConnectionError, OSError) as e:
            log('call ended:', e)
        conn.close()
        if args.once:
            return 0


if __name__ == '__main__':
    sys.exit(main())
