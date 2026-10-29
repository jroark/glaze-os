#!/usr/bin/env python3
"""Real guest TLS/RPC/reload integration; all output uses disposable NAND copies.
Build with NETDEBUG_TESTS=1 INIT=bin/debug_guest DEBUG_RELOAD_ADDR=HOST_LAN_IP
and run inside the emulator's toolchain container. The emulator NAT has no
inbound port forwarding, so the guest test peer uses lwIP loopback for debugger
RPC and fetches only the reload image stream from this host fixture.
"""
import argparse, os, shutil, socketserver, struct, subprocess, threading, time, zlib
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--host',required=True)
p.add_argument('--emulator',default=str(ROOT.parent/'be300/build-host/be300'))
p.add_argument('--nand',default=str(ROOT/'out/glaze.bin'))
p.add_argument('--artifacts',default=str(ROOT/'out/glazeboot'))
p.add_argument('--timeout',type=int,default=360)
p.add_argument('--core-only',action='store_true',help='stop after RPC checks, before RAM reload')
a=p.parse_args()
artifacts=Path(a.artifacts)
k=(artifacts/'kernel.bin').read_bytes();r=(artifacts/'romfs.img').read_bytes()
assert k[:4]==b'GZKB' and r[:4]==b'GZRF'
manifest=struct.pack('<6I',len(k),len(r),2048,2,zlib.crc32(k),zlib.crc32(r))
stream=manifest+k+bytes(-len(k)%4096)+r
class Images(socketserver.BaseRequestHandler):
    def handle(self):
        try:self.request.sendall(stream)
        except (BrokenPipeError,ConnectionResetError):pass
class Server(socketserver.TCPServer):allow_reuse_address=True
work=ROOT/'out/debug-network-runtime';work.mkdir(parents=True,exist_ok=True)
shutil.copyfile(a.nand,work/'nand.img')
with Server((a.host,8877),Images) as server:
    threading.Thread(target=server.serve_forever,daemon=True).start()
    with (work/'stdout.log').open('wb') as out,(work/'stderr.log').open('wb') as err:
        emulator=subprocess.Popen([a.emulator,'--nand','nand.img','--rtc-host-time','--speed','0','--ne2000'],cwd=work,stdout=out,stderr=err,
                                  env=dict(os.environ,BE300_AUDIO='0',BE300_AUTOSTOP_SEC=str(a.timeout+10)))
        try:
            end=time.monotonic()+a.timeout
            while emulator.poll() is None and time.monotonic()<end:
                logs=(work/'stdout.log').read_text(errors='replace')
                if 'debug-net-guest: ALL PASS' in logs or (a.core_only and 'debug-net-guest: files, replay, logs, app, screenshot, script, disconnect PASS' in logs):
                    print('PASS: guest TLS/RPC, pairing import, replay, screen/input and disconnect cleanup' + ('' if a.core_only else ' and RAM reload/reconnect'));break
                if 'debug-net-guest: FAIL' in logs or 'PANIC' in logs or 'user fault' in logs:raise RuntimeError(logs[-4000:])
                time.sleep(.2)
            else:raise RuntimeError('Guest did not complete; see '+str(work/'stdout.log'))
        finally:
            emulator.terminate();emulator.wait(timeout=15);server.shutdown()
print(work)
