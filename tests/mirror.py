#!/usr/bin/env python3
"""Native production VNC bridge, real delta engine, deterministic device I/O."""
import contextlib
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]

def exact(s,n):
    out=bytearray()
    while len(out)<n:
        b=s.recv(n-len(out))
        if not b:raise EOFError('viewer disconnected')
        out+=b
    return bytes(out)

def hello(s,version=8):
    assert exact(s,12)==b'RFB 003.008\n'
    banner=f'RFB 003.{version:03}\n'.encode()
    for i in range(0,len(banner),3):s.sendall(banner[i:i+3])
    if version==3:assert exact(s,4)==struct.pack('>I',1)
    else:
        assert exact(s,2)==b'\x01\x01'
        s.sendall(b'\x01')
        if version==8:assert exact(s,4)==bytes(4)
    s.sendall(b'\x01')
    header=exact(s,24)
    assert struct.unpack_from('>HH',header)==(240,320)
    assert exact(s,struct.unpack_from('>I',header,20)[0])==b'Glaze BE-300'
    return header[4:20]

def request(s,rect=(0,0,3,1),incremental=False):
    s.sendall(struct.pack('>BBHHHH',3,incremental,*rect))

def update(s,bpp=2):
    head=exact(s,4);assert head[:2]==bytes(2)
    count=struct.unpack_from('>H',head,2)[0]
    if not count:return None,b''
    assert count==1
    x,y,w,h,encoding=struct.unpack('>HHHHI',exact(s,12));assert encoding==0
    return (x,y,w,h),exact(s,w*h*bpp)

def fmt(s,bits=32,big=0,maxima=(255,255,255),shifts=(16,8,0),depth=None):
    payload=struct.pack('>BBBBHHHBBB3x',bits,depth or bits,big,1,*maxima,*shifts)
    for byte in b'\0\0\0\0'+payload:s.sendall(bytes([byte]))

def pointer(s,buttons,x,y):s.sendall(struct.pack('>BBHH',5,buttons,x,y))
def key(s,down,symbol):s.sendall(struct.pack('>BBHI',4,down,0,symbol))

@contextlib.contextmanager
def bridge(*args,corrupt=False):
    with tempfile.TemporaryDirectory(prefix='glaze-mirror-') as d:
        metrics=Path(d)/'metrics';log=Path(d)/'log'
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
        env=dict(os.environ)
        if corrupt:env['MIRROR_CORRUPT_ONCE']='1'
        with log.open('wb') as out:
            p=subprocess.Popen([str(ROOT/'out/host/mirror-fixture'),str(metrics),'--port',str(port),'--fps','15',*args],stdout=out,stderr=out,env=env)
            try:
                end=time.monotonic()+5
                while True:
                    try:
                        s=socket.create_connection(('127.0.0.1',port),timeout=3);break
                    except ConnectionRefusedError:
                        if p.poll() is not None or time.monotonic()>end:raise RuntimeError(log.read_text())
                        time.sleep(.02)
                yield s,metrics,port
                s.close()
            finally:
                p.terminate();p.wait(timeout=10)
        text=log.read_text()
        assert p.returncode==0,text
        assert 'runtime error:' not in text and 'ERROR: AddressSanitizer' not in text,text

def metrics(path):
    end=time.monotonic()+3
    while time.monotonic()<end:
        lines=path.read_text().splitlines() if path.exists() else []
        if lines and len(lines[0].split())==7:
            values=list(map(int,lines[0].split()));data=bytes.fromhex(lines[1]) if len(lines)>1 else b''
            if len(data)==values[3]:return values,data
        time.sleep(.01)
    raise AssertionError('fixture metrics incomplete')

class Mirroring(unittest.TestCase):
    def test_01_versions_and_colour(self):
        for version in (3,7,8):
            with self.subTest(version=version),bridge() as (s,_,_):
                hello(s,version);request(s)
                self.assertEqual(update(s),((0,0,3,1),b'\x00\xf8\xe0\x07\x1f\x00'))
                fmt(s,depth=24);request(s,incremental=True)
                self.assertEqual(update(s,4)[1],b'\0\0\xff\0\0\xff\0\0\xff\0\0\0')
                fmt(s,16,1,(31,63,31),(11,5,0));request(s)
                self.assertEqual(update(s)[1],b'\xf8\0\x07\xe0\0\x1f')
                fmt(s,8,0,(7,7,3),(5,2,0));request(s)
                self.assertEqual(update(s,1)[1],b'\xe0\x1c\x03')

    def test_02_incremental_input_and_delta_bandwidth(self):
        with bridge() as (s,path,_):
            hello(s);request(s,(0,0,16,16));update(s)
            before,_=metrics(path)
            request(s,(0,0,16,16),True)
            s.settimeout(.15)
            with self.assertRaises(socket.timeout):s.recv(1)
            s.settimeout(3)
            pointer(s,1,4,4);pointer(s,1,5,5);pointer(s,0,5,5)
            rectangle,pixels=update(s)
            self.assertEqual(rectangle,(0,0,16,16))
            self.assertEqual(pixels[(4*16+4)*2:(4*16+4)*2+2],b'\xff\xff')
            key(s,True,ord('A'));key(s,False,ord('A'))
            key(s,True,0xffbe);key(s,False,0xffbe)
            time.sleep(.15)
            after,events=metrics(path)
            self.assertGreater(after[2]-before[2],0)
            self.assertLessEqual(after[2]-before[2],2*516)
            decoded=list(struct.iter_unpack('<HHhh',events))
            self.assertEqual(decoded[:3],[(1,0,4,4),(2,0,5,5),(3,0,5,5)])
            self.assertIn((6,ord('A'),0,0),decoded)
            self.assertIn((4,7,0,0),decoded)
            self.assertIn((5,7,0,0),decoded)
            pointer(s,31,65535,65535);pointer(s,0,65535,65535)
            time.sleep(.1)
            events=list(struct.iter_unpack('<HHhh',metrics(path)[1]))
            self.assertIn((1,0,239,319),events)
            for code in (7,2,3,4):  # Home, Escape, wheel Up and Down
                self.assertIn((4,code,0,0),events)
                self.assertIn((5,code,0,0),events)

    def test_03_view_only_and_cut_text(self):
        with bridge('--view-only') as (s,path,_):
            hello(s)
            s.sendall(struct.pack('>BBHii',2,0,2,5,0)) # encoding hints incl. Raw
            text=b'x'*9000;s.sendall(b'\x06\0\0\0'+struct.pack('>I',len(text))+text)
            pointer(s,1,2,2);key(s,True,ord('z'))
            request(s);self.assertEqual(update(s)[1][:2],b'\0\xf8')
            self.assertEqual(metrics(path)[0][3],0)

    def test_04_regions_and_held_input_cleanup(self):
        with bridge() as (s,path,port):
            hello(s);request(s,(65535,65535,65535,65535));self.assertEqual(update(s),(None,b''))
            request(s,(239,319,65535,65535));self.assertEqual(update(s)[0],(239,319,1,1))
            pointer(s,1,65535,65535);key(s,True,0xff52)
            time.sleep(.1);self.assertEqual(metrics(path)[0][5:], [1,8])
            s.close();time.sleep(.1)
            self.assertEqual(metrics(path)[0][5:],[0,0])
            with socket.create_connection(('127.0.0.1',port),timeout=3) as other:
                hello(other);request(other);self.assertEqual(update(other)[1][:2],b'\0\xf8')

    def test_05_corrupt_upstream_frame_resync(self):
        with bridge(corrupt=True) as (s,path,_):
            hello(s);request(s);self.assertEqual(update(s)[1][:2],b'\0\xf8')
            self.assertGreaterEqual(metrics(path)[0][1],2)

    def test_06_bad_messages(self):
        for bad in (b'\x7f',b'\x02\0\x01\x01',b'\x06\0\0\0'+struct.pack('>I',1024*1024+1)):
            with self.subTest(bad=bad),bridge() as (s,_,_):
                hello(s);s.sendall(bad);self.assertEqual(s.recv(1),b'')
        with bridge() as (s,_,_):
            hello(s);fmt(s,16,0,(31,63,31),(0,0,0))
            self.assertEqual(s.recv(1),b'')

    def test_07_subregion_baselines(self):
        with bridge() as (s,_,_):
            hello(s);request(s,(0,0,1,1));self.assertEqual(update(s)[1],b'\0\xf8')
            request(s,(1,0,1,1),True);self.assertEqual(update(s)[1],b'\xe0\x07')
            request(s,(2,0,1,1),True);self.assertEqual(update(s)[1],b'\x1f\0')

    def test_08_partial_handshake_deadline(self):
        with bridge() as (s,_,port):
            self.assertEqual(exact(s,12),b'RFB 003.008\n')
            s.sendall(b'RFB ');s.settimeout(7)
            self.assertEqual(s.recv(1),b'')
            with socket.create_connection(('127.0.0.1',port),timeout=3) as other:
                hello(other);request(other);self.assertEqual(update(other)[1][:2],b'\0\xf8')

if __name__=='__main__':unittest.main(verbosity=2)
