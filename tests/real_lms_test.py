"""Real, temporary LMS plus real yeney and loopback Sonos. Never owner endpoints.

The LMS distribution is downloaded, not vendored. GPL server/reference code is
never incorporated into the bridge. Every spawned process is reaped on failure.
"""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import json
import os
from pathlib import Path
import select
import signal
import socket
import socketserver
import struct
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
from urllib.parse import quote, unquote
from urllib.request import urlopen
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from queue_binary_test import Coordinator, Device, HTTP, wav, alac
from decoders_test import alac_parts, mp4
HOME=Path(os.environ.get('YENEY_REAL_LMS_HOME','/tmp/yeney-real-lms-9.1.1'))
OWNER_CLI=[]
URL='https://downloads.lms-community.org/LyrionMusicServer_v9.1.1/lyrionmusicserver-9.1.1.tgz'

class Tap:
    def __init__(self):
        self.listener=socket.socket();self.listener.bind(('127.0.0.1',0));self.listener.listen(2);self.listener.settimeout(.1)
        self.port=self.listener.getsockname()[1];self.closed=False;self.sockets=[];self.records=[];self.errors=[]
        self.worker=threading.Thread(target=self.run);self.worker.start()
    def run(self):
        try:
            while not self.closed:
                try:client,_=self.listener.accept()
                except socket.timeout:continue
                peer=socket.create_connection(('127.0.0.1',3483),timeout=3)
                self.sockets += [client,peer];buffers={client:bytearray(),peer:bytearray()}
                while not self.closed:
                    ready,_,_=select.select([client,peer],[],[],.1)
                    ended=False
                    for source in ready:
                        data=source.recv(65536)
                        if not data:ended=True;break
                        target=peer if source is client else client;target.sendall(data)
                        buffer=buffers[source];buffer.extend(data)
                        while len(buffer)>=(8 if source is client else 2):
                            header=8 if source is client else 2
                            size=struct.unpack('!I',buffer[4:8])[0] if source is client else struct.unpack('!H',buffer[:2])[0]
                            if len(buffer)<header+size:break
                            packet=bytes(buffer[:header+size]);del buffer[:header+size]
                            op=packet[:4].decode() if source is client else packet[2:6].decode()
                            body=packet[8:] if source is client else packet[6:]
                            record=dict(direction='player' if source is client else 'server',op=op,when=time.monotonic(),body=body.hex())
                            if op=='STAT':record['event']=body[:4].decode()
                            if op=='strm':record['command']=chr(body[0])
                            self.records.append(record)
                    if ended:break
                client.close();peer.close()
        except OSError as e:
            if not self.closed:self.errors.append(str(e))
    def close(self):
        self.closed=True;self.worker.join(4);self.listener.close()
        for s in self.sockets:s.close()
        assert not self.worker.is_alive(), 'SlimProto proxy did not stop'

class CLIProxy(socketserver.ThreadingTCPServer):
    allow_reuse_address=True
    daemon_threads=True
    def __init__(self):
        self.records=[];self.closing=False;owner=self
        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                wire=self.rfile.readline()
                if not wire:return
                response=bytearray()
                with socket.create_connection(('127.0.0.1',19090),timeout=30) as peer:
                    peer.sendall(wire)
                    if wire.strip()==b'listen 1':
                        peer.settimeout(.1)
                        while not owner.closing:
                            try:chunk=peer.recv(65536)
                            except socket.timeout:continue
                            if not chunk:break
                            self.wfile.write(chunk);self.wfile.flush()
                        return
                    while not response.endswith(b'\n'):
                        chunk=peer.recv(65536)
                        if not chunk:break
                        response.extend(chunk)
                owner.records.append(dict(request=wire.decode().strip(),reply=response.decode().strip(),when=time.monotonic()))
                self.wfile.write(response)
        super().__init__(('127.0.0.1',9090),Handler)
        self.worker=threading.Thread(target=self.serve_forever);self.worker.start()
    def close(self):self.closing=True;self.shutdown();self.server_close();self.worker.join()

class Remote:
    def __init__(self,body):
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_HEAD(self):self.respond(False)
            def do_GET(self):self.respond(True)
            def respond(self,content):
                self.send_response(200);self.send_header('Content-Type','audio/flac')
                self.send_header('Content-Length',str(len(body)));self.end_headers()
                if content:
                    try:self.wfile.write(body)
                    except (BrokenPipeError,ConnectionResetError):pass
        self.server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        self.url='http://127.0.0.1:'+str(self.server.server_port)+'/test-radio.flac'
        self.worker=threading.Thread(target=self.server.serve_forever);self.worker.start()
    def close(self):self.server.shutdown();self.server.server_close();self.worker.join()

def cli(command,player=''):
    wire=(player+' ' if player else '')+command
    with socket.create_connection(('127.0.0.1',19090),timeout=30) as sock:
        sock.sendall((wire+'\n').encode());data=bytearray()
        while not data.endswith(b'\n'):
            chunk=sock.recv(65536)
            if not chunk:raise AssertionError('LMS closed CLI for '+wire)
            data.extend(chunk)
    OWNER_CLI.append(dict(request=wire,reply=data.decode().strip(),when=time.monotonic()))
    return [unquote(t) for t in data.decode().strip().split()]

def wait(predicate, timeout=15, label='evidence'):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        if predicate():return
        time.sleep(.05)
    raise AssertionError('real LMS deadline: '+label)

def stop(proc):
    if proc is None:return
    try:os.killpg(proc.pid,signal.SIGTERM)
    except ProcessLookupError:pass
    try:proc.wait(10)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid,signal.SIGKILL);proc.wait()

def install():
    if (HOME/'slimserver.pl').is_file():return
    HOME.mkdir(parents=True,exist_ok=True)
    archive=HOME/'distribution.tgz'
    with urlopen(URL,timeout=60) as source,archive.open('wb') as out:
        while chunk:=source.read(1024*1024):out.write(chunk)
    with tarfile.open(archive) as tar:
        prefix=tar.getmembers()[0].name.split('/')[0]+'/'
        for member in tar.getmembers():
            if not member.name.startswith(prefix):continue
            member.name=member.name[len(prefix):]
            if member.name:tar.extract(member,HOME,filter='data')

def perl_environment(base):
    prefix=Path(os.environ.get('YENEY_REAL_LMS_PERL5','/tmp/yeney-real-lms-perl5'))
    env=dict(os.environ,PERL5LIB=str(prefix/'lib/perl5')+':'+os.environ.get('PERL5LIB',''),PERL_MM_USE_DEFAULT='1')
    probe=['perl','-MIO::Socket::SSL','-MNet::SSLeay','-e','print "Perl SSL $IO::Socket::SSL::VERSION / $Net::SSLeay::VERSION\\n"']
    if subprocess.run(probe,env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode:
        build=base/'perl-build';build.mkdir()
        urls=('https://cpan.metacpan.org/authors/id/C/CH/CHRISN/Net-SSLeay-1.96.tar.gz',
              'https://cpan.metacpan.org/authors/id/S/SU/SULLR/IO-Socket-SSL-2.099.tar.gz')
        with (base/'perl-install.log').open('w') as log:
            for index,url in enumerate(urls):
                archive=build/f'{index}.tgz'
                with urlopen(url,timeout=60) as source,archive.open('wb') as out:
                    while chunk:=source.read(1024*1024):out.write(chunk)
                with tarfile.open(archive) as tar:
                    root=build/tar.getmembers()[0].name.split('/')[0]
                    tar.extractall(build,filter='data')
                for command in (['perl','Makefile.PL','INSTALL_BASE='+str(prefix)],['make','-j4'],['make','install']):
                    print('$ '+' '.join(command),file=log,flush=True)
                    subprocess.run(command,cwd=root,env=env,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,check=True)
    result=subprocess.run(probe,env=env,capture_output=True,text=True,check=True)
    print('REAL LMS private dependency: '+result.stdout.strip(),flush=True)
    return env

base=Path(tempfile.mkdtemp(prefix='yeney-real-lms-acceptance-'))
print('REAL LMS transcript directory: '+str(base),flush=True)
install()
perl_env=perl_environment(base)
for name in ('library','prefs/plugin','cache','log','playlists'): (base/name).mkdir(parents=True,exist_ok=True)
for i,rate in enumerate((44100,48000,44100,48000)):
    path=base/f'input-{i}.wav';path.write_bytes(wav(rate,24,noise=True))
    subprocess.run(['flac','-s','-f','-T',f'TITLE=Title & {i}','-T','ARTIST=Artist <one>',
                    '-T','ALBUM=Album & fixture','-T',f'TRACKNUMBER={i+1}', '-o',str(base/f'library/{i}.flac'),str(path)],check=True)
for i,rate in ((4,44100),(5,48000)):
    cookie,packets=alac_parts(alac(24))
    cookie=cookie[:20]+struct.pack('!I',rate)
    count=(24*rate+4095)//4096
    (base/f'library/{i}.m4a').write_bytes(mp4(cookie,[packets[0]]*count,count*4096))
(base/'prefs/server.prefs').write_text('---\naudiodir: '+str(base/'library')+'\nplaylistdir: '+str(base/'playlists')+'\ncheckVersion: 0\nlanguage: EN\n')
# Seed CLI preferences so first-start plugin migration cannot replace the port.
(base/'prefs/plugin/cli.prefs').write_text('---\ncliport: 19090\n_version: 1\n')
lms=bridge=tap=sonos=remote=proxy=None
lms_log=(base/'lms.log').open('w');bridge_log=(base/'bridge.log').open('w')
try:
    command=['perl',str(HOME/'slimserver.pl'),'--prefsdir',str(base/'prefs'),'--cachedir',str(base/'cache'),
             '--logdir',str(base/'log'),'--httpport','9000','--httpaddr','127.0.0.1','--cliport','19090',
             '--cliaddr','127.0.0.1','--playeraddr','127.0.0.1','--streamaddr','127.0.0.1',
             '--advertiseaddr','127.0.0.1','--nosetup','--norestart']
    lms=subprocess.Popen(command,stdout=lms_log,stderr=subprocess.STDOUT,start_new_session=True,env=perl_env)
    def available():
        if lms.poll() is not None:raise AssertionError('real LMS exited: '+(base/'lms.log').read_text())
        try:return cli('version ?')[-1]=='9.1.1'
        except OSError:return False
    wait(available,30,'LMS startup')
    print('REAL LMS version: '+cli('version ?')[-1],flush=True)
    proxy=CLIProxy()
    cli('rescan')
    wait(lambda:cli('rescan ?')[-1]=='0' and int(cli('info total songs ?')[-1])>=6,90,'library scan')
    rows=cli('songs 0 100 tags:u');tracks=[]
    for field in rows:
        key,sep,value=field.partition(':')
        if key=='id':tracks.append({'id':value})
        elif tracks and sep:tracks[-1][key]=value
    tracks.sort(key=lambda t:t.get('url',''))
    (base/'library.json').write_text(json.dumps(tracks,indent=2))
    ids=[t['id'] for t in tracks if t.get('url','').endswith('.flac')]
    alac_ids=[t['id'] for t in tracks if t.get('url','').endswith('.m4a')]
    assert len(ids)==4 and len(alac_ids)==2,tracks
    sonos=Coordinator(('127.0.0.1',1400),Device);sonos.follow_controls=True
    sonos.expected_title_prefix='';sonos.expected_albums=['Album & fixture','No Album','',None]
    tap=Tap()
    env=dict(os.environ,YENEY_PLAYBACK_MODE='queue',YENEY_POLL='events',YENEY_QUEUE_DISK_MIB='64',STATE_DIRECTORY=str(base))
    bridge=subprocess.Popen([str(ROOT/'yeney'),'--room=Study','--ip=127.0.0.1',f'--server=127.0.0.1:{tap.port}'],
                            stdout=bridge_log,stderr=subprocess.STDOUT,env=env,start_new_session=True)
    wait(lambda:any(r['op']=='HELO' for r in tap.records),10,'HELO')
    hello=next(r for r in tap.records if r['op']=='HELO');mac=':'.join(f'{v:02x}' for v in bytes.fromhex(hello['body'])[2:8])
    wait(lambda:cli('connected ?',mac)[-1]=='1',10,'player connected')
    def packets(event):return [r for r in tap.records if r.get('event')==event]
    def items():
        with sonos.lock:return [i for i in sonos.items if 'resource' in i]
    def select_item(index):
        wait(lambda:len(items())>index,15,'prepared speaker item')
        item=items()[index]
        with sonos.lock:
            sonos.position=dict(Track=str(sonos.items.index(item)+1),TrackURI=item['uri'],RelTime='00:00:00')
            sonos.transport_state='PLAYING'
    def identity(index):
        wait(lambda:int(cli('playlist index ?',mac)[-1])==index,3,'LMS/audible index')
        assert not tap.errors,tap.errors
        text=(base/'bridge.log').read_text()
        assert 'rearm-loop room=' not in text and 'state=drain' not in text,text[-8000:]
        assert 'queue LMS mutation ' not in text,text[-8000:]
    cli('playlistcontrol cmd:load track_id:'+','.join(ids),mac)
    wait(lambda:len(items())>=1,15,'initial takeover')
    select_item(0);wait(lambda:len(packets('STMs'))>=1)
    identity(0)
    assert len(sonos.actions('Play'))==1,sonos.calls
    controls_only=os.environ.get('YENEY_REAL_LMS_CONTROLS_ONLY')=='1'
    for index in (() if controls_only else (1,2,3)):
        # Keep the fake's RelTime continuous until the natural boundary.
        for second in range(1,24):
            time.sleep(1)
            with sonos.lock:sonos.position['RelTime']=f'00:00:{second:02}'
        select_item(index);wait(lambda:len(packets('STMs'))>=index+1)
        identity(index)
    if not controls_only: print('PASS real LMS 9.1.1 album: FLAC 44.1/48 kHz, three audible joins, matching LMS index, no CLI mutation/rearm/restart',flush=True)
    for index in ((1,0) if controls_only else (2,3)):
        before=len(packets('STMs'));cli(f'playlist index {index}',mac)
        wait(lambda:len(packets('STMs'))>before,15,'LMS next/previous acknowledgement')
        identity(index)
    cli('pause 1',mac);wait(lambda:sonos.transport_state=='PAUSED_PLAYBACK')
    time.sleep(10);cli('play',mac);wait(lambda:sonos.transport_state=='PLAYING')
    before=len(packets('STMs'))
    cli('time 12',mac);wait(lambda:any(a[1].get('Unit')=='TRACK_NR' for a in sonos.actions('Seek')),10,'seek')
    wait(lambda:len(packets('STMs'))>before and sonos.transport_state=='PLAYING',10,'seek confirmed start')
    # A relay seek is a newly rendered remaining source, never REL_TIME on
    # the previous asset. Its duration proves LMS applied the source offset.
    duration=items()[0]['resource']['duration']
    seconds=sum(int(v)*factor for v,factor in zip(duration.split(':'),(3600,60,1)))
    assert 10<=seconds<=15,('seek remaining duration',duration)
    wait(lambda:9<=float(cli('time ?',mac)[-1])<=14,3,'LMS seek source offset')
    assert not any(a[1].get('Unit')=='REL_TIME' for a in sonos.actions('Seek'))
    cli('playlist move 1 2',mac)
    cli('playlistcontrol cmd:add track_id:'+ids[0],mac)
    cli('playlist delete 4',mac)
    time.sleep(1.2)
    identity(0 if controls_only else 3)
    stops=len(sonos.actions('Stop'))
    cli('stop',mac);wait(lambda:sonos.transport_state=='STOPPED' and len(sonos.actions('Stop'))>stops)
    wait(lambda:cli('mode ?',mac)[-1]=='stop',3,'explicit stop')
    time.sleep(.2)
    before=len(packets('STMs'));plays=len(sonos.actions('Play'));cli('play',mac)
    wait(lambda:len(packets('STMs'))>before and len(sonos.actions('Play'))==plays+1,15,'stop/new play')
    identity(0 if controls_only else 3)
    print('PASS real LMS controls: next/previous, pause/resume, seek, playlist edit, stop/new play; one Play per start, no mutation echo',flush=True)
    cli('stop',mac)
    for position,rate in enumerate((44100,48000)):
        cli('playlistcontrol cmd:load track_id:'+alac_ids[position],mac)
        wait(lambda:any(i['metadata'].get('title','').split(' - ')[-1]==str(4+position)
                        and i['resource'].get('sampleFrequency')==str(rate) for i in items()),15,'ALAC prepared')
        assert sum(r['op']=='HELO' for r in tap.records)==1,tap.records
        identity(0)
        wait(lambda:packets('STMd') and packets('STMd')[-1]['when']>=packets('STMs')[-1]['when'],10,'ALAC fully produced')
        cli('stop',mac)
    print('PASS real LMS ALAC sources: generated MP4 at 44.1/48 kHz, LMS default FLAC transcoding, finite queue starts, one connection HELO',flush=True)
    remote_path=base/'radio.flac';remote_path.write_bytes((base/'library/0.flac').read_bytes())
    # LMS's remote scanner needs a probe-sized body; constant PCM compresses
    # below that size. Valid FLAC padding preserves the exact audio frames.
    subprocess.run(['metaflac','--add-padding=65536',str(remote_path)],check=True)
    remote=Remote(remote_path.read_bytes())
    cli('stop',mac);cli('playlist clear',mac);cli('playlist add '+remote.url,mac)
    wait(lambda:cli('playlist tracks ?',mac)[-1]=='1',15,'remote playlist addition')
    cli('playlistcontrol cmd:add track_id:'+ids[0],mac)
    wait(lambda:cli('playlist tracks ?',mac)[-1]=='2',15,'remote successor addition')
    starts=len(packets('STMs'));done=len(packets('STMd'))
    cli('playlist index 0',mac);cli('play',mac)
    wait(lambda:len(packets('STMs'))>starts,15,'remote HTTP start')
    started=packets('STMs')[-1]['when'];identity(0)
    for second in range(1,7):
        time.sleep(1)
        with sonos.lock:sonos.position['RelTime']=f'00:00:{second:02}'
        if second<=3:assert len(packets('STMd'))==done,tap.records[-12:]
    wait(lambda:len(packets('STMd'))>done,3,'remote near-end STMd')
    assert packets('STMd')[-1]['when']-started>=3.5,tap.records[-12:]
    assert sum(r['op']=='HELO' for r in tap.records)==1
    assert (base/'bridge.log').read_text().count('ownership acquired')>=1
    assert not [r for r in proxy.records if not (' status ' in r['request'] or r['request'].endswith(' ?'))],proxy.records
    print('PASS real LMS remote HTTP: local radio-like FLAC, full production plus <20 s audible gate, no idle successor/CLI restart, one connection HELO',flush=True)

finally:
    failures=[]
    if proxy:proxy.closing=True
    if sonos:sonos.closing=True
    def cleanup(action):
        try:action()
        except Exception as error:failures.append(error)
    cleanup(lambda:stop(bridge))
    if tap:
        cleanup(tap.close);(base/'slimproto.json').write_text(json.dumps(tap.records,indent=2))
    if proxy:
        cleanup(proxy.close);(base/'cli.json').write_text(json.dumps(proxy.records,indent=2))
    if sonos:
        def ownership_restored():
            owned={entry[1]['EnqueuedURI'] for entry in sonos.actions('AddURIToQueue')}
            assert not any(i['uri'] in owned for i in sonos.items) and sonos.crossfade,'owned cleanup/crossfade'
            assert sonos.verified_save and sonos.snapshots[sonos.last_save]==sonos.original,'verified Before YeneY backup'
        cleanup(ownership_restored)
        cleanup(sonos.close)
    if remote:cleanup(remote.close)
    cleanup(lambda:stop(lms));bridge_log.close();lms_log.close()
    (base/'owner-cli.json').write_text(json.dumps(OWNER_CLI,indent=2))
    if failures:raise failures[0]
