#!/usr/bin/env python3
"""Local transfer/TLS tests; no public network access required."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os
import gzip
import ssl
import subprocess
import tempfile
import threading
import time
import unittest
ROOT=Path(__file__).resolve().parents[1]
BINARY=ROOT/'out/host/store-http-test'
class Fixture(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_GET(self):
        self.protocol_version='HTTP/1.1'
        self.send_response(302 if self.path=='/redirect' else 200)
        if self.path=='/chunked':
            self.send_header('Transfer-Encoding','chunked'); self.end_headers()
            self.wfile.write(b'4\r\nhell\r\n1\r\no\r\n0\r\n\r\n'); return
        if self.path=='/badchunk':
            self.send_header('Transfer-Encoding','chunked'); self.end_headers(); self.wfile.write(b'nope\r\n'); return
        if self.path=='/gzip':
            body=gzip.compress(b'hello')
            self.send_header('Content-Encoding','gzip'); self.send_header('Content-Length',str(len(body)))
            self.end_headers(); self.wfile.write(body); return
        self.send_header('Content-Length', '-1' if self.path=='/badlength' else '100' if self.path in ('/short','/large','/cancel') else '5')
        self.end_headers()
        try:
            if self.path=='/cancel':
                self.wfile.write(b'hello'); self.wfile.flush(); time.sleep(.2); self.wfile.write(b'x'*95)
            else: self.wfile.write(b'x'*100 if self.path=='/large' else b'hello')
        except (BrokenPipeError,ConnectionResetError): pass
class Transfer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(); cls.directory=Path(cls.tmp.name)
        cls.server=ThreadingHTTPServer(('127.0.0.1',0),Fixture)
        threading.Thread(target=cls.server.serve_forever,daemon=True).start()
        cls.base=f'http://127.0.0.1:{cls.server.server_port}'
        cls.output=cls.directory/'download'
    @classmethod
    def tearDownClass(cls): cls.server.shutdown(); cls.server.server_close(); cls.tmp.cleanup()
    def fetch(self,path,limit=256,base=None,env=None,cancel=False,buffered=False):
        args=[str(BINARY),(base or self.base)+path,str(self.output),str(limit)]
        if cancel: args.append('cancel')
        elif buffered: args.append('buffered')
        return subprocess.run(args,env=env,capture_output=True,timeout=10)
    def test_framing(self):
        for endpoint in ('/ok','/chunked'):
            self.assertEqual(self.fetch(endpoint).returncode,0)
            self.assertEqual(self.output.read_bytes(),b'hello')
        for endpoint in ('/short','/large','/badlength','/badchunk','/gzip','/redirect'):
            self.assertNotEqual(self.fetch(endpoint,64).returncode,0,endpoint)
        self.assertEqual(self.fetch('/cancel',cancel=True).returncode,0)
        self.assertEqual(self.output.read_bytes(),b'hello')
    def test_buffered_browser_transport(self):
        for endpoint in ('/ok','/chunked','/gzip'):
            result=self.fetch(endpoint,buffered=True)
            self.assertEqual(result.returncode,0,result.stderr.decode())
            self.assertEqual(self.output.read_bytes(),b'hello')
    def test_tls(self):
        cert=self.directory/'cert.pem'; key=self.directory/'key.pem'; der=self.directory/'roots.der'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-days','2','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost'],check=True,capture_output=True)
        subprocess.run(['openssl','x509','-in',str(cert),'-outform','DER','-out',str(der)],check=True)
        server=ThreadingHTTPServer(('127.0.0.1',0),Fixture)
        ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert,key)
        server.socket=ctx.wrap_socket(server.socket,server_side=True)
        threading.Thread(target=server.serve_forever,daemon=True).start()
        try:
            base=f'https://localhost:{server.server_port}'
            env=dict(os.environ,STORE_TEST_ROOTS=str(der))
            result=self.fetch('/ok',base=base,env=env)
            self.assertEqual(result.returncode,0,result.stderr.decode())
            self.assertNotEqual(self.fetch('/ok',base=base).returncode,0)
            self.assertNotEqual(self.fetch('/ok',base=base.replace('localhost','127.0.0.1'),env=env).returncode,0)
        finally: server.shutdown(); server.server_close()
if __name__=='__main__': unittest.main()
