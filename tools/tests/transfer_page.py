#!/usr/bin/env python3
"""Check the upload page's file check (content/transfer.html) against real files made with ffmpeg.

The check reads MP4/MOV and Matroska/WebM headers in the browser and says whether the Wii U can play
the file. Node runs the very same code that ships in the page."""
import json,os,re,shutil,subprocess,sys,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[2];os.chdir(root)
if not shutil.which('node') or not shutil.which('ffmpeg'):
    print('SKIP transfer page check: needs node and ffmpeg');sys.exit(0)
html=(root/'content/transfer.html').read_text()
assert html.count('{{WORDS}}')==1 and html.count('{{TOKEN}}')==2
work=Path(tempfile.mkdtemp(prefix='coffee-page-probe-'))
script=html[html.index('<script nonce'):html.index('</script>')]
script=script[script.index('>')+1:].replace('{{WORDS}}','{}').replace('{{TOKEN}}','k')
(work/'page.js').write_text(script)
subprocess.run(['node','--check',str(work/'page.js')],check=True)
def make(name,size,rate,*opts,vf=None,audio=True):
    cmd=['ffmpeg','-hide_banner','-loglevel','error','-y','-f','lavfi','-i',f'testsrc2=size={size}:rate={rate}:duration=2']
    if audio:cmd+=['-f','lavfi','-i','sine=frequency=440:duration=2']
    if vf:cmd+=['-vf',vf]
    cmd+=[*opts]+(['-shortest'] if audio else ['-an'])+[str(work/name)]
    subprocess.run(cmd,check=True)
HDR='format=%s,setparams=color_primaries=bt2020:color_trc=%s:colorspace=bt2020nc'
cases=[ # file, expected level, expected reason
 ('h264.mp4','ready','',lambda n:make(n,'1280x720',30,'-c:v','libx264','-c:a','aac')),
 ('h264_faststart.mp4','ready','',lambda n:make(n,'1280x720',30,'-c:v','libx264','-c:a','aac','-movflags','+faststart')),
 ('h264.mov','ready','',lambda n:make(n,'1280x720',30,'-c:v','libx264','-c:a','aac')),
 ('h264_1080p_ac3.mkv','ready','',lambda n:make(n,'1920x1080',30,'-c:v','libx264','-c:a','ac3')),
 ('h264_silent.mp4','ready','',lambda n:make(n,'1280x720',30,'-c:v','libx264',audio=False)),
 ('phone_portrait.mp4','ready','',lambda n:make(n,'1080x1920',30,'-c:v','libx264','-c:a','aac')),
 ('hevc.mp4','convert','HEVC (H.265) video needs conversion',lambda n:make(n,'1280x720',30,'-c:v','libx265','-tag:v','hvc1','-c:a','aac')),
 ('hdr10.mp4','convert','HDR video needs conversion',lambda n:make(n,'1280x720',30,'-c:v','libx265','-tag:v','hvc1','-c:a','aac',vf=HDR%('yuv420p10le','smpte2084'))),
 ('hlg.mkv','convert','HDR video needs conversion',lambda n:make(n,'1280x720',30,'-c:v','libx264','-c:a','aac',vf=HDR%('yuv420p','arib-std-b67'))),
 ('ten_bit.mkv','convert','10-bit video needs conversion',lambda n:make(n,'1280x720',30,'-c:v','libx264','-pix_fmt','yuv420p10le','-c:a','aac')),
 ('qhd.mp4','convert','Larger than 1080p, needs conversion',lambda n:make(n,'2560x1440',30,'-c:v','libx264','-c:a','aac')),
 ('vp9.webm','convert','Video format needs conversion',lambda n:make(n,'640x360',30,'-c:v','libvpx-vp9','-c:a','libopus')),
 ('av1.mkv','convert','Video format needs conversion',lambda n:make(n,'640x360',30,'-c:v','libsvtav1','-c:a','libopus')),
 ('sixty.mp4','limits','1080p at 60 fps shows about 45 pictures a second',lambda n:make(n,'1920x1080',60,'-c:v','libx264','-c:a','aac')),
 ('mpeg4.mp4','limits','Plays with software decoding, may be slow',lambda n:make(n,'640x480',30,'-c:v','mpeg4','-c:a','aac')),
]
made={}
for name,level,reason,build in cases:build(name)
# A phone video stores landscape pixels and a rotation: it must be judged by its shown size.
subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-y','-display_rotation','90','-i',str(work/'phone_portrait.mp4'),'-c','copy',str(work/'phone_rotated.mp4')],check=True)
cases.append(('phone_rotated.mp4','ready','',None))
(work/'garbage.mp4').write_bytes(os.urandom(5000));(work/'empty.mkv').write_bytes(b'')
(work/'truncated.mp4').write_bytes((work/'h264.mp4').read_bytes()[:3000])
for name in('garbage.mp4','empty.mkv','truncated.mp4'):cases.append((name,'unknown',"Couldn't check this file",None))
(work/'song.mp3').write_bytes(b'ID3');cases.append(('song.mp3',None,None,None))
harness='''
import fs from 'fs';
const html=fs.readFileSync(process.argv[2],'utf8');
const code=html.slice(html.indexOf('/*probe*/'),html.indexOf('/*end probe*/'));
const {check}=new Function(code+';return {check};')();
const out={};
for(const f of fs.readdirSync(process.argv[3]))if(/\\.(mp4|mkv|webm|mov|mp3)$/.test(f))out[f]=await check(new File([fs.readFileSync(process.argv[3]+'/'+f)],f));
console.log(JSON.stringify(out));
'''
(work/'run.mjs').write_text(harness)
result=json.loads(subprocess.run(['node',str(work/'run.mjs'),str(root/'content/transfer.html'),str(work)],check=True,capture_output=True,text=True).stdout)
for name,level,reason,_ in cases:
    got=result[name]
    if level is None:assert got is None,(name,got);continue
    assert got and got['level']==level and got['reason']==reason,(name,got,level,reason)
# Every wording the page shows has to be in the receiver's word list, or it can't be translated.
listed=set(re.findall(r'N_\("((?:[^"\\]|\\.)*)"\)',(root/'src/core/media_transfer.cpp').read_text()))
used=set(re.findall(r"text\('((?:[^'\\]|\\.)*)'\)",html))|set(re.findall(r"fill\('((?:[^'\\]|\\.)*)'",html))|set(re.findall(r"reason\('(?:ready|limits|convert)','((?:[^'\\]|\\.)*)'\)",html))|set(re.findall(r'data-text="([^"]*)"',html))
used-={''}
used|={'Reconnecting…','Waiting','Sending','Saved','Transfer cancelled','Ready to play','Plays with limits','Needs conversion',"Couldn't check this file",'Choose a file smaller than 2 GB','Unsupported file type or filename','Unsupported folder name'}
missing={w.replace("\\'","'") for w in used}-{w.replace('\\"','"') for w in listed}
assert not missing,missing
print(f'PASS upload page file check: {len(cases)} files judged as expected, unreadable files left unjudged, every page wording is translatable')
shutil.rmtree(work)
