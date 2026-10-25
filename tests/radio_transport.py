#!/usr/bin/env python3
"""Loopback tests for the production radio worker, TLS and bounded buffer."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os
import ssl
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT/'out/host/radio-worker-test'
AUDIO = (ROOT/'userdata/Music/Demo.mp3').read_bytes()


class Fixture(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *args):
        pass
    def do_GET(self):
        try:
            self.respond()
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass
    def respond(self):
        if self.path.endswith('.pls'):
            if self.path == '/slow.pls':
                time.sleep(2)
            targets = ['/unavailable', '/redirect'] if self.path == '/fallback.pls' else ['/redirect']
            body = '[playlist]\n' + ''.join(f'File{i+1}=http://127.0.0.1:{self.server.server_port}{p}\n' for i,p in enumerate(targets))
            body = body.encode()
            self.send_response(200); self.send_header('Content-Length', str(len(body))); self.end_headers()
            for i in range(0,len(body),7):
                self.wfile.write(body[i:i+7]); self.wfile.flush()
        elif self.path == '/unavailable':
            self.send_response(503); self.send_header('Content-Length','0'); self.end_headers()
        elif self.path == '/redirect':
            self.send_response(302); self.send_header('Location','/stream'); self.send_header('Content-Length','0'); self.end_headers()
        else:
            self.send_response(200); self.send_header('Content-Type','audio/mpeg')
            self.send_header('icy-metaint','4096'); self.send_header('Transfer-Encoding','chunked'); self.end_headers()
            metadata = b"StreamTitle='Fixture Artist - Song';"
            metadata += b'\0' * (-len(metadata)%16)
            for i in range(2000):
                pos = i*4096
                block = bytes(AUDIO[(pos+j)%len(AUDIO)] for j in range(4096))
                block += bytes([len(metadata)//16]) + metadata
                self.wfile.write(f'{len(block):x}\r\n'.encode()+block+b'\r\n'); self.wfile.flush()
                time.sleep(.001)


class Radio(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server=ThreadingHTTPServer(('127.0.0.1',0),Fixture)
        threading.Thread(target=cls.server.serve_forever,daemon=True).start()
        cls.base=f'http://127.0.0.1:{cls.server.server_port}'
    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown(); cls.server.server_close()
    def run_worker(self, mode, url, *extra, env=None):
        subprocess.run([str(BINARY),mode,url,*extra],check=True,timeout=15,env=env)
    def test_chunked_metadata_redirect(self):
        self.run_worker('play',self.base+'/radio.pls')
    def test_alternate_server(self):
        self.run_worker('play',self.base+'/fallback.pls')
    def test_cancel_pending_playlist(self):
        self.run_worker('switch',self.base+'/slow.pls',self.base+'/radio.pls')
    def test_cancel_full_buffer(self):
        self.run_worker('full',self.base+'/radio.pls',self.base+'/radio.pls')
    def test_untrusted_certificate(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp=Path(tmp)
            subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(tmp/'key.pem'),
                '-out',str(tmp/'cert.pem'),'-days','1','-subj','/CN=localhost'],check=True,capture_output=True)
            subprocess.run(['openssl','x509','-in',str(tmp/'cert.pem'),'-outform','DER','-out',str(tmp/'cert.der')],check=True)
            server=ThreadingHTTPServer(('127.0.0.1',0),Fixture)
            context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); context.load_cert_chain(tmp/'cert.pem',tmp/'key.pem')
            server.socket=context.wrap_socket(server.socket,server_side=True)
            threading.Thread(target=server.serve_forever,daemon=True).start()
            try:
                # Trust the root but request an IP absent from its SAN: must reject.
                env=dict(os.environ,RADIO_TEST_ROOTS=str(tmp/'cert.der'))
                self.run_worker('error',f'https://127.0.0.1:{server.server_port}/radio.pls','3',env=env)
                # Correct hostname, but a chain outside the production trust store.
                env=dict(os.environ,RADIO_TEST_ROOTS=str(ROOT/'assets/ssl/roots.der'))
                self.run_worker('error',f'https://localhost:{server.server_port}/radio.pls','3',env=env)
            finally:
                server.shutdown(); server.server_close()


if __name__=='__main__':
    unittest.main()
