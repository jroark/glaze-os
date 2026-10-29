#!/usr/bin/env python3
"""Paired network transport tested against the production device TLS code."""
from pathlib import Path
import os, socket, struct, subprocess, tempfile, time, unittest
ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / 'out/host/glaze-usb'

class NetworkDebugTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='glaze-network-debug-')
        cls.root = Path(cls.tmp.name)
        cls.key = cls.root/'DEBUG.KEY'
        cls.bad = cls.root/'wrong.key'
        for path in (cls.key, cls.bad):
            subprocess.run([str(CLI),'net-keygen',str(path)],check=True,capture_output=True)
        (cls.root/'debug.network').write_bytes(cls.key.read_bytes())
        cls.enable(False)
        cls.log = (cls.root/'server.log').open('w+')
        cls.server = subprocess.Popen([str(ROOT/'out/host/debug-net-fixture'),str(cls.root)],stdout=cls.log,stderr=cls.log,
                                      env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
        time.sleep(.3)
        if cls.server.poll() is not None:
            cls.log.seek(0); raise RuntimeError(cls.log.read())

    @classmethod
    def tearDownClass(cls):
        cls.server.terminate();cls.server.wait(timeout=5)
        cls.log.seek(0); logs=cls.log.read();cls.log.close();cls.tmp.cleanup()
        (ROOT/"out/network-debug-native-server.log").write_text(logs)
        if 'runtime error:' in logs or 'ERROR: AddressSanitizer' in logs: raise AssertionError(logs)

    @classmethod
    def enable(cls,on):
        (cls.root/'debug.enabled').write_bytes(struct.pack('<I',bool(on)))

    def cli(self,*args,key=None):
        return subprocess.run([str(CLI),'--host','127.0.0.1','--key',str(key or self.key),*args],capture_output=True,text=True,timeout=10)

    def test_01_disabled_by_default(self):
        with socket.socket() as s: self.assertNotEqual(s.connect_ex(('127.0.0.1',3000)),0)
        self.enable(True);time.sleep(1.5)

    def test_02_private_key_and_exclusive_creation(self):
        self.assertEqual(self.key.stat().st_mode&0o777,0o600)
        before=self.key.read_bytes()
        result=subprocess.run([str(CLI),'net-keygen',str(self.key)],capture_output=True)
        self.assertNotEqual(result.returncode,0);self.assertEqual(before,self.key.read_bytes())
        self.bad.chmod(0o644)
        self.assertNotEqual(self.cli('probe',key=self.bad).returncode,0)
        self.bad.chmod(0o600)

    def test_03_wrong_key_rejected(self):
        self.assertNotEqual(self.cli('probe',key=self.bad).returncode,0)
        time.sleep(1.2)
        result=self.cli('probe');self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('native TLS fixture',result.stderr)

    def test_04_plaintext_rejected(self):
        time.sleep(1.2)
        with socket.create_connection(('127.0.0.1',3000),timeout=2) as s:
            s.sendall(b'GZUB'+bytes(40));s.settimeout(6)
            try: result=s.recv(100)
            except ConnectionResetError: result=b''
            self.assertNotIn(b'GZUB',result)
        time.sleep(1.2)
        result=self.cli('probe');self.assertEqual(result.returncode,0,result.stderr)

    def test_05_seed_ratchet_and_ordered_frames(self):
        before=(self.root/'debug.network').read_bytes()
        time.sleep(1.2)
        r=subprocess.run([str(ROOT/'out/host/debug-net-client'),str(self.key)],capture_output=True,text=True,timeout=15)
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertIn('PASS',r.stdout)
        after=(self.root/'debug.network').read_bytes()
        self.assertEqual(before[:32],after[:32]);self.assertNotEqual(before[32:],after[32:])

    def test_06_disable_closes_active_session(self):
        time.sleep(1.2)
        p=subprocess.Popen([str(CLI),'--host','127.0.0.1','--key',str(self.key),'connect'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        time.sleep(.5);self.assertIsNone(p.poll());self.enable(False)
        p.communicate(timeout=5)
        with socket.socket() as s: self.assertNotEqual(s.connect_ex(('127.0.0.1',3000)),0)

    def test_07_key_rotation_rejects_old_pairing(self):
        (self.root/'debug.network').write_bytes(self.bad.read_bytes())
        self.enable(True);time.sleep(1.6)
        self.assertNotEqual(self.cli('probe').returncode,0)
        time.sleep(1.2)
        result=self.cli('probe',key=self.bad)
        self.assertEqual(result.returncode,0,result.stderr)

    def test_08_invalid_configuration_fails_closed(self):
        (self.root/'debug.network').write_bytes(b'invalid')
        time.sleep(.7)
        with socket.socket() as s:self.assertNotEqual(s.connect_ex(('127.0.0.1',3000)),0)

if __name__=='__main__':unittest.main(verbosity=2)
