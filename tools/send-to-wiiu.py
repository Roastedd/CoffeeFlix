#!/usr/bin/env python3
"""Convert & Send to Wii U.

Checks each video for the Wii U, converts what it can't play (H.264, 8-bit SDR, AAC, upright, fitted
under the 2 GB limit) and sends it to the "Receive files" screen. Sends a dropped connection on
from where it stopped.

    tools/send-to-wiiu.py http://192.168.1.20:8080/receive/<code>/ Movie.mkv Episode.mp4
    tools/send-to-wiiu.py --check Movie.mkv                  # only say what would happen
    tools/send-to-wiiu.py URL Movie.mkv --quality 720p       # smaller, faster to send
    tools/send-to-wiiu.py URL Movie.mkv --folder Movies      # Received/Movies

The address is the one shown on the Wii U under Media > Receive files. Needs ffmpeg and ffprobe.
Files that already play are sent as they are. On a Mac, conversion uses the hardware encoder
(VideoToolbox) when the installed ffmpeg has it; --software uses libx264 instead.
"""
import argparse, hashlib, http.client, json, os, re, shutil, subprocess, sys, tempfile, time, urllib.parse
from pathlib import Path

MAX_FILE = 2147483647            # what the Wii U's SD card can hold in one file
HEADROOM = 0.95                  # container overhead and encoder overshoot
AUDIO_KBPS = 128
# Starting points: (name, longest side, shortest side, video kbps). The list runs from best to smallest;
# a file that doesn't fit under 2 GB moves down it.
PRESETS = [('1080p', 1920, 1080, 4500), ('720p', 1280, 720, 2500), ('480p', 854, 480, 1200), ('360p', 640, 360, 700)]
VIDEO_EXT = {'mp4', 'm4v', 'mkv', 'webm', 'avi', 'mov', 'ts', 'm2ts', 'mpg', 'mpeg', 'flv', '3gp'}
PLAIN_EXT = {'mp3', 'm4a', 'aac', 'flac', 'ogg', 'opus', 'wav', 'wv', 'alac', 'oga', 'mka', 'jpg', 'jpeg', 'png', 'gif', 'webp', 'bmp', 'cbz', 'epub'}
SUBTITLE_EXT = {'srt', 'vtt', 'ass', 'ssa'}
TEXT_SUBTITLES = {'subrip': 'srt', 'srt': 'srt', 'webvtt': 'srt', 'mov_text': 'srt', 'ass': 'ass', 'ssa': 'ass'}
AUDIO_OK = {'aac', 'ac3', 'eac3', 'mp3', 'mp2', 'opus', 'vorbis', 'flac', 'alac', 'wavpack'}
HDR_TRANSFER = {'smpte2084', 'arib-std-b67'}


class Failure(Exception):
    pass


def say(text=''):
    print(text, flush=True)


def size_text(n):
    for unit, step in (('GB', 1 << 30), ('MB', 1 << 20)):
        if n >= step:
            return f'{n / step:.2f} {unit}' if unit == 'GB' else f'{n / step:.1f} {unit}'
    return f'{max(1, round(n / 1024))} KB'


def clock(seconds):
    seconds = max(0, int(round(seconds)))
    h, m, s = seconds // 3600, seconds % 3600 // 60, seconds % 60
    return f'{h}:{m:02d}:{s:02d}' if h else f'{m}:{s:02d}'


def run(*command):
    done = subprocess.run(command, capture_output=True, text=True)
    if done.returncode:
        raise Failure(f'{command[0]} failed: {done.stderr.strip().splitlines()[-1] if done.stderr.strip() else done.returncode}')
    return done.stdout


# ---------------------------------------------------------------------------------------- looking at a file
class Info:
    """What ffprobe says about one file, reduced to what the Wii U cares about."""
    def __init__(self, path):
        self.path = Path(path)
        self.size = self.path.stat().st_size
        data = json.loads(run('ffprobe', '-v', 'error', '-print_format', 'json', '-show_format', '-show_streams', str(path)))
        streams = data.get('streams', [])
        self.duration = float(data.get('format', {}).get('duration') or 0)
        self.video = next((s for s in streams if s.get('codec_type') == 'video' and not s.get('disposition', {}).get('attached_pic')), None)
        self.audio = [s for s in streams if s.get('codec_type') == 'audio']
        self.subtitles = [s for s in streams if s.get('codec_type') == 'subtitle']
        if not self.video:
            return
        v = self.video
        self.codec = v.get('codec_name', '')
        self.width, self.height = int(v.get('width') or 0), int(v.get('height') or 0)
        self.rotation = 0
        for side in v.get('side_data_list', []):
            if 'rotation' in side:
                self.rotation = int(round(float(side['rotation'])))
        if not self.rotation and v.get('tags', {}).get('rotate'):
            self.rotation = -int(float(v['tags']['rotate']))
        self.turned = abs(self.rotation) % 180 == 90
        # Pixel shape: a 720x480 picture that is meant as 16:9 has non-square pixels.
        sar = v.get('sample_aspect_ratio', '1:1')
        try:
            a, b = (float(x) for x in sar.split(':'))
            self.sar = a / b if a > 0 and b > 0 and 0.1 <= a / b <= 10 else 1.0
        except ValueError:
            self.sar = 1.0
        self.pix_fmt = v.get('pix_fmt', '')
        self.profile = v.get('profile', '')
        self.transfer = v.get('color_transfer', '')
        self.dolby = any(side.get('side_data_type', '').startswith('DOVI') for side in v.get('side_data_list', []))
        try:
            num, den = v.get('avg_frame_rate', '0/1').split('/')
            self.fps = float(num) / float(den) if float(den) else 0.0
        except ValueError:
            self.fps = 0.0
        # Shown size: square pixels, turned upright.
        w, h = self.width * self.sar, self.height
        self.shown_w, self.shown_h = (h, w) if self.turned else (w, h)

    @property
    def ext(self):
        return self.path.suffix.lower().lstrip('.')


def verdict(info):
    """('ready' | 'limits' | 'convert', reason). Same rules and wording as the Wii U and the upload page."""
    if not info.video:
        return 'convert', 'No video found'
    big, small = max(info.shown_w, info.shown_h), min(info.shown_w, info.shown_h)
    soft = info.codec in ('mpeg4', 'mpeg2video', 'mpeg1video')
    if info.dolby or info.transfer in HDR_TRANSFER:
        return 'convert', 'HDR video needs conversion'
    if info.codec == 'hevc':
        return 'convert', 'HEVC (H.265) video needs conversion'
    if info.codec != 'h264' and not soft:
        return 'convert', 'Video format needs conversion'
    if info.codec == 'h264' and (info.pix_fmt not in ('yuv420p', 'yuvj420p') or info.profile in ('High 10', 'High 4:2:2', 'High 4:4:4 Predictive', 'High 10 Intra')):
        return 'convert', '10-bit video needs conversion'
    if big > 1920 or small > 1088:
        return 'convert', 'Larger than 1080p, needs conversion'
    if soft and info.shown_w * info.shown_h > 720 * 576:
        return 'convert', 'Video format needs conversion'
    if info.fps >= 50 and big > 1280:
        return 'limits', '1080p at 60 fps shows about 45 pictures a second'
    if info.audio and all(a.get('codec_name') not in AUDIO_OK and not a.get('codec_name', '').startswith('pcm_') for a in info.audio):
        return 'limits', 'No sound: unsupported audio'
    if soft:
        return 'limits', 'Plays with software decoding, may be slow'
    return 'ready', ''


# ---------------------------------------------------------------------------------------- deciding what to make
class Plan:
    def __init__(self, info, args):
        self.info, self.args = info, args
        self.notes = []
        self.level, self.reason = verdict(info)
        self.limit = min(int(args.limit_mb * 1024 * 1024) if args.limit_mb else MAX_FILE, MAX_FILE)
        too_big = info.size > self.limit
        # A file that only shows fewer pictures a second than it has is left alone when 60 fps was asked for.
        accepted = self.level == 'limits' and 'pictures a second' in self.reason and args.fps >= 50
        works = self.level == 'ready' or accepted
        self.copy = bool(args.no_convert) or (works and not too_big and not args.force_convert)
        if not self.copy:
            self.choose(too_big)

    def choose(self, too_big):
        info, args = self.info, self.args
        long_side, short_side = max(info.shown_w, info.shown_h), min(info.shown_w, info.shown_h)
        start = next((i for i, p in enumerate(PRESETS) if p[0] == args.quality), 0)
        # Never enlarge: begin at the best preset the picture itself can fill.
        while start < len(PRESETS) - 1 and short_side < PRESETS[start][2] * 0.95 and long_side < PRESETS[start][1] * 0.95:
            start += 1
        duration = max(info.duration, 1.0)
        budget = self.limit * HEADROOM * 8 / duration - AUDIO_KBPS * 1000       # bits/s left for video
        chosen = None
        for i in range(start, len(PRESETS)):
            name, w, h, kbps = PRESETS[i]
            if args.kbps:
                kbps = args.kbps
            if budget >= kbps * 1000 * 0.6 or i == len(PRESETS) - 1:
                chosen = (name, w, h, min(kbps, max(200, int(budget / 1000))))
                break
        self.name, self.long_side, self.short_side, self.kbps = chosen
        self.fps = None
        cap = args.fps
        if info.fps > cap + 0.5:
            self.fps = cap
        if chosen[0] != PRESETS[start][0]:
            self.notes.append(f'{PRESETS[start][0]} would not fit under the size limit; using {chosen[0]}')
        elif chosen[3] < PRESETS[start][3]:
            self.notes.append(f'video rate lowered to {chosen[3]} kbps to fit the size limit')
        self.estimate = (self.kbps + AUDIO_KBPS) * 1000 / 8 * duration
        if info.transfer in HDR_TRANSFER or info.dolby:
            self.notes.append('HDR is mapped to SDR' + ('' if ZSCALE else ' (this ffmpeg has no zscale, colours may look flat)'))

    def describe(self):
        if self.copy:
            if self.args.no_convert and self.level != 'ready':
                return f'send as is ({self.reason}; conversion turned off)'
            return 'send as is' if self.info.video else 'send as is (not a video)'
        fps = f'{self.fps:g} fps, ' if self.fps else ''
        return f'convert to H.264 {self.name}, {fps}{self.kbps} kbps + AAC, about {size_text(self.estimate)}'


def encoders():
    text = run('ffmpeg', '-hide_banner', '-encoders')
    return {'videotoolbox': ' h264_videotoolbox ' in text, 'x264': ' libx264 ' in text}


def has_zscale():
    try:
        return ' zscale ' in run('ffmpeg', '-hide_banner', '-filters')
    except Failure:
        return False


ZSCALE = False


def video_filters(plan):
    info = plan.info
    chain = []
    # ffmpeg turns a rotated phone video upright by itself before these filters run.
    if abs(info.sar - 1) > 0.01:
        chain.append('scale=w=trunc(iw*sar/2)*2:h=ih,setsar=1')
    if info.transfer in HDR_TRANSFER or info.dolby:
        if ZSCALE:
            chain.append('zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,tonemap=tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv')
        else:
            chain.append('colorspace=all=bt709:iall=bt2020:fast=1')
    portrait = info.shown_h > info.shown_w
    w, h = (plan.short_side, plan.long_side) if portrait else (plan.long_side, plan.short_side)
    chain.append(f"scale=w='min({w},iw)':h='min({h},ih)':force_original_aspect_ratio=decrease:force_divisible_by=2")
    if plan.fps:
        chain.append(f'fps={plan.fps:g}')
    chain.append('format=yuv420p')
    return ','.join(chain)


def pick_audio(info, language):
    if not info.audio:
        return None
    wanted = {language.lower(), {'eng': 'en'}.get(language.lower(), language.lower())} if language else set()
    for stream in info.audio:
        if stream.get('tags', {}).get('language', '').lower() in wanted:
            return stream
    return next((s for s in info.audio if s.get('disposition', {}).get('default')), info.audio[0])


def convert(plan, dst, encoder, say_progress=True):
    info = plan.info
    for attempt in range(3):
        kbps = plan.kbps
        audio = pick_audio(info, plan.args.audio_language)
        command = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-nostdin', '-y', '-i', str(info.path),
                   '-map', f'0:{info.video["index"]}']
        if audio:
            command += ['-map', f'0:{audio["index"]}']
        command += ['-sn', '-dn', '-map_chapters', '-1', '-vf', video_filters(plan)]
        if encoder == 'videotoolbox':
            command += ['-c:v', 'h264_videotoolbox', '-profile:v', 'high', '-level', '4.1', '-b:v', f'{kbps}k', '-maxrate', f'{int(kbps * 1.5)}k', '-tag:v', 'avc1']
        else:
            command += ['-c:v', 'libx264', '-preset', 'faster', '-profile:v', 'high', '-level', '4.1', '-b:v', f'{kbps}k',
                        '-maxrate', f'{int(kbps * 1.5)}k', '-bufsize', f'{kbps * 2}k']
        command += ['-pix_fmt', 'yuv420p']
        if audio and audio.get('codec_name') == 'aac' and int(audio.get('channels') or 2) <= 2:
            command += ['-c:a', 'copy']
        elif audio:
            command += ['-c:a', 'aac', '-b:a', f'{AUDIO_KBPS}k', '-ac', '2', '-ar', '48000']
        command += ['-movflags', '+faststart', '-progress', 'pipe:1', '-nostats', '-f', 'mp4', str(dst)]
        partial = str(dst)
        started = time.time()
        proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        problems = []
        for line in proc.stdout:
            if not re.match(r'^\w+=', line) and line.strip():
                problems.append(line.strip())
            if line.startswith('out_time_us=') and info.duration and say_progress:
                try:
                    done = int(line.split('=')[1]) / 1e6
                except ValueError:
                    continue
                fraction = min(done / info.duration, 1)
                elapsed = time.time() - started
                left = elapsed / fraction - elapsed if fraction > 0.02 else -1
                sys.stdout.write(f'\r  converting {fraction * 100:3.0f}%' + (f'  {clock(left)} left' if left >= 0 else '') + '   ')
                sys.stdout.flush()
        proc.wait()
        if say_progress:
            sys.stdout.write('\r' + ' ' * 60 + '\r')
        if proc.returncode:
            raise Failure(problems[-1] if problems else f'ffmpeg exited with {proc.returncode}')
        size = Path(partial).stat().st_size
        if size <= plan.limit:
            return size
        # The encoder overshot its target: try again with less.
        plan.kbps = max(150, int(kbps * plan.limit * 0.93 / size))
        say(f'  the result was {size_text(size)}, over the limit; trying again at {plan.kbps} kbps')
    raise Failure('could not make the file small enough')


def extract_subtitles(info, stem, folder):
    """Text subtitle tracks inside the file, written beside it as <stem>.<language>.srt / .ass. Returns the paths."""
    made, seen = [], {}
    for stream in info.subtitles:
        kind = TEXT_SUBTITLES.get(stream.get('codec_name', ''))
        if not kind or len(made) >= 8:
            continue
        lang = stream.get('tags', {}).get('language', '')
        lang = '' if lang in ('', 'und') else re.sub(r'[^A-Za-z0-9-]', '', lang)[:8]
        seen[lang] = seen.get(lang, 0) + 1
        tag = (lang or 'sub') + ('' if seen[lang] == 1 else str(seen[lang]))
        out = Path(folder) / f'{stem}.{tag}.{kind}'
        try:
            run('ffmpeg', '-hide_banner', '-loglevel', 'error', '-nostdin', '-y', '-i', str(info.path), '-map', f'0:{stream["index"]}', '-c:s', 'ass' if kind == 'ass' else 'srt', str(out))
            if out.stat().st_size > 0:
                made.append(out)
        except Failure:
            pass
    return made


def sidecar_subtitles(path):
    """Subtitle files that sit next to the video and start with its name."""
    stem = path.stem.lower()
    found = []
    for other in sorted(path.parent.iterdir()):
        if other.suffix.lower().lstrip('.') in SUBTITLE_EXT and other.name.lower().startswith(stem + '.') and other != path:
            found.append(other)
    return found


def safe_name(name, keep_ext=None):
    stem = Path(name).stem
    stem = re.sub(r'[/\\:*?"<>|\x00-\x1f\x7f]', ' ', stem)
    stem = re.sub(r'\s+', ' ', stem).strip(' .')
    stem = stem.encode('utf-8')[:150].decode('utf-8', 'ignore').strip(' .') or 'video'
    return stem


# ---------------------------------------------------------------------------------------- sending
class Receiver:
    def __init__(self, url):
        parts = urllib.parse.urlsplit(url)
        segments = [s for s in parts.path.split('/') if s]
        if parts.scheme != 'http' or not parts.hostname or len(segments) != 2 or segments[0] != 'receive':
            raise Failure('the address should look like http://192.168.1.20:8080/receive/<code>/ (shown on the Wii U)')
        self.host, self.port, self.key = parts.hostname, parts.port or 80, segments[1]
        self.base = parts.path if parts.path.endswith('/') else parts.path + '/'

    def connection(self, timeout=30):
        return http.client.HTTPConnection(self.host, self.port, timeout=timeout)

    def ask(self, method, path, headers=None):
        conn = self.connection(15)
        try:
            conn.request(method, self.base + path, headers={'X-Upload-Key': self.key, **(headers or {})})
            reply = conn.getresponse()
            data = reply.read()
            try:
                return reply.status, json.loads(data)
            except ValueError:
                return reply.status, {}
        finally:
            conn.close()

    def info(self):
        status, data = self.ask('GET', 'info')
        if status != 200:
            raise Failure('the Wii U did not accept this address (is the Receive files screen still open?)')
        return data

    def kept(self, upload_id, total):
        status, data = self.ask('GET', 'upload', {'X-Upload-Id': upload_id, 'X-Total-Size': str(total)})
        offset = data.get('offset', 0) if status == 200 else 0
        return offset if 0 < offset <= total else 0

    def send(self, path, name, folder, index, count, log=say):
        path = Path(path)
        total = path.stat().st_size
        identity = f'{name}|{total}|{path.stat().st_mtime_ns}'
        upload_id = hashlib.sha256(identity.encode()).hexdigest()[:32]
        stalled, offset = 0, None
        while True:
            try:
                if offset is None:
                    offset = self.kept(upload_id, total)
                    if offset:
                        log(f'  continuing from {size_text(offset)}')
                before = offset
                answer, body = self.put(path, name, folder, index, count, upload_id, total, offset)
                if answer == 200 and body.get('saved'):
                    return body['saved']
                if isinstance(body.get('offset'), int) and answer in (200, 409):
                    if body['offset'] > before:
                        stalled = 0
                    else:
                        stalled += 1
                    offset = body['offset']
                    if stalled > 6:
                        raise Failure(body.get('error') or 'the Wii U keeps refusing the rest of the file')
                    time.sleep(1)
                    continue
                raise Failure(body.get('error') or f'the Wii U answered {answer}')
            except (OSError, http.client.HTTPException) as problem:
                stalled += 1
                if stalled > 12:
                    raise Failure(f'lost the connection to the Wii U ({problem})')
                sys.stdout.write(f'\r  connection lost, trying again ({stalled})...' + ' ' * 20 + '\n')
                time.sleep(min(1.5 * stalled, 10))
                previous = offset or 0
                try:
                    offset = self.kept(upload_id, total)
                except (OSError, http.client.HTTPException):
                    offset = None
                if offset and offset > previous:
                    stalled = 0
                    log(f'  continuing from {size_text(offset)}')

    def put(self, path, name, folder, index, count, upload_id, total, offset):
        conn = self.connection(30)
        headers = {'Content-Type': 'application/octet-stream', 'X-Upload-Key': self.key, 'X-File-Name': urllib.parse.quote(name, safe=''),
                   'X-Upload-Id': upload_id, 'X-Upload-Offset': str(offset), 'X-Total-Size': str(total),
                   'X-Queue-Index': str(index), 'X-Queue-Count': str(count), 'Content-Length': str(total - offset)}
        if folder:
            headers['X-Folder'] = urllib.parse.quote(folder, safe='')
        try:
            conn.putrequest('PUT', self.base + 'upload', skip_accept_encoding=True)
            for k, v in headers.items():
                conn.putheader(k, v)
            conn.endheaders()
            sent, mark, mark_bytes, speed = offset, time.time(), offset, 0.0
            try:
                with open(path, 'rb') as f:
                    f.seek(offset)
                    while True:
                        chunk = f.read(256 * 1024)
                        if not chunk:
                            break
                        conn.send(chunk)
                        sent += len(chunk)
                        now = time.time()
                        if now - mark >= 0.5:
                            instant = (sent - mark_bytes) / (now - mark)
                            speed = instant if speed == 0 else speed * 0.7 + instant * 0.3
                            mark, mark_bytes = now, sent
                            left = (total - sent) / speed if speed > 1 else -1
                            sys.stdout.write(f'\r  sending {sent / total * 100:3.0f}%  {size_text(speed)}/s' + (f'  {clock(left)} left' if left >= 0 else '') + '   ')
                            sys.stdout.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass  # the Wii U may already have answered (a refused file): read that answer below
            reply = conn.getresponse()
            data = reply.read()
            sys.stdout.write('\r' + ' ' * 60 + '\r')
            try:
                return reply.status, json.loads(data)
            except ValueError:
                return reply.status, {}
        finally:
            conn.close()


# ---------------------------------------------------------------------------------------- the tool
def parse():
    p = argparse.ArgumentParser(description='Check, convert and send videos to the Wii U.', usage='%(prog)s [URL] FILE [FILE...] [options]')
    p.add_argument('items', nargs='+', help='the address from the Wii U (http://...), then the files')
    p.add_argument('--check', action='store_true', help='only report what would be done')
    p.add_argument('--quality', choices=[q[0] for q in PRESETS], default='1080p', help='largest picture to make (default 1080p; 720p is smaller and quicker)')
    p.add_argument('--fps', type=float, default=30, help='most pictures per second to keep (default 30; 60 needs a 720p or smaller picture to run smoothly)')
    p.add_argument('--kbps', type=int, help='video rate, instead of the preset')
    p.add_argument('--limit-mb', type=float, help='keep files under this size instead of 2 GB')
    p.add_argument('--audio-language', default='eng', help='preferred audio track (default eng)')
    p.add_argument('--folder', help='save in this folder inside Received')
    p.add_argument('--software', action='store_true', help='encode with libx264 instead of the hardware encoder')
    p.add_argument('--force-convert', action='store_true', help='convert even files that already play')
    p.add_argument('--no-convert', action='store_true', help='send files as they are')
    p.add_argument('--keep', action='store_true', help='keep converted files after sending')
    p.add_argument('--work-dir', help='where converted files are kept (default: a cache folder)')
    args = p.parse_args()
    args.url = None
    if args.items and args.items[0].startswith('http'):
        args.url = args.items.pop(0)
    if not args.items:
        p.error('no files given')
    if not args.url and not args.check:
        p.error('give the address shown on the Wii U, or use --check')
    return args


def cache_dir(args):
    if args.work_dir:
        folder = Path(args.work_dir)
    elif sys.platform == 'darwin':
        folder = Path.home() / 'Library' / 'Caches' / 'CoffeeFlix-send'
    else:
        folder = Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'coffeeflix-send'
    folder.mkdir(parents=True, exist_ok=True)
    return folder


def prepare(source, args, encoder, available, work):
    """What to send for one file: [(path, name on the Wii U)], and the files made along the way."""
    ext = source.suffix.lower().lstrip('.')
    if ext in SUBTITLE_EXT or ext in PLAIN_EXT:
        say('  not a video: sent as it is')
        return [(source, source.name)], []
    if ext not in VIDEO_EXT:
        raise Failure(f'.{ext} is not a file type the Wii U receives')
    info = Info(source)
    plan = Plan(info, args)
    say(f'  {info.codec or "no video"} {int(info.shown_w)}x{int(info.shown_h)}' + (f' {info.fps:g} fps' if info.video else '') + f', {clock(info.duration)}, {size_text(info.size)}')
    label = {'ready': 'Ready to play', 'limits': 'Plays with limits', 'convert': 'Needs conversion'}[plan.level]
    say(f'  {label}' + (f': {plan.reason}' if plan.reason else ''))
    say(f'  plan: {plan.describe()}')
    for note in plan.notes:
        say(f'  note: {note}')
    if args.check:
        return [], []
    stem = safe_name(source.name)
    beside = [(extra, stem + extra.name[len(source.stem):]) for extra in sidecar_subtitles(source)]
    if plan.copy:
        return [(source, stem + source.suffix.lower())] + beside, []
    signature = hashlib.sha256(f'{source.resolve()}|{info.size}|{source.stat().st_mtime_ns}|{plan.name}|{plan.kbps}|{plan.fps}|{args.audio_language}|{encoder}'.encode()).hexdigest()[:16]
    target = work / f'{stem}-{signature}.mp4'
    made = [target]
    if target.exists():
        say(f'  using the converted copy from earlier ({size_text(target.stat().st_size)})')
    else:
        partial = work / f'{stem}-{signature}.partial.mp4'
        try:
            try:
                convert(plan, partial, encoder)
            except Failure as problem:
                if encoder != 'videotoolbox' or not available['x264']:
                    raise
                say(f'  hardware encoder failed ({problem}); using libx264')
                convert(plan, partial, 'x264')
            os.replace(partial, target)
        finally:
            if partial.exists():
                partial.unlink()
        say(f'  converted: {size_text(target.stat().st_size)}')
    inside = extract_subtitles(info, stem, work)
    made += inside
    return [(target, stem + '.mp4')] + [(s, s.name) for s in inside] + beside, made


def main():
    global ZSCALE
    args = parse()
    for tool in ('ffmpeg', 'ffprobe'):
        if not shutil.which(tool):
            raise Failure(f'{tool} is needed (brew install ffmpeg)')
    ZSCALE = has_zscale()
    available = encoders()
    if not available['videotoolbox'] and not available['x264'] and not args.check and not args.no_convert:
        raise Failure('this ffmpeg has neither h264_videotoolbox nor libx264')
    encoder = 'videotoolbox' if available['videotoolbox'] and not args.software else 'x264'
    receiver = None
    if args.url and not args.check:
        receiver = Receiver(args.url)
        room = receiver.info()
        say(f'Wii U found. {size_text(room["free"])} free on its SD card.' if room.get('free', -1) >= 0 else 'Wii U found.')
    files = [Path(x) for x in args.items]
    for f in files:
        if not f.is_file():
            raise Failure(f'{f} is not a file')
    work = cache_dir(args)
    sent = failures = 0
    for number, source in enumerate(files, 1):
        say(f'[{number}/{len(files)}] {source.name}')
        made, complete = [], False
        try:
            uploads, made = prepare(source, args, encoder, available, work)
            for path, name in uploads:
                if path.stat().st_size > MAX_FILE:
                    raise Failure(f'{name} is over 2 GB')
                saved = receiver.send(path, name, args.folder, number, len(files))
                say(f'  saved as {saved}' if saved != name else f'  saved ({size_text(path.stat().st_size)})')
                sent += 1
            complete = True
        except Failure as problem:
            failures += 1
            say(f'  failed: {problem}' + ('  (run the same command again to continue)' if made else ''))
        finally:
            # A converted copy stays after a failure: the next run sends on from where it stopped.
            if complete and not args.keep:
                for path in made:
                    path.unlink(missing_ok=True)
    if not args.check:
        say(f'{sent} file{"s" if sent != 1 else ""} sent' + (f', {failures} failed' if failures else ''))
    return 1 if failures else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Failure as problem:
        print(f'error: {problem}', file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print('\ninterrupted; run the same command again to continue', file=sys.stderr)
        sys.exit(130)
