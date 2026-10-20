#!/usr/bin/env python3
"""Serve a local store, offering Hello 1.1 after the first catalog fetch.
Use with t_appstore_install.txt to exercise an update preserving Hello's counter.
"""
import argparse
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import shutil
import struct
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import mkstore

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',type=int,default=18767)
    parser.add_argument('--output',type=Path,default=Path('out/store-fixture'))
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=True)
    packages=output/'packages'; packages.mkdir(exist_ok=True)
    for path in (root/'out/pkg').glob('*.gzp'): shutil.copy2(path,packages/path.name)
    mkstore.build(root/'store/apps.json',packages,output)
    original=(output/'catalog.txt').read_bytes()
    hello=packages/'hello.gzp'; data=bytearray(hello.read_bytes())
    count,=struct.unpack_from('<I',data,4)
    for i in range(count):
        name,off,size=struct.unpack_from('<48sII',data,8+56*i)
        if name.split(b'\0')[0]==b'Info':
            data[off:off+size]=bytes(data[off:off+size]).replace(b'version=1.0\n',b'version=1.1\n')
    hello.write_bytes(data)
    mkstore.build(root/'store/apps.json',packages,output)
    updated=(output/'catalog.txt').read_bytes()
    class Handler(SimpleHTTPRequestHandler):
        catalogs=0
        def __init__(self,*a,**kw): super().__init__(*a,directory=str(output),**kw)
        def do_GET(self):
            if self.path=='/catalog.txt':
                type(self).catalogs+=1
                body=original if type(self).catalogs==1 else updated
                self.send_response(200); self.send_header('Content-Length',str(len(body)))
                self.end_headers(); self.wfile.write(body)
            else: super().do_GET()
    print(f'Fixture: first catalog Hello 1.0, subsequent catalogs Hello 1.1; port {args.port}',flush=True)
    ThreadingHTTPServer(('0.0.0.0',args.port),Handler).serve_forever()
