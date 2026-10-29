#!/usr/bin/env python3
"""Local HTTP fixtures for scripts, CSP, frames, redirects and named targets."""
import http.server
import base64
import json
import subprocess
import sys
import threading
import tempfile
import os
import pathlib
import time

class Handler(http.server.BaseHTTPRequestHandler):
    cookie_checked = False
    forbidden_requests = []
    def log_message(self, *_):
        pass

    def do_POST(self):
        if self.path == '/api-no-preflight': Handler.forbidden_requests.append(self.path)
        body=self.rfile.read(int(self.headers.get('Content-Length', '0')))
        self.send_response(200)
        self.send_header('Content-Type',self.headers.get('Content-Type','text/plain'))
        self.send_header('Content-Length',str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        headers = {}
        if self.path == '/api-cross-target': Handler.forbidden_requests.append(self.path)
        if self.path == '/api-slow': time.sleep(0.2)
        pages = {
            '/device': '<title>Network APIs</title><p id=result>Checking network APIs...</p><script src=/device-test.js></script>',
            '/device-test.js': (pathlib.Path(__file__).parent/'web/browser_device.js').read_text(),
            '/api-page': '<script src=/fetch-test.js></script>',
            '/fetch-test.js': (pathlib.Path(__file__).parent/'web/browser_fetch.js').read_text(),
            '/api-json': json.dumps({'text':'café 😀'}),
            '/api-cors': json.dumps({'origin':self.headers.get('Origin')}),
            '/api-cors-credentials': json.dumps({'origin':self.headers.get('Origin')}),
            '/api-large': 'x'*65537,
            '/modules': """<script type=module>import {answer} from './module/a.js';
if(answer!==42)throw Error('module dependency');globalThis.moduleAnswer=answer;</script>
<script type=module src=/module/entry.js></script><script type=module src=/module/entry.js></script>
<script src=/module/classic.js></script>
<script>var dynamicDone=false,dynamicError='';import('./module/dynamic.js').then(function(ns){dynamicDone=ns.value===43;},function(e){dynamicError=String(e);});</script>""",
            '/module/a.js': "import {fnB} from './b.js';export function fnA(){return 40;}export const answer=fnB();",
            '/module/b.js': "import {fnA} from './a.js';export function fnB(){return fnA()+2;}",
            '/module/entry.js': "import {answer} from './a.js';globalThis.entryRuns=(globalThis.entryRuns||0)+1;globalThis.metaURL=import.meta.url;",
            '/module/classic.js': "var externalDynamicDone=false;import('./dynamic.js').then(function(m){externalDynamicDone=m.value===43;});",
            '/module/nested/entry.js': "import {answer} from '../a.js';globalThis.redirectModule=answer;",
            '/module-nonce': "<script type=module nonce=test>import {answer} from './module/a.js';globalThis.nonceModule=answer;</script>",
            '/module-redirect': '<script type=module src=/module-redirect-start></script>',
            '/module-limit': '<script type=module src=/module/too-many.js></script><script>globalThis.afterModuleLimit=true;</script>',
            '/module/too-many.js': ''.join(f"import './leaf{i}.js';\n" for i in range(32))+'globalThis.overLimitExecuted=true;',
            '/module/dynamic.js': "import {answer} from './a.js';export const value=answer+1;",
            '/module-fail': "<script type=module src=/module/bad-mime.js></script><script>globalThis.afterBadModule=true;</script>",
            '/module/bad-mime.js': "globalThis.badMimeExecuted=true;export const x=1;",
            '/module-bare': "<script type=module>import x from 'package';globalThis.bareExecuted=true;</script><script>globalThis.afterBare=true;</script>",
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
            '/platform': '''<script>var imageLoaded=false,imageFailed=false,pixel=new Image(1,1);
pixel.onload=function(){imageLoaded=true};pixel.src='/pixel.gif';
var broken=new Image();broken.onerror=function(){imageFailed=true};broken.src='/missing-image';
document.cookie='shared=parent; Path=/';</script>
<iframe id=cookie-child srcdoc="<script>if(document.cookie.indexOf('shared=parent')>=0)document.title='inherited cookie';</script>"></iframe>''',
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
        if self.path.startswith('/module/leaf'):
            pages[self.path]='export const value=1;'
        if self.path == '/module-nonce': headers['Content-Security-Policy']="script-src 'nonce-test'"
        if self.path == '/module-redirect-start':
            self.send_response(302)
            self.send_header('Location','/module/nested/entry.js')
            self.send_header('Content-Length','0')
            self.end_headers()
            return
        if self.path == '/api-redirect' or self.path == '/api-cross-redirect':
            self.send_response(302)
            self.send_header('Location', '/api-json' if self.path == '/api-redirect' else f'http://localhost:{self.server.server_port}/api-cross-target')
            self.send_header('Content-Length','0')
            self.end_headers()
            return
        if self.path == '/api-cors': headers['Access-Control-Allow-Origin']='*'
        if self.path == '/api-cors-credentials':
            headers['Access-Control-Allow-Origin']=self.headers.get('Origin','')
            headers['Access-Control-Allow-Credentials']='true'
        if self.path == '/api-wrong-origin': headers['Access-Control-Allow-Origin']='http://wrong.test'
        if self.path == '/api-duplicate-origin': headers['Access-Control-Allow-Origin']='*'
        if self.path == '/api-connect-denied':
            pages[self.path]='<script>globalThis.cspDenied=false;fetch("/api-json").catch(function(){cspDenied=true;});</script>'
            headers['Content-Security-Policy']="script-src 'unsafe-inline'; connect-src 'none'"
        if self.path == '/denied':
            headers['X-Frame-Options'] = 'DENY'
        if self.path == '/navigator':
            expected = json.dumps({'userAgent': self.headers.get('User-Agent'),
                                   'language': self.headers.get('Accept-Language')})
            pages[self.path] = '''<script>var expected=''' + expected + ''';
if(navigator.userAgent!==expected.userAgent||navigator.language!==expected.language)
  throw Error('navigator disagrees with request headers');
document.title='navigator matches headers';</script>'''
        if self.path == '/cookie-check':
            cookies = self.headers.get('Cookie', '')
            Handler.cookie_checked = 'httpOnly=preserved' in cookies and 'scriptCookie=sent' in cookies and 'overwritten' not in cookies
        if self.path == '/platform':
            headers['Set-Cookie'] = 'httpOnly=preserved; Path=/; HttpOnly'
        if self.path == '/csp':
            headers['Content-Security-Policy'] = "script-src 'self'"
        if self.path == '/limits' or self.path.startswith('/tree/'):
            depth = 0 if self.path == '/limits' else int(self.path.split('/')[-1])
            pages[self.path] = f'<iframe src="/tree/{depth+1}"></iframe><iframe src="/tree/{depth+2}"></iframe>'
        body = pages.get(self.path, '<p>not found</p>').encode()
        if self.path == '/pixel.gif':
            body = base64.b64decode('R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7')
            pages[self.path] = ''
        self.send_response(200 if self.path in pages else 404)
        ctype='application/javascript' if self.path.endswith('.js') else 'text/html; charset=utf-8'
        if self.path == '/module/bad-mime.js': ctype='text/plain'
        if self.path.startswith('/api-') and self.path not in ('/api-page','/api-connect-denied'): ctype='application/json'
        self.send_header('Content-Type',ctype)
        if self.path == '/api-duplicate-origin': self.send_header('Access-Control-Allow-Origin','*')
        self.send_header('Content-Length', str(len(body)))
        for k, v in headers.items():
            self.send_header(k, v)
        self.end_headers()
        try: self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError): pass

def main():
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory() as storage:
            env=dict(os.environ,BROWSE_STORAGE=storage)
            subprocess.run([sys.argv[1], '--storage-write'], check=True, timeout=10, env=env)
            subprocess.run([sys.argv[1], '--storage-read'], check=True, timeout=10, env=env)
            subprocess.run([sys.argv[1], f'http://127.0.0.1:{server.server_port}'], check=True, timeout=60, env=env)
        assert not Handler.forbidden_requests, Handler.forbidden_requests
        assert Handler.cookie_checked, 'script cookies must reach HTTP without overwriting HttpOnly cookies'
    finally:
        server.shutdown()

if __name__ == '__main__':
    main()
