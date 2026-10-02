#!/usr/bin/env python3
"""Exercise the real LAN receiver with local HTTP uploads and malformed requests."""
import hashlib,http.client,json,os,re,socket,subprocess,tempfile,time,urllib.parse
from pathlib import Path
root=Path(__file__).resolve().parents[2];os.chdir(root)
folder=Path(tempfile.mkdtemp(prefix='coffee-transfer-test-'));received=folder/'Received';received.mkdir(parents=True)
# Leftovers of earlier sessions: temporary uploads and week-old partials go at start, a fresh partial stays.
(received/'.upload-deadbeef-1.part').write_bytes(b'old');(received/'.resume-aaaaaaaa-100.part').write_bytes(b'fresh')
(received/'.resume-bbbbbbbb-100.part').write_bytes(b'stale');os.utime(received/'.resume-bbbbbbbb-100.part',(time.time()-8*86400,)*2)
p=subprocess.Popen(['build-desktop/media-transfer-test',str(received)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1)
url=''
while not url:
 line=p.stdout.readline();assert line,'receiver did not start'
 if line.startswith('http://'):url=line.strip()
u=urllib.parse.urlsplit(url);key=u.path.strip('/').split('/')[-1]
def status(command='status'):
 p.stdin.write(command+'\n');p.stdin.flush()
 while True:
  line=p.stdout.readline();assert line
  if line.startswith('{'):return json.loads(line)
def request(name,body=b'x',extra=None,method='PUT',path=None):
 c=http.client.HTTPConnection(u.hostname,u.port,timeout=4)
 headers={'X-Upload-Key':key,'X-File-Name':urllib.parse.quote(name,safe=''),'Content-Type':'application/octet-stream'}
 if extra:headers.update(extra)
 c.request(method,path or u.path+'upload',body,headers);r=c.getresponse();data=r.read();code=r.status;c.close();return code,data
try:
 code,html=request('',b'',method='GET',path=u.path);assert code==200 and b'Send to your Wii U' in html
 assert request('video.mp4',extra={'X-Upload-Key':'wrong'})[0]==404
 assert request('video.mp4',extra={'Origin':'https://evil.invalid'})[0]==403
 assert request('video.mp4',extra={'Host':'evil.invalid'})[0]==403
 assert request('video.mp4',extra={'Transfer-Encoding':'chunked'})[0]==411
 assert request('video.mp4',b'',{'Content-Length':'2147483648'})[0]==413
 for name in ['../escape.mp4','/escape.mp4','bad\\path.mp4','bad\0.mp4','app.wuhb','settings.json','.hidden.mp4']:
  assert request(name)[0]==400,name
 payload=os.urandom(5*1024*1024+37)
 code,data=request('My video.mp4',payload);assert code==200 and json.loads(data)['saved']=='My video.mp4'
 assert hashlib.sha256((received/'My video.mp4').read_bytes()).digest()==hashlib.sha256(payload).digest()
 code,data=request('My video.mp4',b'second');assert code==200 and json.loads(data)['saved']=='My video (2).mp4'
 assert (received/'My video.mp4').read_bytes()==payload
 assert request('字幕.srt',b'1\n00:00:00,000 --> 00:00:01,000\nHello\n')[0]==200
 outside=folder/'outside';outside.write_bytes(b'unchanged');(received/'link.mp4').symlink_to(outside)
 assert request('link.mp4',b'new')[0]==200 and outside.read_bytes()==b'unchanged'
 def partial(name):
  s=socket.create_connection((u.hostname,u.port),timeout=4)
  s.sendall((f'PUT {u.path}upload HTTP/1.1\r\nHost: {u.netloc}\r\nX-Upload-Key: {key}\r\nX-File-Name: {name}\r\nContent-Length: 1000000\r\n\r\n').encode()+b'x'*65536)
  for _ in range(40):
   if status()['active']:break
   time.sleep(.025)
  assert status()['active'];return s
 s=partial('incomplete.mp4');s.shutdown(socket.SHUT_WR);assert b'409' in s.recv(8192);s.close()
 s=partial('cancelled.mp4');status('cancel');assert b'409' in s.recv(8192);s.close()
 for _ in range(40):
  if not status()['active']:break
  time.sleep(.025)
 assert not (received/'incomplete.mp4').exists() and not (received/'cancelled.mp4').exists()
 assert not list(received.glob('.upload-*'))
 status('limit');assert request('write-failure.mp4',b'x'*262144)[0]==409;status('unlimit')
 assert not (received/'write-failure.mp4').exists() and not list(received.glob('.upload-*'))
 assert not (received/'.upload-deadbeef-1.part').exists() and not (received/'.resume-bbbbbbbb-100.part').exists() and (received/'.resume-aaaaaaaa-100.part').exists()
 (received/'.resume-aaaaaaaa-100.part').unlink()
 def call(method,path,extra=None,body=None):
  c=http.client.HTTPConnection(u.hostname,u.port,timeout=6);headers={'X-Upload-Key':key}
  if extra:headers.update(extra)
  c.request(method,u.path+path,body,headers);r=c.getresponse();data=r.read();c.close();return r.status,json.loads(data) if data.startswith(b'{') else data
 def resumable(name,uid,total,body,offset=None,extra=None):
  headers={'X-Upload-Id':uid,'X-Total-Size':str(total)}
  if offset is not None:headers['X-Upload-Offset']=str(offset)
  if extra:headers.update(extra)
  code,data=request(name,body,headers);return code,json.loads(data)
 def query(uid,total):return call('GET','upload',{'X-Upload-Id':uid,'X-Total-Size':str(total)})
 def wait_idle():
  for _ in range(80):
   if not status()['active']:return
   time.sleep(.025)
  raise AssertionError('receiver stayed busy')
 # info: free space, the limit and the folders that exist
 code,info=call('GET','info');assert code==200 and info['max']==2147483647 and (info['free']>0 or info['free']==-1) and info['folders']==[]
 assert call('GET','info',{'X-Upload-Key':'wrong'})[0]==404 and call('POST','info')[0]==404
 # a file sent in two parts: the first leaves it incomplete, the query says where it stopped, the rest completes it
 whole=os.urandom(3*1024*1024+11);uid='0123456789abcdef0123456789abcdef';cut=1024*1024+5
 assert query(uid,len(whole))==(200,{'offset':0})
 code,data=resumable('Resumed movie.mp4',uid,len(whole),whole[:cut],0);assert code==200 and data=={'offset':cut},(code,data)
 assert not (received/'Resumed movie.mp4').exists() and (received/f'.resume-{uid}-{len(whole)}.part').stat().st_size==cut
 assert query(uid,len(whole))==(200,{'offset':cut}) and query(uid,len(whole)+1)==(200,{'offset':0})
 code,data=resumable('Resumed movie.mp4',uid,len(whole),whole[cut+3:],cut+3);assert code==409 and data['offset']==cut,(code,data)
 code,data=resumable('Resumed movie.mp4',uid,len(whole),whole[cut+7:],cut+7);assert code==409 and data['offset']==cut
 code,data=resumable('Resumed movie.mp4','fedcba9876543210fedcba9876543210',len(whole),whole[cut:],cut);assert code==409 and data['offset']==0
 code,data=resumable('Resumed movie.mp4',uid,len(whole),whole[cut:],cut);assert code==200 and data=={'saved':'Resumed movie.mp4'},(code,data)
 assert (received/'Resumed movie.mp4').read_bytes()==whole and not list(received.glob('.resume-*'))
 s=status();assert s['resumed_from']==cut and s['completed']>=1
 # a body that is too long, an offset beyond the file, malformed ids and headers
 assert resumable('a.mp4',uid,100,b'x'*200,0)[0]==400 and resumable('a.mp4',uid,100,b'x',101)[0]==400
 for bad in ['SHORT','0123456789ABCDEF','zzzzzzzzzzzz',' ','a'*65]:
  assert resumable('a.mp4',bad,100,b'x',0)[0]==400,bad
  assert call('GET','upload',{'X-Upload-Id':bad,'X-Total-Size':'100'})[0]==400
 assert call('GET','upload',{'X-Upload-Id':uid,'X-Total-Size':'0'})[0]==400 and call('GET','upload',{'X-Upload-Id':uid})[0]==400
 assert request('a.mp4',b'x',{'X-Upload-Offset':'1','X-Total-Size':'2'})[0]==400 and request('a.mp4',b'x',{'X-Total-Size':'9'})[0]==400
 assert resumable('a.mp4',uid,2147483648,b'x',0)[0]==413
 assert not list(received.glob('.resume-*')) and not (received/'a.mp4').exists()
 # the Wi-Fi drops mid-body: the bytes that arrived are kept, and the sender continues from what the receiver reports
 whole=os.urandom(2*1024*1024+3);uid='aaaabbbbccccdddd1111222233334444'
 def start_body(uid,total,length,name='broken.mp4',offset=None,extra=None):
  s=socket.create_connection((u.hostname,u.port),timeout=4)
  head=f'PUT {u.path}upload HTTP/1.1\r\nHost: {u.netloc}\r\nX-Upload-Key: {key}\r\nX-File-Name: {name}\r\nX-Upload-Id: {uid}\r\nX-Total-Size: {total}\r\nContent-Length: {length}\r\n'
  if offset is not None:head+=f'X-Upload-Offset: {offset}\r\n'
  for k,v in (extra or {}).items():head+=f'{k}: {v}\r\n'
  return s,head.encode()+b'\r\n'
 s,head=start_body(uid,len(whole),len(whole));s.sendall(head+whole[:300000])
 for _ in range(80):
  st=status()
  if st['active'] and st['received']>=300000:break
  time.sleep(.025)
 s.close();wait_idle();st=status()
 assert st['interrupted'] and st['error'] and not st['active'],st
 code,q=query(uid,len(whole));kept=q['offset'];assert code==200 and 300000<=kept<=len(whole)
 assert kept==(received/f'.resume-{uid}-{len(whole)}.part').stat().st_size and not (received/'broken.mp4').exists()
 code,data=resumable('broken.mp4',uid,len(whole),whole[kept:],kept);assert code==200 and data=={'saved':'broken.mp4'},(code,data)
 assert (received/'broken.mp4').read_bytes()==whole and not list(received.glob('.resume-*'))
 st=status();assert st['resumed_from']==kept and not st['interrupted'] and st['saved_name']=='broken.mp4'
 # restarting from zero throws the old partial away
 s,head=start_body('9999888877776666',1000,1000,'restart.mp4');s.sendall(head+b'a'*400);time.sleep(.3);s.close();wait_idle()
 assert query('9999888877776666',1000)==(200,{'offset':400}) and resumable('restart.mp4','9999888877776666',1000,b'b'*1000,0)[0]==200
 assert (received/'restart.mp4').read_bytes()==b'b'*1000
 # a new connection waiting behind a silent one takes over (the sender's retry after a Wi-Fi drop): no wait for the old timeout
 s,head=start_body('7777666655554444',5000,5000,'stall.mp4');s.sendall(head+b'c'*1200);time.sleep(.3)
 t0=time.time();code,q=query('7777666655554444',5000);assert code==200 and q['offset']==1200 and time.time()-t0<6,(code,q,time.time()-t0)
 s.close();wait_idle()
 code,data=resumable('stall.mp4','7777666655554444',5000,b'c'*3800,1200);assert code==200 and (received/'stall.mp4').read_bytes()==b'c'*5000
 # cancelling on the Wii U discards the partial: the sender is told without an offset, so it doesn't retry
 s,head=start_body('5555444433332222',9000,9000,'cancelled2.mp4');s.sendall(head+b'd'*3000)
 for _ in range(80):
  if status()['active']:break
  time.sleep(.025)
 status('cancel');reply=s.recv(8192);s.close();assert b'409' in reply and b'"offset"' not in reply,reply
 wait_idle();assert query('5555444433332222',9000)==(200,{'offset':0}) and not list(received.glob('.resume-*'))
 # folders: created on demand, listed by info, refused when they could leave Received
 code,data=resumable('ep1.mkv','1111000011110000',6,b'abcdef',0,{'X-Folder':urllib.parse.quote('Anime')});assert code==200 and data=={'saved':'ep1.mkv'}
 assert (received/'Anime'/'ep1.mkv').read_bytes()==b'abcdef' and status()['saved_name']=='Anime/ep1.mkv'
 code,data=resumable('ep1.mkv','2222000022220000',3,b'xyz',0,{'X-Folder':'Anime'});assert (received/'Anime'/'ep1 (2).mkv').read_bytes()==b'xyz'
 for bad in ['..','../out','a%2Fb','.hidden','x%3A','']:
  code,data=resumable('ep2.mkv','3333000033330000',3,b'xyz',0,{'X-Folder':bad})
  if bad=='': assert code==200 and (received/'ep2.mkv').exists()
  else: assert code==400 and 'error' in data,(bad,code)
 assert not (folder/'out').exists() and not (received/'.hidden').exists()
 code,info=call('GET','info');assert info['folders']==['Anime'],info
 code,data=request('plain.mp4',b'plain',{'X-Folder':'Series'});assert code==200 and (received/'Series'/'plain.mp4').read_bytes()==b'plain'
 # queue position and speed are reported while the file arrives
 s,head=start_body('4444333322221111',1000000,1000000,'measured.mp4',extra={'X-Queue-Index':'2','X-Queue-Count':'5'})
 s.sendall(head)
 for i in range(8):s.sendall(b'e'*50000);time.sleep(.2)
 st=status();assert st['active'] and st['queue_index']==2 and st['queue_count']==5 and st['speed']>0 and st['total']==1000000,st
 s.close();wait_idle();assert status()['interrupted']
 assert resumable('measured.mp4','4444333322221111',1000000,b'e'*1000000,0)[0]==200
 assert status()['queue_index']==0
 print('PASS resume after interruption, offset correction, stale cleanup, cancel discards partial, folders, info, queue and speed reporting')
 s=partial('leaving.mp4');p.stdin.write('quit\n');p.stdin.flush();p.wait(timeout=4);s.close()
 assert not (received/'leaving.mp4').exists() and not list(received.glob('.upload-*'))
 print('PASS upload bytes, duplicate/Unicode names, symlink protection, auth/origin/host checks, request limits, disconnect/cancel/close cleanup')
 print('Artifacts:',folder)
finally:
 if p.poll() is None:p.stdin.write('quit\n');p.stdin.flush();p.wait(timeout=4)
