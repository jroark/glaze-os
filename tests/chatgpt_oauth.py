#!/usr/bin/env python3
"""Codex OAuth/refresh/SSE fixtures. No OpenAI account or real token is used.
--serve PORT starts the same fixture for a diagnostic Glaze package.
"""
import argparse
import base64
import contextlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import json
import os
from pathlib import Path
import ssl
import subprocess
import tempfile
import threading
import time
import unittest
from urllib.parse import parse_qs

ROOT = Path(__file__).resolve().parent.parent
CLIENT = "app_EMoamEEZ73f0CkXaXp7hrann"
ACCOUNT = "00000000-0000-4000-8000-000000000001"


def jwt(exp=None, account=ACCOUNT, **extra):
    claims = {"exp": exp if exp is not None else int(time.time()) + 3600,
              "https://api.openai.com/auth": {"chatgpt_account_id": account, "chatgpt_plan_type": "plus", **extra}}
    def enc(data): return base64.urlsafe_b64encode(json.dumps(data).encode()).decode().rstrip("=")
    return enc({"alg": "none", "typ": "JWT"}) + "." + enc(claims) + ".fixture-signature"


def grant(refresh="synthetic-refresh", **kwargs):
    return {"access_token": jwt(**kwargs), "refresh_token": refresh, "id_token": jwt(), "token_type": "Bearer"}


def auth_file(**kwargs):
    return {"auth_mode": "chatgpt", "OPENAI_API_KEY": None,
            "tokens": {**grant(**kwargs), "account_id": ACCOUNT,
                       "oauth_provenance": {"issuer": "https://auth.openai.com", "client_id": CLIENT}}}


class Fixture(ThreadingHTTPServer):
    daemon_threads = True
    def __init__(self, address):
        self.calls = []; self.mode = "ok"; self.polls = 0; self.responses = 0
        super().__init__(address, Handler)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *_args): pass
    def reply(self, status, data):
        data = json.dumps(data).encode()
        self.send_response(status); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data))); self.send_header("Connection", "close"); self.end_headers()
        try: self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError): pass
        self.close_connection = True

    def do_POST(self):
        raw = self.rfile.read(int(self.headers["Content-Length"]))
        form = self.headers.get_content_type() == "application/x-www-form-urlencoded"
        value = {k: v[0] for k, v in parse_qs(raw.decode(), strict_parsing=True).items()} if form else json.loads(raw)
        self.server.calls.append((self.path, dict(self.headers), value))
        mode = self.server.mode
        if self.path == "/api/accounts/deviceauth/usercode":
            if mode == "unavailable": return self.reply(404, {"error": "not_found"})
            if mode == "bad_device": return self.reply(200, {"device_auth_id": "fixture", "user_code": "ABCD-1234", "interval": "999999"})
            return self.reply(200, {"device_auth_id": "fixture-device", "user_code": "ABCD-1234", "interval": "1"})
        if self.path == "/api/accounts/deviceauth/token":
            self.server.polls += 1
            if mode == "denied": return self.reply(403, {"error": "access_denied"})
            if mode == "expired": return self.reply(400, {"error": "expired_token"})
            if self.server.polls == 1 and mode != "instant": return self.reply(403, {"error": "authorization_pending"})
            return self.reply(200, {"authorization_code": "code&with=symbols", "code_verifier": "v" * 43,
                                    "code_challenge": "published-by-auth-server"})
        if self.path == "/oauth/token":
            if value.get("grant_type") == "authorization_code": return self.reply(200, grant())
            if mode == "revoked": return self.reply(400, {"error": "invalid_grant"})
            if mode == "changed_account": return self.reply(200, grant("rotated-refresh", account="other-account"))
            return self.reply(200, grant("rotated-refresh"))
        if self.path == "/backend-api/codex/responses":
            self.server.responses += 1
            if mode == "reauth" and self.server.responses == 1: return self.reply(401, {"error": "expired"})
            if mode == "quota": return self.reply(429, {"error": "usage_limit"})
            if mode == "no_access": return self.reply(403, {"error": "not_entitled"})
            if mode == "slow": time.sleep(3)
            def event(value): return b"data: " + json.dumps(value, ensure_ascii=False).encode() + b"\r\n\r\n"
            text = "Hello, caf\u00e9!\nA direct Codex reply."
            if mode == "long": text = "a" * 10000
            data = b": heartbeat\r\n\r\n"
            if mode != "fallback":
                data += event({"type": "response.output_text.delta", "delta": text})
            if mode == "malformed": data += b"data: {bad\n\n"
            elif mode != "cutoff":
                data += event({"type": "response.failed" if mode == "failed" else "response.completed",
                               "response": {"status": "completed", "output": [{"type": "message", "content": [{"type": "output_text", "text": text}]}]}})
            self.send_response(200); self.send_header("Content-Type", "text/event-stream")
            self.send_header("Transfer-Encoding", "chunked"); self.send_header("Connection", "close"); self.end_headers()
            try:
                # Split headers, UTF-8, JSON escapes and SSE separators over chunks.
                for offset in range(0, len(data), 17):
                    chunk = data[offset:offset + 17]
                    self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
                self.wfile.write(b"0\r\n\r\n"); self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError): pass
            self.close_connection = True
            return
        self.reply(404, {"error": "unexpected_path"})


@contextlib.contextmanager
def running(server):
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    try: yield server
    finally: server.shutdown(); server.server_close(); thread.join(timeout=3)


class OAuthTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Fixture(("127.0.0.1", 0)); cls.manager = running(cls.server); cls.manager.__enter__()
        cls.addClassCleanup(cls.manager.__exit__, None, None, None)
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name); self.file = self.work / "CODEX.AUTH"
        self.file.write_text(json.dumps(auth_file()))
        self.server.mode = "ok"; self.server.calls.clear(); self.server.polls = self.server.responses = 0
    def native(self, action="chat", ok=True, base=None, **env):
        result = subprocess.run([str(ARGS.client.resolve()), base or f"http://127.0.0.1:{self.server.server_port}", action, str(self.file)],
            env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0", CX_TEST_STORE=str(self.work / "saved"), **env),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=25)
        output = result.stdout.decode("latin-1")
        self.assertEqual(result.returncode == 0, ok, output)
        self.assertNotIn("synthetic-refresh", output)
        self.assertNotIn("rotated-refresh", output)
        return output
    def test_device_code_and_direct_chat(self):
        out = self.native("device")
        self.assertIn("device-code-ready", out); self.assertIn("Hello, caf\xe9!", out)
        self.assertEqual(self.server.polls, 2)
        exchange = next(call for call in self.server.calls if call[0] == "/oauth/token")
        self.assertEqual(exchange[1]["Content-Type"], "application/x-www-form-urlencoded")
        self.assertEqual(exchange[2]["code"], "code&with=symbols")
        self.assertEqual(exchange[2]["code_verifier"], "v" * 43)
        self.assertEqual(exchange[2]["redirect_uri"], "https://auth.openai.com/deviceauth/callback")
        self.assertEqual(exchange[2]["client_id"], CLIENT)
        _, headers, request = self.server.calls[-1]
        self.assertEqual(headers["ChatGPT-Account-Id"], ACCOUNT)
        self.assertEqual(headers["Accept"], "text/event-stream")
        self.assertEqual(headers["Accept-Encoding"], "identity")
        self.assertTrue(headers["Authorization"].startswith("Bearer ey"))
        self.assertFalse(request["store"]); self.assertTrue(request["stream"])
        self.assertEqual(request["tools"], []); self.assertNotIn("max_output_tokens", request)
        self.assertEqual(request["input"][0]["content"][0]["text"], "Hello, Glaze!")
    def test_browser_codex_import(self):
        self.assertIn("signed_in=1", self.native("import")); self.assertFalse(self.server.calls)
    def test_expired_token_refresh_rotation(self):
        self.file.write_text(json.dumps(auth_file(exp=int(time.time()) - 10)))
        self.native()
        self.assertEqual(self.server.calls[0][0], "/oauth/token")
        self.assertEqual(self.server.calls[0][2]["grant_type"], "refresh_token")
        self.assertEqual(self.server.calls[0][2]["refresh_token"], "synthetic-refresh")
        self.assertIn(b"rotated-refresh\0", (self.work / "saved").read_bytes())
        stored = (self.work / "saved").read_bytes()
        refresh = stored[8 + 8192:8 + 8192 + 4096]
        self.assertEqual(set(refresh[len(b"rotated-refresh\0"):]), {0})
    def test_401_refreshes_once(self):
        self.server.mode = "reauth"; self.native()
        self.assertEqual(self.server.responses, 2)
        self.assertEqual(sum(p == "/oauth/token" for p, _, _ in self.server.calls), 1)
    def test_revoked_refresh_clears_saved_credentials(self):
        self.server.mode = "revoked"; self.file.write_text(json.dumps(auth_file(exp=1)))
        self.assertIn("Sign-in expired", self.native(ok=False))
        self.assertEqual(set((self.work / "saved").read_bytes()), {0})
        self.assertEqual(self.server.responses, 0)
    def test_account_change_and_untrusted_import_rejected(self):
        self.server.mode = "changed_account"; self.file.write_text(json.dumps(auth_file(exp=1)))
        self.native(ok=False); self.assertEqual(self.server.responses, 0)
        for change in ("issuer", "account", "api_key", "duplicate", "fedramp"):
            value = auth_file()
            if change == "issuer": value["tokens"]["oauth_provenance"]["issuer"] = "https://attacker.invalid"
            if change == "account": value["tokens"]["account_id"] = "wrong-account"
            if change == "api_key": value["OPENAI_API_KEY"] = "synthetic-api-key"
            if change == "fedramp": value["tokens"]["access_token"] = jwt(chatgpt_account_is_fedramp=True)
            data = json.dumps(value)
            if change == "duplicate": data = data.replace('"access_token":', '"access_token":"bad","access_token":')
            self.file.write_text(data); self.native("import", ok=False)
    def test_expiry_denial_bad_response_and_cancellation(self):
        for mode in ("denied", "expired", "bad_device", "unavailable"):
            self.server.mode = mode; self.native("device", ok=False)
        self.server.mode = "ok"; self.server.polls = 0
        self.native("device", ok=False, CX_TEST_CANCEL_MS="500")
        self.assertFalse((self.work / "saved").exists())
    def test_stream_terminal_validation_and_fallback(self):
        for mode in ("cutoff", "failed", "malformed"):
            self.server.mode = mode; self.native(ok=False)
        self.server.mode = "fallback"; self.assertIn("direct Codex reply", self.native())
        self.server.mode = "long"; self.assertIn("[Reply shortened]", self.native())
    def test_plan_and_usage_errors_do_not_retry(self):
        for mode, message in (("quota", "usage limit"), ("no_access", "cannot use")):
            self.server.mode = mode; self.server.responses = 0
            self.assertIn(message, self.native(ok=False)); self.assertEqual(self.server.responses, 1)
    def test_https_certificate_and_hostname(self):
        cert, key, der = self.work / "cert.pem", self.work / "key.pem", self.work / "cert.der"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=Codex fixture",
            "-addext", "subjectAltName=IP:127.0.0.1", "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True)
        subprocess.run(["openssl", "x509", "-in", str(cert), "-outform", "DER", "-out", str(der)], check=True)
        server = Fixture(("127.0.0.1", 0)); context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        with running(server):
            self.native(base=f"https://127.0.0.1:{server.server_port}", CHATGPT_ROOTS=str(der))
            self.native(base=f"https://localhost:{server.server_port}", ok=False, CHATGPT_ROOTS=str(der))
    def test_official_cli_export_helper(self):
        output = self.work / "export/CODEX.AUTH"
        result = subprocess.run(["python3", str(ROOT / "tools/chatgpt_signin.py"), "--from-auth", str(self.file), "--output", str(output)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(output.stat().st_mode & 0o777, 0o600)
        self.file = output; self.native("import")
        self.assertNotIn("synthetic-refresh", result.stdout + result.stderr)
        again = subprocess.run(["python3", str(ROOT / "tools/chatgpt_signin.py"), "--from-auth", str(self.file), "--output", str(output)], capture_output=True)
        self.assertNotEqual(again.returncode, 0)
        for invalid in (json.dumps({**auth_file(), "OPENAI_API_KEY": "synthetic-api-key"}),
                        json.dumps(auth_file()).replace('"access_token":', '"access_token":"bad","access_token":')):
            bad = self.work / "bad.json"; bad.write_text(invalid)
            rejected = subprocess.run(["python3", str(ROOT / "tools/chatgpt_signin.py"), "--from-auth", str(bad),
                "--output", str(self.work / "bad-export.AUTH")], capture_output=True, text=True)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertFalse((self.work / "bad-export.AUTH").exists())
            self.assertNotIn("synthetic-api-key", rejected.stdout + rejected.stderr)
            self.assertNotIn("synthetic-refresh", rejected.stdout + rejected.stderr)

    def test_browser_and_device_cli_use_dedicated_login(self):
        executable = self.work / "fake-codex"
        marker = self.work / "invocation.json"
        executable.write_text("#!/usr/bin/env python3\nimport os,sys,json\nfrom pathlib import Path\n"
            + f"value={auth_file()!r}\n"
            + "home=Path(os.environ['CODEX_HOME'])\n"
            + "(home/'auth.json').write_text(json.dumps(value))\n"
            + f"Path({str(marker)!r}).write_text(json.dumps([str(home),sys.argv[1:]]))\n")
        executable.chmod(0o700)
        for device in (False, True):
            output = self.work / ("device.AUTH" if device else "browser.AUTH")
            result = subprocess.run(["python3", str(ROOT / "tools/chatgpt_signin.py"), "--codex", str(executable),
                "--output", str(output)] + (["--device-code"] if device else []), capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            home, arguments = json.loads(marker.read_text())
            self.assertFalse(Path(home).exists())
            self.assertEqual(arguments[:3], ["-c", 'cli_auth_credentials_store="file"', "login"])
            self.assertEqual("--device-auth" in arguments, device)
            self.file = output; self.native("import")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", type=Path); parser.add_argument("--serve", type=int)
    parser.add_argument("--listen", default="127.0.0.1", help="fixture interface; use the host LAN address for an emulator")
    ARGS = parser.parse_args()
    if ARGS.serve:
        with Fixture((ARGS.listen, ARGS.serve)) as server:
            print(f"Synthetic Codex fixture on {ARGS.listen}:{ARGS.serve}", flush=True)
            try: server.serve_forever()
            except KeyboardInterrupt: pass
    elif ARGS.client:
        unittest.main(argv=[__file__], verbosity=2)
    else: parser.error("provide --client or --serve")
