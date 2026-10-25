#!/usr/bin/env python3
"""Boot disposable guest images against the secured virtual AP fixtures.
Build first: ./build.sh WL54CF=1 INIT=bin/ktest; make -C host wifi-mkfs
"""
import argparse, hashlib, ipaddress, os, re, secrets, shutil, signal, socket, struct
import subprocess, threading, time
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emulator',type=Path,required=True)
    parser.add_argument('--host-ip',type=ipaddress.IPv4Address,required=True)
    parser.add_argument('--cases',default='xi825-open,xi825-wep,xi825-wep104,xi825-legacy,wl54cf-open,wl54cf-wep,wl54cf-wep104,wl54cf-wpa,wl54cf-wpa2,ne2000,none,cf')
    parser.add_argument('--seconds',type=int,default=90)
    parser.add_argument('--workers',type=int,default=2)
    args=parser.parse_args();root=Path(__file__).resolve().parents[1]
    directory=root/'out'/('wifi-security-'+time.strftime('%Y%m%d-%H%M%S'));directory.mkdir()
    source=root/'out/glaze.bin';before=hashlib.sha256(source.read_bytes()).hexdigest()
    blocks=int(re.search(r'#define GZ_FLASH_BLOCKS\s+(\d+)',(root/'lib/include/glaze/abi.h').read_text())[1])
    requests=[]
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            requests.append(self.path);body=b'<html>WIFI SECURITY HTTP PASS</html>'
            self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
        def log_message(self,*args):pass
    server=ThreadingHTTPServer((str(args.host_ip),0),Handler)
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    def boot(work,card,mode,label,wep104=False):
        env=dict(os.environ,SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy',BE300_WIFI_SECURITY=mode,
                 BE300_WIFI_KEY=('1234567890123' if wep104 else '12345') if mode=='wep' else 'password')
        options=[] if card=='none' else ['--cf','cf.img'] if card=='cf' else ['--'+card]
        with (work/(label+'.stdout.log')).open('w') as out,(work/(label+'.stderr.log')).open('w') as err:
            process=subprocess.Popen([str(args.emulator.resolve()),'--nand','nand.bin','--rtc-host-time']+options,cwd=work,env=env,stdout=out,stderr=err)
            try:
                end=time.monotonic()+args.seconds
                while process.poll() is None and time.monotonic()<end:
                    log=(work/(label+'.stdout.log')).read_text(errors='replace')
                    if re.search(r'ktest: (ALL PASS|FAILED)',log):break
                    time.sleep(.25)
            finally:
                if process.poll() is None:process.send_signal(signal.SIGTERM)
                process.wait(timeout=10)
        log=(work/(label+'.stdout.log')).read_text(errors='replace')
        if 'ktest: ALL PASS (0 failures)' not in log:raise RuntimeError(f'{work.name}/{label}: guest failed; see logs')
        if card not in ('none','cf') and 'net: DHCP address ' not in log:raise RuntimeError(f'{work.name}: no DHCP')
        if card in ('xi825','wl54cf'):
            number={'open':0,'wep':1,'wpa':2,'wpa2':3}[mode]
            if f'ktest: wifi security {number} connected' not in log:raise RuntimeError(f'{work.name}: security mismatch')
        print(work.name+'/'+label+': PASS',flush=True)
    def run(case):
        card,_,mode=case.partition('-');legacy=mode=='legacy';wep104=mode=='wep104'
        mode='open' if legacy else 'wep' if wep104 else mode or 'open';work=directory/case;work.mkdir()
        overrides=[]
        seed=work/'entropy';seed.write_bytes(secrets.token_bytes(32));overrides.append('/Library/Preferences/net.entropy='+str(seed))
        if card in ('xi825','wl54cf'):
            key=(b'1234567890123' if wep104 else b'12345') if mode=='wep' else b'' if mode=='open' else b'password'
            credentials=struct.pack('<I32sIIII64s',5,b'BE300',{'open':0,'wep':1,'wpa':2,'wpa2':3}[mode],len(key),0,0,key)
            pref=work/'preference'
            pref.write_bytes(struct.pack('<III32s',1,1,5,b'BE300') if legacy else struct.pack('<II',2,1)+credentials)
            overrides.append('/Library/Preferences/net.wifi='+str(pref))
        if card not in ('none','cf'):
            fixture=work/'http';fixture.write_bytes(args.host_ip.packed+struct.pack('<HH',server.server_port,0))
            overrides.append('/Library/Preferences/test.wifi.http='+str(fixture))
        fs=work/'fs.img'
        subprocess.run([str(root/'out/host/wifi-mkfs'),str(root/'userdata'),str(blocks),str(fs)]+overrides,check=True,stdout=subprocess.DEVNULL)
        nand=bytearray(source.read_bytes());nand[0x500000:0x500000+fs.stat().st_size]=fs.read_bytes();(work/'nand.bin').write_bytes(nand)
        if card=='cf':(work/'cf.img').write_bytes(bytes(8*1024*1024))
        boot(work,card,mode,'first',wep104)
        if card in ('xi825','wl54cf'):boot(work,card,mode,'reboot',wep104)
    try:
        with ThreadPoolExecutor(max_workers=args.workers) as pool:list(pool.map(run,args.cases.split(',')))
        if hashlib.sha256(source.read_bytes()).hexdigest()!=before:raise RuntimeError('source NAND changed')
        (directory/'source.sha256').write_text(before+'\n')
        print('All requested cases PASS; artifacts: '+str(directory))
    finally:server.shutdown();server.server_close();thread.join(timeout=5)
if __name__=='__main__':main()
