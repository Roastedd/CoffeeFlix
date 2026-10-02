#!/usr/bin/env python3
"""Run tools/send-to-wiiu.py against the real receiver: what it sends as is, what it converts (codec, HDR-less
10-bit, rotation, non-square pixels, size limit), subtitles that travel with the video, folders, and an
upload that breaks half-way and continues."""
import hashlib,json,os,shutil,socket,subprocess,sys,tempfile,threading,time,urllib.parse
from pathlib import Path
root=Path(__file__).resolve().parents[2];os.chdir(root)
if not shutil.which('ffmpeg') or not shutil.which('ffprobe'):
    print('SKIP send-to-wiiu: needs ffmpeg');sys.exit(0)
work=Path(tempfile.mkdtemp(prefix='coffee-send-test-'));received=work/'Received';media=work/'media';media.mkdir();cache=work/'cache'
p=subprocess.Popen(['build-desktop/media-transfer-test',str(received)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1)
url=''
while not url:
    line=p.stdout.readline();assert line,'receiver did not start'
    if line.startswith('http://'):url=line.strip()
u=urllib.parse.urlsplit(url)
def ffmpeg(*args):subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-y',*args],check=True)
def clip(name,size,*opts,rate=30,seconds=2,audio=True,vf=None,extra_inputs=()):
    cmd=['-f','lavfi','-i',f'testsrc2=size={size}:rate={rate}:duration={seconds}']
    if audio:cmd+=['-f','lavfi','-i',f'sine=frequency=440:duration={seconds}']
    cmd+=list(extra_inputs)
    if vf:cmd+=['-vf',vf]
    ffmpeg(*cmd,*opts,*(['-shortest'] if audio else ['-an']),str(media/name))
def probe(path):
    data=json.loads(subprocess.run(['ffprobe','-v','error','-print_format','json','-show_streams','-show_format',str(path)],check=True,capture_output=True,text=True).stdout)
    video=next(s for s in data['streams'] if s['codec_type']=='video');audio=[s for s in data['streams'] if s['codec_type']=='audio']
    return video,audio,data['format']
def sender(*args,address=None,expect=0,software=True):
    done=subprocess.run([sys.executable,'tools/send-to-wiiu.py',address or url,*map(str,args),'--work-dir',str(cache),*(['--software'] if software else [])],capture_output=True,text=True,timeout=300)
    assert done.returncode==expect,(done.returncode,done.stdout,done.stderr);return done.stdout
try:
    # Compatible: sent byte for byte.
    clip('plain.mp4','1280x720','-c:v','libx264','-c:a','aac')
    out=sender(media/'plain.mp4');assert 'send as is' in out and 'saved' in out,out
    assert (received/'plain.mp4').read_bytes()==(media/'plain.mp4').read_bytes()
    # HEVC: becomes 8-bit H.264 + AAC in MP4, keeps the picture size.
    clip('hevc.mkv','1280x720','-c:v','libx265','-c:a','ac3')
    out=sender(media/'hevc.mkv');assert 'HEVC' in out and 'converted' in out,out
    v,a,f=probe(received/'hevc.mp4');assert v['codec_name']=='h264' and v['pix_fmt']=='yuv420p' and (v['width'],v['height'])==(1280,720) and a[0]['codec_name']=='aac' and a[0]['channels']==2,(v,a)
    # 10-bit source: 8-bit result. Its subtitles (one inside, one beside it) are sent along with the matching name.
    (media/'Ten Bit Show.eng.srt').write_text('1\n00:00:00,000 --> 00:00:01,000\nHello\n')
    (media/'sidecar.srt').write_text('1\n00:00:00,000 --> 00:00:01,000\nHallo\n')
    clip('Ten Bit Show.mkv','1280x720','-c:v','libx264','-pix_fmt','yuv420p10le','-c:a','aac','-map','0:v','-map','1:a','-map','2:s','-c:s','srt','-metadata:s:s:0','language=fre',extra_inputs=['-i',str(media/'sidecar.srt')])
    out=sender(media/'Ten Bit Show.mkv','--folder','Shows');assert '10-bit' in out,out
    v,a,f=probe(received/'Shows'/'Ten Bit Show.mp4');assert v['pix_fmt']=='yuv420p' and v['codec_name']=='h264'
    assert (received/'Shows'/'Ten Bit Show.fre.srt').exists() and 'Hallo' in (received/'Shows'/'Ten Bit Show.fre.srt').read_text()
    assert (received/'Shows'/'Ten Bit Show.eng.srt').read_text().startswith('1\n')
    # A 4K phone video stored sideways with a rotation: comes out upright, sized for the TV, no rotation left in it.
    clip('phone_raw.mp4','3840x2160','-c:v','libx264','-preset','ultrafast','-c:a','aac',seconds=1)
    ffmpeg('-display_rotation','90','-i',str(media/'phone_raw.mp4'),'-c','copy',str(media/'Phone.mp4'))
    out=sender(media/'Phone.mp4');assert 'Larger than 1080p' in out,out
    v,a,f=probe(received/'Phone.mp4');rot=[s for s in v.get('side_data_list',[]) if 'rotation' in s]
    assert (v['width'],v['height'])==(1080,1920) and not rot and v['pix_fmt']=='yuv420p',(v,)
    # Non-square pixels (DVD-like 720x480 meant as 16:9), stored as HEVC so it converts: comes out square.
    clip('dvd.mkv','720x480','-c:v','libx265','-vf','setsar=32/27','-c:a','aac')
    sender(media/'dvd.mkv');v,a,f=probe(received/'dvd.mp4')
    assert v.get('sample_aspect_ratio','1:1') in ('1:1','0:1') and abs(v['width']/v['height']-16/9)<0.03,(v,)
    # 60 fps 1080p is brought down to 30; a 720p60 file that already plays isn't touched.
    clip('sixty.mp4','1920x1080','-c:v','libx264','-preset','ultrafast','-c:a','aac',rate=60,seconds=1)
    out=sender(media/'sixty.mp4');v,a,f=probe(received/'sixty.mp4');assert eval(v['avg_frame_rate'])<=30.5,v
    # Fitting under a size limit: a small limit makes it use a lower rate (and smaller picture) so the file fits.
    clip('long.mkv','1280x720','-c:v','libx265','-c:a','aac',seconds=6)
    out=sender(media/'long.mkv','--limit-mb','0.5');assert 'size limit' in out or 'kbps' in out,out
    assert (received/'long.mp4').stat().st_size<=0.5*1048576,(received/'long.mp4').stat().st_size
    # The hardware encoder, where this Mac's ffmpeg has one.
    if 'h264_videotoolbox' in subprocess.run(['ffmpeg','-hide_banner','-encoders'],capture_output=True,text=True).stdout:
        clip('Hardware.mkv','1280x720','-c:v','libx265','-c:a','aac');sender(media/'Hardware.mkv',software=False)
        v,a,f=probe(received/'Hardware.mp4');assert v['codec_name']=='h264' and v['pix_fmt']=='yuv420p' and v['profile']=='High',v
    # --check changes nothing on the Wii U.
    before=sorted(x.name for x in received.rglob('*'));out=sender(media/'hevc.mkv',address='--check');assert 'Needs conversion' in out
    assert before==sorted(x.name for x in received.rglob('*'))
    # Wrong code: refused, nothing written.
    bad=url.rsplit('/',2)[0]+'/'+('0'*32)+'/'
    done=subprocess.run([sys.executable,'tools/send-to-wiiu.py',bad,str(media/'plain.mp4'),'--work-dir',str(cache)],capture_output=True,text=True);assert done.returncode!=0 and 'did not accept' in done.stderr,done
    # A connection that breaks half-way: the helper asks the Wii U what it kept and sends only the rest.
    big=media/'Big.mp4';clip('Big.mp4','1280x720','-c:v','libx264','-preset','ultrafast','-b:v','9M','-c:a','aac',seconds=12)
    size=big.stat().st_size;assert size>6_000_000,size
    cut=size//3;seen={'cut':False,'offsets':[]}
    def serve_proxy(listener):
        while True:
            try:c,_=listener.accept()
            except OSError:return
            threading.Thread(target=handle,args=(c,),daemon=True).start()
    def handle(c):
        head=b''
        while b'\r\n\r\n' not in head:
            d=c.recv(65536)
            if not d:c.close();return
            head+=d
        h,rest=head.split(b'\r\n\r\n',1);h=h.replace(b'127.0.0.1:%d'%proxy_port,b'127.0.0.1:%d'%u.port)
        up=socket.create_connection(('127.0.0.1',u.port));up.sendall(h+b'\r\n\r\n'+rest)
        limit=None
        for line in h.split(b'\r\n'):
            if line.lower().startswith(b'x-upload-offset'):seen['offsets'].append(int(line.split(b':')[1]))
        if h.startswith(b'PUT') and not seen['cut']:seen['cut']=True;limit=cut-len(rest)
        def pump(a,b,limit=None):
            sent=0
            try:
                while True:
                    d=a.recv(65536)
                    if not d:break
                    if limit is not None and sent+len(d)>limit:
                        b.sendall(d[:max(0,limit-sent)]);raise ConnectionResetError
                    b.sendall(d);sent+=len(d)
            except OSError:pass
            finally:
                for s in(a,b):
                    try:s.shutdown(socket.SHUT_RDWR)
                    except OSError:pass
                    s.close()
        threading.Thread(target=pump,args=(up,c),daemon=True).start();pump(c,up,limit)
    listener=socket.socket();listener.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);listener.bind(('127.0.0.1',0));listener.listen(8)
    proxy_port=listener.getsockname()[1];threading.Thread(target=serve_proxy,args=(listener,),daemon=True).start()
    out=sender(big,address=url.replace(f'127.0.0.1:{u.port}',f'127.0.0.1:{proxy_port}'))
    assert seen['cut'] and 'continuing from' in out and len(seen['offsets'])>=2 and seen['offsets'][-1]>0,(out,seen)
    assert hashlib.sha256((received/'Big.mp4').read_bytes()).digest()==hashlib.sha256(big.read_bytes()).digest()
    assert not list(received.glob('.resume-*')) and not list(received.glob('.upload-*'))
    listener.close()
    # The converted copy is cleaned up after a successful send.
    assert not list(cache.glob('*.mp4')) and not list(cache.glob('*.srt'))
    print('PASS Convert & Send: as-is copy, HEVC/10-bit/4K/anamorphic/60 fps conversion, upright phone video, size limit, subtitles and folders, --check, refused address, resume after a broken connection')
finally:
    if p.poll() is None:p.stdin.write('quit\n');p.stdin.flush();p.wait(timeout=6)
    shutil.rmtree(work,ignore_errors=True)
