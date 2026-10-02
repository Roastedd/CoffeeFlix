#!/usr/bin/env python3
"""Requests for several chunks at once, and a busy server's answer read out (see spans_test.cpp).
Build with make -f desktop.mk -f tools/tests/spans.mk spans-tests; run from the repository root.
The fixture is an HTTP/1.1 server with a slow first byte and paced connections, like the media servers
a Wii U streams from; generated local data only.
"""
import http.server, socketserver, subprocess, sys, threading, time
from pathlib import Path

root = Path(__file__).resolve().parents[2]
SIZE = 16 << 20
data = (bytes(range(251)) * (SIZE // 251 + 1))[:SIZE]  # byte i is i % 251
lock = threading.Lock()
log = {'span': [], 'drop': [], 'busy': []}  # (start, end, client port, status)
state = {'dropped': 0, 'busy': 0}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *a):
        pass

    def do_GET(self):
        name = self.path.split('?')[0].lstrip('/').removesuffix('.bin')
        start, _, end = self.headers.get('Range', 'bytes=0-')[6:].partition('-')
        start, end = int(start or 0), min(int(end) if end else SIZE - 1, SIZE - 1)
        port = self.client_address[1]
        if name == 'busy':
            with lock:
                busy = state['busy'] < 3
                state['busy'] += busy
            if busy:
                body = b'<html>busy, try again</html>' * 150  # a real busy page: a few KB
                self.send_response(503)
                self.send_header('Retry-After', '1')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                with lock:
                    log['busy'].append((start, end, port, 503))
                return
        time.sleep(0.15 if name in ('span', 'drop') else 0)  # the wait for the first byte
        body = data[start:end + 1]
        cut = len(body)
        if name == 'drop':
            with lock:
                if cut >= 768 << 10 and state['dropped'] < 2:
                    state['dropped'] += 1
                    cut = 600 << 10
        self.send_response(206)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Range', f'bytes {start}-{end}/{SIZE}')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            sent = 0
            while sent < cut:
                piece = body[sent:min(cut, sent + (64 << 10))]
                self.wfile.write(piece)
                sent += len(piece)
                if name in ('span', 'drop'):
                    time.sleep(len(piece) / (3 << 20))  # 3 MB/s on each connection
        except (BrokenPipeError, ConnectionResetError):
            return
        with lock:
            log.setdefault(name, []).append((start, end, port, 206))
        if cut < len(body):
            self.close_connection = True


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    request_queue_size = 64


server = Server(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
base = f'http://127.0.0.1:{server.server_address[1]}'
binary = root / 'build-desktop' / 'spans-test'
proc = subprocess.run([str(binary), base], cwd=root, capture_output=True, text=True, timeout=300)
sys.stdout.write(proc.stdout)
sys.stderr.write(proc.stderr)
assert proc.returncode == 0, f'spans-test failed ({proc.returncode})'
assert 'Spans test passed' in proc.stdout

# What the server saw: the requests covered several chunks, and stayed under the most asked for.
spans = log['span']
sizes = [e - s + 1 for s, e, _, _ in spans]
print(f"span.bin: {len(sizes)} answered requests, average {sum(sizes) / len(sizes) / 1024:.0f} KB, largest {max(sizes) / 1024:.0f} KB")
assert sum(sizes) / len(sizes) > 512 << 10, 'requests did not cover several chunks'
assert max(sizes) <= 8 * (256 << 10), 'a request covered more than 8 chunks'
assert state['dropped'] >= 1, 'no long request was cut (spans never grew)'

# With http_io_back_off_all(), a busy answer with a body keeps its connection: the ports that got a
# 503 served data later (a connection closed with the body unread would have been replaced by one on
# a new port). Without it the connection is closed at once, as it was before: only the retries count.
refused = {p for _, _, p, st in log['busy'] if st == 503}
served = {p for _, _, p, st in log['busy'] if st == 206}
print('busy.bin: 503 on ports', sorted(refused), '; data on ports', sorted(served))
assert len([1 for e in log['busy'] if e[3] == 503]) == 3
if 'back_off_all: 1' in proc.stdout:
    assert refused & served, 'the connection that was told "busy" was not used again'
print('Spans tests passed')
