#!/usr/bin/env python3
"""Local HTTP fixtures for scripts, CSP, frames, redirects and named targets."""
import http.server
import subprocess
import sys
import threading

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        headers = {}
        pages = {
            '/index': '''<title>Browser test</title><p id=out>old</p>
<iframe id=frame name=child src=/child width=240 height=80></iframe>
<iframe id=denied src=/denied></iframe>
<iframe id=sandbox sandbox srcdoc="<p>private</p><script>throw Error('sandbox');</script>"></iframe>
<iframe src=/cycle></iframe>
<script>var order=['inline'];document.addEventListener('DOMContentLoaded',function(){order.push('ready');});window.addEventListener('load',function(){order.push('load');});</script>
<script src=/external.js></script>
<script>order.push('after');</script>''',
            '/external.js': '''order.push('external');document.getElementById('out').textContent='external';document.write('<script>order.push("written");<'+ '/script>');''',
            '/child': '''<p id=child>child</p><p style="height:400px">long frame</p><script>document.getElementById('child').textContent='child JS';</script>''',
            '/denied': '<p>must not embed</p>',
            '/cycle': '<iframe src=/index></iframe>',
            '/next': '<p id=next>navigated</p>',
            '/frameset': '<frameset cols="50%,*"><frame src=/child><frame src=/next></frameset>',
            '/csp': '''<title>initial</title><script>var forbidden=true;</script><script src=/allowed.js></script>''',
            '/allowed.js': "document.title='external allowed';",
            '/noscript': '''<head><noscript><meta http-equiv=refresh content="0;url=/enablejs">
<base href="http://wrong.test/"><link rel=stylesheet href=/fallback.css>
<script src=/fallback.js></script></noscript>
<script>document.title='neighbor ran';document.write('<noscript><meta http-equiv=refresh content="0;url=/written"><script>throw Error("inert");<'+ '/script></noscript>');</script></head>
<body><noscript><img src=/fallback.png></noscript>
<iframe id=local sandbox srcdoc="<noscript><meta http-equiv=refresh content='0;url=/local'><p id=fallback>fallback</p></noscript>"></iframe>
<iframe id=remote sandbox src=/noscript-child></iframe></body>''',
            '/noscript-child': '''<head><noscript><meta http-equiv=refresh content="0;url=/remote"></noscript></head>
<body><noscript><p id=fallback>fallback</p></noscript><script>throw Error('sandbox');</script></body>''',
        }
        if self.path == '/denied':
            headers['X-Frame-Options'] = 'DENY'
        if self.path == '/csp':
            headers['Content-Security-Policy'] = "script-src 'self'"
        if self.path == '/limits' or self.path.startswith('/tree/'):
            depth = 0 if self.path == '/limits' else int(self.path.split('/')[-1])
            pages[self.path] = f'<iframe src="/tree/{depth+1}"></iframe><iframe src="/tree/{depth+2}"></iframe>'
        body = pages.get(self.path, '<p>not found</p>').encode()
        self.send_response(200 if self.path in pages else 404)
        self.send_header('Content-Type', 'application/javascript' if self.path.endswith('.js') else 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        for k, v in headers.items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    subprocess.run([sys.argv[1], f'http://127.0.0.1:{server.server_port}'], check=True, timeout=30)
finally:
    server.shutdown()
