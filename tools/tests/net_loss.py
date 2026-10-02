#!/usr/bin/env python3
"""A download that breaks mid-file: waited out (flaky.mp4) or given up on as an error, not a finish (dead.mp4).
Build with make -f desktop.mk -f tools/tests/net_loss.mk net-loss-tests. Needs ffmpeg; generated local media only.
"""
import http.server, os, subprocess, tempfile, threading, time, json, sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
folder = Path(tempfile.mkdtemp(prefix='coffeeflix-net-loss-'))
print('Artifacts:', folder, flush=True)
episode = '--episode' in sys.argv
sample = folder / ('sample.mkv' if episode else 'sample.mp4')
command = ['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
           'testsrc2=size=1920x1080:rate=24' if episode else 'testsrc2=size=320x180:rate=24',
           '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000', '-t', '12' if episode else '24',
           '-vf', 'noise=alls=25:allf=t', '-c:v', 'libx264', '-preset', 'ultrafast' if episode else 'medium',
           '-b:v', '2400k' if episode else '1200k', '-maxrate', '2400k' if episode else '1200k',
           '-bufsize', '2400k' if episode else '1200k', '-pix_fmt', 'yuv420p', '-g', '24',
           '-c:a', 'eac3' if episode else 'aac']
if not episode: command += ['-movflags', '+faststart']
subprocess.run(command + [str(sample)], check=True)
media = sample.read_bytes()
print('media bytes', len(media), flush=True)
packets=json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','v','-show_packets','-show_entries','packet=pos,size,pts_time,flags','-of','json',str(sample)]))['packets']
bad_packet=next(p for p in packets if float(p.get('pts_time',0))>5 and 'K' not in p.get('flags',''))
bad_at=int(bad_packet['pos']);bad_size=int(bad_packet['size'])
damaged=bytearray(media);damaged[bad_at:bad_at+4]=(bad_size+100).to_bytes(4,'big')
repair_sent=False
repair_lock=threading.Lock()
FROM = 1_000_000
CUT = 1_500_000
state = {'armed': 0.0, 'overload': 0}

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def do_GET(self):
        start, _, end = self.headers.get('Range', 'bytes=0-')[6:].partition('-')
        start, end = int(start or 0), min(int(end) if end else len(media) - 1, len(media) - 1)
        if self.path in ('/flaky.mp4', '/dead.mp4') and start >= FROM:
            if self.path == '/flaky.mp4':
                if not state['armed']: state['armed'] = time.monotonic()
                down = time.monotonic() - state['armed'] < 7
            else: down = True
            if down:
                self.send_response(500); self.send_header('Content-Length', '0'); self.end_headers(); return
        if self.path == '/episode.mkv' and start >= FROM:
            with repair_lock:
                overload = state['overload'] == 0
                if overload: state['overload'] += 1
            if overload:
                self.send_response(503); self.send_header('Content-Length', '0'); self.end_headers(); return
        global repair_sent
        payload=media
        if self.path == '/badframe.mp4': payload=damaged
        if self.path == '/repair.mp4' and start<=bad_at<end+1:
            with repair_lock:
                if not repair_sent: payload=damaged;repair_sent=True
        body = payload[start:end + 1]
        if self.path == '/cut.mp4': body = body[:max(0, CUT - start)] # the connection closes early, whatever was asked for
        cut = self.path == '/cut.mp4' and end + 1 > CUT
        self.send_response(206)
        self.send_header('Content-Type', 'video/mp4')
        self.send_header('Content-Length', str(end - start + 1 if cut else len(body)))
        self.send_header('Content-Range', f'bytes {start}-{end}/{len(media)}')
        self.end_headers()
        try:
            self.wfile.write(body)
            if cut: self.wfile.flush(); self.close_connection = True
        except (BrokenPipeError, ConnectionResetError): pass

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
base = f'http://127.0.0.1:{server.server_port}'
with (folder / 'net-loss.log').open('w') as log:
    try:
        subprocess.run([str(root / 'build-desktop/net-loss-test'), base, str(folder)] + (['--episode'] if episode else []), cwd=root,
                       env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=330)
    except subprocess.CalledProcessError:
        print((folder / 'net-loss.log').read_text()[-6000:]); raise
if episode: assert state['overload'] == 1
print((folder / 'net-loss.log').read_text()[-1500:])
print('Connection loss regression passed')
