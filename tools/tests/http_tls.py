#!/usr/bin/env python3
"""Local-only HTTPS regression. No addon accounts or external requests."""
import http.server, os, socketserver, ssl, subprocess, tempfile, threading
from pathlib import Path
root=Path(__file__).resolve().parents[2]
folder=Path(tempfile.mkdtemp(prefix='coffeeflix-tls-'))
openssl='/opt/homebrew/opt/openssl@3/bin/openssl' if Path('/opt/homebrew/opt/openssl@3/bin/openssl').exists() else 'openssl'
def run(*args):subprocess.run([openssl,*args],cwd=folder,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
run('req','-x509','-newkey','rsa:2048','-nodes','-days','2','-subj','/CN=Local test CA','-keyout','ca.key','-out','ca.pem','-addext','basicConstraints=critical,CA:TRUE')
run('req','-newkey','rsa:2048','-nodes','-subj','/CN=localhost','-keyout','server.key','-out','server.csr')
(folder/'extensions').write_text('subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=critical,CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n')
run('x509','-req','-in','server.csr','-CA','ca.pem','-CAkey','ca.key','-CAcreateserial','-days','2','-extfile','extensions','-out','server.pem')
run('req','-x509','-newkey','rsa:2048','-nodes','-days','2','-subj','/CN=localhost','-keyout','untrusted.key','-out','untrusted.pem','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1')
(folder/'bad.pem').write_text('not a certificate\n')
class Handler(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_GET(self):
  self.send_response(200);self.send_header('Content-Length','7');self.end_headers();self.wfile.write(b'trusted')
class Closed(socketserver.BaseRequestHandler):
 def handle(self):
  self.request.recv(4096)
servers=[]
def server(cert,key):
 s=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
 ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain(folder/cert,folder/key)
 s.socket=ctx.wrap_socket(s.socket,server_side=True)
 threading.Thread(target=s.serve_forever,daemon=True).start();servers.append(s);return s.server_port
trusted=server('server.pem','server.key');untrusted=server('untrusted.pem','untrusted.key')
closed=socketserver.ThreadingTCPServer(('127.0.0.1',0),Closed)
threading.Thread(target=closed.serve_forever,daemon=True).start();servers.append(closed)
def url(port):return f'https://127.0.0.1:{port}/PRIVATE-ADDON-TOKEN/manifest.json?key=PRIVATE-QUERY-TOKEN'
try:
 result=subprocess.run([str(root/'build-desktop/http-tls-test'),str(folder/'ca.pem'),str(folder/'bad.pem'),url(trusted),url(closed.server_address[1]),url(untrusted),url(trusted)],cwd=root,capture_output=True,text=True,timeout=40)
 output=result.stdout+result.stderr
 assert 'PRIVATE-ADDON-TOKEN' not in output and 'PRIVATE-QUERY-TOKEN' not in output, 'Private URL leaked into diagnostics'
 assert '[private URL]' in output and 'TLS: curl 35' in output and 'TLS: curl 60' in output and 'TLS: curl 77' in output
 print(output)
 result.check_returncode()
 print('PASS private addon path/query redaction and distinct TLS diagnostics')
finally:
 for s in servers:s.shutdown();s.server_close()
