#!/usr/bin/env python3
"""Closing the app while a video plays: the watchdog ends a stuck process, and the player closes in bounded time
even when the server holds a download open without answering. Build with
make -f desktop.mk -f tools/tests/exit.mk exit-tests. Needs ffmpeg; generated local media only.
"""
import http.server, os, subprocess, sys, tempfile, threading, time
from pathlib import Path

root = Path(__file__).resolve().parents[2]
binary = str(root / 'build-desktop/exit-test')
env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')

def run(args, timeout, expect_code=0):
    began = time.monotonic()
    p = subprocess.run([binary, *args], cwd=root, env=env, capture_output=True, text=True, timeout=timeout)
    took = time.monotonic() - began
    print(p.stdout.strip(), flush=True)
    print(p.stderr.strip()[-1200:], flush=True)
    assert p.returncode == expect_code, f'{args[0]}: exit code {p.returncode}, wanted {expect_code}'
    return took, p

# The watchdog: a hung process ends by itself soon after its deadline (0.5 s plus the moment it gives the log)...
took, p = run(['watchdog-stuck'], 20)
assert 0.4 < took < 5, f'ended after {took:.1f} s'
print(f'PASS the watchdog ended a stuck process after {took:.1f} s', flush=True)
# ...and once disarmed it leaves the process alone.
took, p = run(['watchdog-clean'], 20)
assert 'PASS a disarmed watchdog' in p.stdout

# An exception nobody catches ends the app; what it said goes to the crash file, and the next start repeats it
# in its log and keeps the file as coffeeflix-crash-previous.log.
crash_dir = Path(tempfile.mkdtemp(prefix='coffeeflix-crash-'))
took, p = run(['crash-terminate', str(crash_dir)], 20, expect_code=-6)
crash = (crash_dir / 'coffeeflix-crash.log').read_text()
assert 'TERMINATE uncaught std::runtime_error: boom' in crash, crash
took, p = run(['crash-report', str(crash_dir)], 20)
assert 'The last run crashed' in p.stdout and 'uncaught std::runtime_error: boom' in p.stdout, p.stdout
assert not (crash_dir / 'coffeeflix-crash.log').exists() and (crash_dir / 'coffeeflix-crash-previous.log').exists()
took, p = run(['crash-report', str(crash_dir)], 20)  # nothing new: nothing repeated
assert 'The last run crashed' not in p.stdout
(crash_dir / 'coffeeflix-previous.log').write_text('  1.0 [App] [OK] CoffeeFlix starting\n  2.0 [Player] [OK] Opened: a video\n')
took, p = run(['crash-report', str(crash_dir)], 20)  # a log that stops in the middle
assert 'did not end cleanly' in p.stdout and 'Opened: a video' in p.stdout, p.stdout
(crash_dir / 'coffeeflix-previous.log').write_text('  1.0 [App] [OK] Shutting down\n')
took, p = run(['crash-report', str(crash_dir)], 20)  # one that ended properly
assert 'did not end cleanly' not in p.stdout
print('PASS the crash file and the last run\'s log are read back at the next start', flush=True)

# A long run's log on the card keeps its end (and so its "Shutting down"), not its start.
log_dir = Path(tempfile.mkdtemp(prefix='coffeeflix-log-'))
took, p = run(['log-long', str(log_dir)], 120)
log_text = (log_dir / 'coffeeflix.log').read_text()
assert len(log_text) < 1_300_000, len(log_text)
assert 'line 3999 ' in log_text and 'Shutting down' in log_text, log_text[-300:]
assert 'line 0 ' not in log_text and 'was left out' in log_text
(log_dir / 'coffeeflix-previous.log').write_text(log_text)  # what the next start finds
took, p = run(['crash-report', str(log_dir)], 20)
assert 'did not end cleanly' not in p.stdout, p.stdout
print('PASS a long run\'s log keeps its end', flush=True)

folder = Path(tempfile.mkdtemp(prefix='coffeeflix-exit-'))
print('Artifacts:', folder, flush=True)
subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=24',
                '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000', '-t', '24',
                '-vf', 'noise=alls=25:allf=t', '-c:v', 'libx264', '-b:v', '1200k', '-maxrate', '1200k', '-bufsize', '1200k',
                '-pix_fmt', 'yuv420p', '-g', '24', '-c:a', 'aac', '-movflags', '+faststart', str(folder / 'sample.mp4')], check=True)
media = (folder / 'sample.mp4').read_bytes()
FROM = 1_000_000  # the stalling file answers nothing beyond this

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path == '/hang':  # never answers, the connection stays open
            time.sleep(120)
            return
        if self.path == '/hello':
            self.send_response(200)
            self.send_header('Content-Length', '5')
            self.end_headers()
            self.wfile.write(b'hello')
            return
        start, _, end = self.headers.get('Range', 'bytes=0-')[6:].partition('-')
        start, end = int(start or 0), min(int(end) if end else len(media) - 1, len(media) - 1)
        body = media[start:end + 1]
        self.send_response(206)
        self.send_header('Content-Type', 'video/mp4')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Content-Range', f'bytes {start}-{end}/{len(media)}')
        self.end_headers()
        try:
            if self.path == '/stall.mp4' and end >= FROM:
                self.wfile.write(body[:max(0, FROM - start)])  # what comes before, then silence with the connection open
                self.wfile.flush()
                time.sleep(120)
                return
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError): pass

class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

server = Server(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
base = f'http://127.0.0.1:{server.server_port}'

# A worker waiting on a server that never answers doesn't hold the app up when it closes.
took, p = run(['http-cancel', base], 60)
assert 'PASS closing cancels a request' in p.stdout, p.stdout

for mode in ('healthy', 'paused', 'stalled'):
    took, p = run([mode, base, str(folder)], 90)
    assert f'PASS closing while playing ({mode})' in p.stdout, mode
print('Closing while playing passed')
