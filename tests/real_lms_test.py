"""Real, temporary LMS plus real yeney and loopback Sonos. Never owner endpoints.

The LMS distribution is downloaded, not vendored. GPL server/reference code is
never incorporated into the bridge. Every spawned process is reaped on failure.
"""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import json
import os
import re
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
tempfile.tempdir = (os.environ.get('TMPDIR') or '/var/tmp')
import threading
import time
from urllib.parse import quote, unquote
from urllib.request import urlopen
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from queue_binary_test import Coordinator, Device, HTTP, wav, alac
from decoders_test import alac_parts, mp4
from length_test import alac as length_alac
from hires_audio import pcm as hires_pcm, wave as hires_wave, alac as hires_alac, raw24
import numpy as np
CACHE=Path(os.environ.get('YENEY_TEST_CACHE',Path.home()/'.cache/yeney-test')).expanduser()
CACHE.mkdir(parents=True,exist_ok=True)
LMS_HOME=Path(os.environ.get('YENEY_REAL_LMS_HOME',CACHE/'real-lms-9.1.1'))
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
    def __init__(self,body,kind="flac",endless=False):
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_HEAD(self):self.respond(False)
            def do_GET(self):self.respond(True)
            def respond(self,content):
                self.send_response(200);self.send_header('Content-Type','audio/mpeg' if kind=='mp3' else 'audio/flac')
                if not endless:self.send_header('Content-Length',str(len(body)))
                else:self.send_header('icy-name','Loopback endless radio')
                self.end_headers()
                if content:
                    try:
                        while True:
                            self.wfile.write(body);self.wfile.flush()
                            if not endless:break
                            time.sleep(.03)
                    except (BrokenPipeError,ConnectionResetError):pass
        self.server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        self.url='http://127.0.0.1:'+str(self.server.server_port)+'/test-radio.'+kind
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
    if (LMS_HOME/'slimserver.pl').is_file():
        print('REAL LMS cache reused: '+str(LMS_HOME),flush=True)
        return
    LMS_HOME.mkdir(parents=True,exist_ok=True)
    archive=LMS_HOME/'distribution.tgz'
    with urlopen(URL,timeout=60) as source,archive.open('wb') as out:
        while chunk:=source.read(1024*1024):out.write(chunk)
    with tarfile.open(archive) as tar:
        prefix=tar.getmembers()[0].name.split('/')[0]+'/'
        for member in tar.getmembers():
            if not member.name.startswith(prefix):continue
            member.name=member.name[len(prefix):]
            if member.name:tar.extract(member,LMS_HOME,filter='data')

def perl_environment(base):
    prefix=Path(os.environ.get('YENEY_REAL_LMS_PERL5',CACHE/'real-lms-perl5'))
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

from test_storage import workspace
base=workspace('real-lms')
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
for rate in (88200,96000):
    path=base/f'hires-{rate}.wav';path.write_bytes(wav(rate,24,noise=True))
    subprocess.run([str(LMS_HOME/'Bin/x86_64-linux/sox'),str(path),'-b','24',str(base/f'library/hires-{rate}.flac')],check=True)
unknown=bytearray((base/'library/hires-96000.flac').read_bytes())
unknown[21]&=0xf0;unknown[22:26]=bytes(4)
(base/'library/unknown-96000.flac').write_bytes(unknown)
native_sources={}
for kind in ('flac','alac'):
    for rate in (88200,96000):
        parts=[]
        for track in range(4):
            samples=hires_pcm(rate,4,start=track*rate*4)
            parts.append(samples)
            extension='flac' if kind=='flac' else 'm4a'
            name=f'native-{kind}-{rate}-{track}.{extension}'
            path=base/'library'/name
            if kind=='flac':
                source=base/(name+'.wav');source.write_bytes(hires_wave(samples,rate))
                subprocess.run(['flac','-s','-f','-T',f'TITLE=Native {track}','-T',f'ALBUM=Native {kind} {rate}',
                                '-T',f'TRACKNUMBER={track+1}','-o',str(path),str(source)],check=True)
            else:path.write_bytes(hires_alac(samples,rate))
        native_sources[kind,rate]=np.concatenate(parts)
# Own fixtures only: no private owner files enter the LMS library.
for rate in (44100,88200,96000):
    parts=[]
    for track in range(4):
        samples=hires_pcm(rate,4,start=track*rate*4);parts.append(samples)
        if track in (0,1):
            stored=np.concatenate((np.zeros((113,2),dtype=np.int32),samples,np.zeros((219,2),dtype=np.int32)))
            options=dict(trim=(113,len(samples))) if track==0 else dict(smpb=(113,219,len(samples)))
        else:
            stored=samples
            capacity=((len(samples)+4095)//4096+(1 if track==3 else 0))*4096
            options=dict(smpb=(0,capacity-len(samples),len(samples)))
            if track==3:options.update(zero_tail=True,declared=capacity)
        (base/'library'/f'native-lengthalac-{rate}-{track}.m4a').write_bytes(length_alac(stored,rate,**options))
    native_sources['lengthalac',rate]=np.concatenate(parts)
(base/'prefs/server.prefs').write_text('---\naudiodir: '+str(base/'library')+'\nplaylistdir: '+str(base/'playlists')+'\ncheckVersion: 0\nlanguage: EN\n')
# Seed CLI preferences so first-start plugin migration cannot replace the port.
(base/'prefs/plugin/cli.prefs').write_text('---\ncliport: 19090\n_version: 1\n')
lms=bridge=tap=sonos=remote=proxy=None
volume_only=os.environ.get('YENEY_REAL_LMS_VOLUME_ONLY')=='1'
extra_remotes=[]
lms_log=(base/'lms.log').open('w');bridge_log=(base/'bridge.log').open('w')
try:
    command=['perl',str(LMS_HOME/'slimserver.pl'),'--prefsdir',str(base/'prefs'),'--cachedir',str(base/'cache'),
             '--logdir',str(base/'log'),'--httpport','9000','--httpaddr','127.0.0.1','--cliport','19090',
             '--cliaddr','127.0.0.1','--playeraddr','127.0.0.1','--streamaddr','127.0.0.1',
             '--advertiseaddr','127.0.0.1','--norestart']
    lms=subprocess.Popen(command,stdout=lms_log,stderr=subprocess.STDOUT,start_new_session=True,env=perl_env)
    def available():
        if lms.poll() is not None:raise AssertionError('real LMS exited: '+(base/'lms.log').read_text())
        try:return cli('version ?')[-1]=='9.1.1'
        except OSError:return False
    wait(available,30,'LMS startup')
    print('REAL LMS version: '+cli('version ?')[-1],flush=True)
    proxy=CLIProxy()
    cli('rescan')
    wait(lambda:cli('rescan ?')[-1]=='0' and int(cli('info total songs ?')[-1])>=8,90,'library scan')
    rows=cli('songs 0 100 tags:u');tracks=[]
    for field in rows:
        key,sep,value=field.partition(':')
        if key=='id':tracks.append({'id':value})
        elif tracks and sep:tracks[-1][key]=value
    tracks.sort(key=lambda t:t.get('url',''))
    (base/'library.json').write_text(json.dumps(tracks,indent=2))
    ids=[t['id'] for t in tracks if t.get('url','').rsplit('/',1)[-1] in ('0.flac','1.flac','2.flac','3.flac')]
    alac_ids=[t['id'] for t in tracks if t.get('url','').rsplit('/',1)[-1] in ('4.m4a','5.m4a')]
    assert len(ids)==4 and len(alac_ids)==2,tracks
    sonos=Coordinator(('127.0.0.1',1400),Device);sonos.follow_controls=True;sonos.consume_stream=True
    sonos.volume=20;sonos.muted=0
    sonos.expected_title_prefix='';sonos.expected_albums=['Album & fixture','No Album','',None]+[f'Native {k} {r}' for k in ('flac','alac') for r in (88200,96000)]
    tap=Tap()
    env=dict(os.environ,YENEY_LMS_VOLUME='off',YENEY_VOLUME_MAX='100',YENEY_PLAYBACK_MODE='queue',YENEY_STREAM_PACING='sonos',YENEY_START_LEAD_MS='1000',YENEY_TIMING_PROBE='1',YENEY_POLL='events',YENEY_QUEUE_DISK_MIB='64',STATE_DIRECTORY=str(base))
    if volume_only:env['YENEY_LMS_VOLUME']='speaker'
    bridge=subprocess.Popen([str(ROOT/'yeney'),'--room=Study','--ip=127.0.0.1',f'--server=127.0.0.1:{tap.port}'],
                            stdout=bridge_log,stderr=subprocess.STDOUT,env=env,start_new_session=True)
    wait(lambda:any(r['op']=='HELO' for r in tap.records),10,'HELO')
    hello=next(r for r in tap.records if r['op']=='HELO')
    assert b'MaxSampleRate=192000' in bytes.fromhex(hello['body']),hello
    print('PASS queue HELO MaxSampleRate=192000; ordinary stream remains 48000',flush=True)
    mac=':'.join(f'{v:02x}' for v in bytes.fromhex(hello['body'])[2:8])
    wait(lambda:cli('connected ?',mac)[-1]=='1',10,'player connected')
    assert b'Model=yeney,' in bytes.fromhex(hello['body']) and b'HasDigitalOut=' not in bytes.fromhex(hello['body'])
    audio_page=urlopen('http://127.0.0.1:9000/settings/player/audio.html?player='+mac,timeout=10).read().decode()
    (base/'player-audio-settings.txt').write_text(audio_page)
    assert 'pref_digitalVolumeControl' not in audio_page, 'native volume preference unexpectedly offered'
    assert 'pref_transitionType' in audio_page, 'player audio settings did not render'
    print('PASS real LMS 9.1.1 native volume preference: Model=yeney; Settings > Player > Audio rendered; pref_digitalVolumeControl absent (HasDigitalOut defaults to 0); YeneY setting is sole source',flush=True)
    if volume_only:
        from volume_test import notify
        from types import SimpleNamespace
        wait(lambda:getattr(sonos,'volume',None)==int(float(cli('mixer volume ?',mac)[-1])),10,'initial volume')
        for value in (0,25,50,100):
            cli('mixer volume '+str(value),mac)
            wait(lambda:getattr(sonos,'volume',None)==value,5,'exact speaker volume '+str(value))
            assert int(float(cli('mixer volume ?',mac)[-1]))==value
        cli('mixer muting 1',mac)
        wait(lambda:getattr(sonos,'muted',0)==1,5,'speaker mute')
        n=len(sonos.volume_writes)
        sonos.volume=42;sonos.muted=0
        notify(SimpleNamespace(sonos=sonos,until=lambda f:wait(f,5,'volume subscription')))
        wait(lambda:int(float(cli('mixer volume ?',mac)[-1]))==42 and cli('mixer muting ?',mac)[-1]=='0',5,'app volume/mute in real LMS')
        time.sleep(.5)
        assert len(sonos.volume_writes)==n,'speaker echo'
        mutations=[r['request'] for r in proxy.records if ' mixer ' in r['request'] and not r['request'].endswith(' ?')]
        assert mutations==[mac+' mixer muting 0',mac+' mixer volume 42'],mutations
        print('PASS real LMS 9.1.1 exact mixer source: 0/25/50/100 and mute -> exact speaker values; app volume 42/mute 0 -> LMS once, raw MAC, no echo; audg gains never used',flush=True)
        raise SystemExit(0)
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
        assert all('reason=speaker-' in line for line in text.splitlines() if 'queue LMS mutation ' in line),text[-8000:]
    hires_only=os.environ.get('YENEY_REAL_LMS_HIRES_ONLY')=='1'
    if not hires_only:
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
        if not controls_only:
            assert len(sonos.actions('Play'))==1 and len(sonos.actions('AddURIToQueue'))==4
            assert (base/'bridge.log').read_text().count('ownership acquired')==1
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
        assert 'relay fault=queue flushed' not in (base/'bridge.log').read_text(),'LMS cancellation reported as renderer failure'
        identity(0 if controls_only else 3)
        stops=len(sonos.actions('Stop'))
        cli('stop',mac);wait(lambda:sonos.transport_state=='STOPPED' and len(sonos.actions('Stop'))>stops)
        wait(lambda:cli('mode ?',mac)[-1]=='stop',3,'explicit stop')
        time.sleep(.2)
        before=len(packets('STMs'));plays=len(sonos.actions('Play'));cli('play',mac)
        wait(lambda:len(packets('STMs'))>before and len(sonos.actions('Play'))==plays+1,15,'stop/new play')
        identity(0 if controls_only else 3)
        print('PASS real LMS controls: next/previous, pause/resume, seek, playlist edit, stop/new play; one Play per start, no mutation echo',flush=True)
        time.sleep(3.2)
        resume_plays=len(sonos.actions('Play'));resume_adds=len(sonos.actions('AddURIToQueue'))
        with sonos.lock:sonos.transport_state='PAUSED_PLAYBACK'
        wait(lambda:cli('mode ?',mac)[-1]=='pause',3,'external pause in LMS')
        wait(lambda:any(r['request']==mac+' pause 1' for r in proxy.records),3,'speaker pause mutation')
        resume_at=time.monotonic()
        with sonos.lock:sonos.transport_state='PLAYING'
        wait(lambda:cli('mode ?',mac)[-1]=='play',3,'external resume in LMS')
        wait(lambda:any(r['request']==mac+' pause 0' for r in proxy.records),3,'speaker resume mutation')
        time.sleep(.3)
        if any(r.get('command')=='q' and r['when']>=resume_at for r in tap.records):
            wait(lambda:'resume echo adopted' in (base/'bridge.log').read_text(),3,'cached resume echo adoption')
        assert len(sonos.actions('Play'))==resume_plays and len(sonos.actions('AddURIToQueue'))==resume_adds,'external resume restarted renderer'
        identity(0 if controls_only else 3)
        with sonos.lock:
            sonos.transport_state='STOPPED';sonos.position.update(TrackURI='',RelTime='00:00:00')
        wait(lambda:cli('mode ?',mac)[-1]=='stop' and not items(),3,'external stop/release in LMS')
        wait(lambda:any(r['request']==mac+' stop' for r in proxy.records),3,'speaker stop mutation')
        print('PASS real LMS speaker actions: pause 1 / pause 0 / stop once, each verified within 3 s, owned release, no echo loop',flush=True)
        cli('stop',mac)
        for position,rate in enumerate((44100,48000)):
            cli('playlistcontrol cmd:load track_id:'+alac_ids[position],mac)
            wait(lambda:any(i['metadata'].get('title','').split(' - ')[-1]==str(4+position)
                            and i['resource'].get('sampleFrequency')==str(rate) for i in items()),15,'ALAC prepared')
            assert sum(r['op']=='HELO' for r in tap.records)==1,tap.records
            identity(0)
            wait(lambda:packets('STMd') and packets('STMd')[-1]['when']>=packets('STMs')[-1]['when'],10,'ALAC fully produced')
            cli('stop',mac)
        print('PASS real LMS ALAC sources: generated MP4 at 44.1/48 kHz, finite queue starts, one connection HELO',flush=True)
        for source_rate,target_rate in ((88200,44100),(96000,48000)):
            track=next(t for t in tracks if t.get('url','').endswith(f'hires-{source_rate}.flac'))
            starts=len(packets('STMs'));cli('playlistcontrol cmd:load track_id:'+track['id'],mac)
            wait(lambda:len(packets('STMs'))>starts,20,'high-rate resampled start')
            resource=items()[0]['resource']
            assert resource['sampleFrequency']==str(target_rate) and resource['bitsPerSample']=='24',resource
            assert f'decimation source={source_rate} target={target_rate}' in (base/'bridge.log').read_text()
            print(f'PASS real LMS core decimation: native {source_rate} Hz -> {target_rate} Hz, 24-bit, PLAYING',flush=True)
            cli('stop',mac)
    for (kind,rate),source_pcm in native_sources.items():
        selected=[t for t in tracks if f'native-{kind}-{rate}-' in t['url']]
        assert len(selected)==4,selected
        cli('stop',mac)
        adds_before=len(sonos.actions('AddURIToQueue'));issued=time.monotonic()
        cli('playlistcontrol cmd:load track_id:'+','.join(t['id'] for t in selected),mac)
        start_count=len(packets('STMs'))
        wait(lambda:len(sonos.actions('AddURIToQueue'))>adds_before and items() and items()[0]['metadata']['album']==(f'Native {kind} {rate}' if kind=='flac' else 'No Album'),3,'native first exact publication')
        first_add=sonos.actions('AddURIToQueue')[-1][3]
        request=next(r for r in tap.records if r.get('command')=='s' and r['when']>=issued)
        assert first_add-request['when']<=3,(kind,rate,'slow publication')
        early_disc=[r for r in tap.records if r['op']=='DSCO' and request['when']<r['when']<first_add]
        assert not early_disc,(kind,rate,'published only after DSCO')
        collected=[]
        for index in range(4):
            wait(lambda:len(items())>index,10,'native prepared successor')
            if index:select_item(index)
            wait(lambda:int(cli('playlist index ?',mac)[-1])==index,3,'native LMS identity')
            resource=items()[index]['resource']
            target=rate if rate<=48000 else 44100 if rate==88200 else 48000
            assert int(resource['sampleFrequency'])==target and resource['bitsPerSample']=='24',resource
            import http.client
            from urllib.parse import urlsplit
            u=urlsplit(items()[index]['uri']);c=http.client.HTTPConnection(u.netloc,timeout=8)
            c.request('GET',u.path+'?'+u.query);response=c.getresponse();body=response.read();c.close()
            expected_frames=rate*4//(rate//target)
            assert len(body)==44+expected_frames*6 and int(resource['size'])==len(body),(len(body),resource)
            collected.append(body[44:])
        raw=(source_pcm.astype(np.int64)*256).astype('<i4');raw.tofile(base/'native-reference.raw')
        if rate>48000:
            result=subprocess.check_output([str(ROOT/'third_party/yeney-core/decimator-fixture'),str(rate),str(base/'native-reference.raw'),str(base/'native-reference-out.raw')],text=True)
            expected=np.fromfile(base/'native-reference-out.raw',dtype='<i4').reshape(-1,2).astype(np.int64)
            expected=np.clip(np.where(expected>=0,(expected+128)//256,-((-expected+128)//256)),-2**23,2**23-1).astype('<i4')
        else:expected=source_pcm
        assert b''.join(collected)==raw24(expected),(kind,rate,'PCM discontinuity at join')
        if kind=='lengthalac':
            assert not [r for r in tap.records if r.get('event')=='STMn' and r['when']>=issued]
            print(f'PASS real LMS M4A length album {rate} -> {target}: elst, iTunSMPB original/priming/padding, short/zero final packet; exact WAV bytes, three continuous joins, matching index, no STMn',flush=True)
            cli('stop',mac)
            continue
        before=len(packets('STMs'));seek_adds=len(sonos.actions('AddURIToQueue'));seek_start=time.monotonic();cli('time 2',mac)
        wait(lambda:len(packets('STMs'))>before,3,'native hi-res seek restart')
        seek_add=sonos.actions('AddURIToQueue')[-1][3]
        seek_request=next(r for r in tap.records if r.get('command')=='s' and r['when']>=seek_start)
        # Short seek payloads may finish during the SOAP takeover; exact
        # source count is available before the boundary, independently of EOF.
        if kind=='flac':
            assert 'exact remaining=' in (base/'bridge.log').read_text().split('queue command strm s')[-1]
        assert 1.8 <= (int(items()[0]['resource']['size'])-44)/6/target <= 2.2,items()[0]['resource']
        print(f'PASS real LMS native {kind} album {rate} -> {target}: first publication {first_add-request["when"]:.3f} s before DSCO, exact WAV sizes, three PCM-continuous joins, matching LMS index; seek {time.monotonic()-seek_start:.3f} s',flush=True)
        cli('stop',mac)
    unknown_track=next(t for t in tracks if t['url'].endswith('unknown-96000.flac'))
    cli('stop',mac);before_add=len(sonos.actions('AddURIToQueue'));issued=time.monotonic()
    cli('playlistcontrol cmd:load track_id:'+unknown_track['id'],mac)
    wait(lambda:len(sonos.actions('AddURIToQueue'))>before_add,15,'unknown-count sealed item')
    when=sonos.actions('AddURIToQueue')[-1][3]
    assert any(r['op']=='DSCO' and issued<r['when']<when for r in tap.records),'unknown count published before EOF'
    cli('stop',mac)
    print('PASS real LMS unknown STREAMINFO count: sealed publication remains after DSCO, exact produced length',flush=True)
    assert 'Stream pacing:' not in (base/'bridge.log').read_text(),'relay emitted stream pacing diagnostics'
    print('PASS real LMS local relay: no stream-pacing lines during album and controls',flush=True)
    def playback():
        return json.loads((base/('stream-pacing-'+b'Study'.hex()+'.json')).read_text())['playback']
    remote_path=base/'radio.flac';remote_path.write_bytes((base/'library/0.flac').read_bytes())
    subprocess.run(['metaflac','--add-padding=65536',str(remote_path)],check=True)
    remote=Remote(remote_path.read_bytes())
    mp3=Remote((ROOT/'third_party/yeney-core/tests/fixtures/gapless-0.mp3').read_bytes(),kind='mp3')
    endless=Remote((ROOT/'third_party/yeney-core/tests/fixtures/gapless-0.mp3').read_bytes(),kind='mp3',endless=True)
    remote96=Remote((base/'library/hires-96000.flac').read_bytes())
    extra_remotes.extend([mp3,endless,remote96])
    def stage(urls):
        cli('stop',mac);cli('playlist clear',mac)
        for url in urls:cli('playlist add '+url,mac)
        wait(lambda:int(cli('playlist tracks ?',mac)[-1])==len(urls),15,'staged source playlist')
    def remote_start(source,label):
        stage([source.url])
        fields=cli('status 0 1 tags:u',mac)
        additions=len(sonos.actions('AddURIToQueue'));before=len(packets('STMs'))
        sonos.stream_audio=b''
        started=time.monotonic();cli('play',mac)
        wait(lambda:playback().get('path')=='stream' and sonos.transport_state=='PLAYING'
             and len(getattr(sonos,'stream_audio',b''))>128,3,label+' immediate continuous audio')
        delay=time.monotonic()-started
        fields=cli('status 0 1 tags:u',mac)
        assert 'remote:1' in fields,fields
        wait(lambda:len(packets('STMs'))>before,3,label+' STMs')
        identity(0)
        assert len(sonos.actions('AddURIToQueue'))==additions,'remote touched Sonos queue'
        assert playback()['reason']=='remote-source' and not playback()['owned'],playback()
        print(f'PASS real LMS remote {label}: remote:1, stream path, audio start {delay:.3f} s (<3 s), title/artwork path, no queue mutation',flush=True)
    remote_start(remote,'HTTP FLAC')
    remote_start(remote96,'HTTP 96000 Hz FLAC')
    header=getattr(sonos,'stream_audio',b'')
    assert header[:4]==b'fLaC' and int.from_bytes(header[18:26],'big')>>44==48000,header[:40]
    print('PASS real LMS remote native 96000 -> stream 48000 Hz FLAC, 24-bit, progressive start unchanged',flush=True)
    remote_start(mp3,'HTTP MP3')
    remote_start(endless,'endless HTTP MP3')
    time.sleep(4)
    assert sonos.transport_state=='PLAYING' and playback()['path']=='stream'
    cli('stop',mac)
    stage([next(t['url'] for t in tracks if t['id']==ids[0]),remote.url,
           next(t['url'] for t in tracks if t['id']==ids[1])])
    mixed_log_start=len((base/'bridge.log').read_text())
    before=len(packets('STMs'));cli('play',mac)
    wait(lambda:playback().get('path')=='relay' and len(packets('STMs'))>before,10,'mixed initial local relay')
    identity(0)
    # The strict speaker supplies the natural final-item boundary. No URI
    # switch or Stop may precede this evidence, even though next strm s arrived.
    wait(lambda:'path=relay reason=local-source' in (base/'bridge.log').read_text())
    wait(lambda:'path-pending path=stream waiting=audible-end' in (base/'bridge.log').read_text()[mixed_log_start:],10,'remote successor held until boundary')
    assert playback()['path']=='relay' and sonos.uri.startswith('x-rincon-queue:'),playback()
    with sonos.lock:
        sonos.position['RelTime']='00:00:24';sonos.transport_state='STOPPED'
    wait(lambda:playback()['path']=='stream' and sonos.uri.startswith('http://'),5,'local -> remote boundary')
    identity(1)
    assert sonos.stream_metadata['title']==remote.url,sonos.stream_metadata
    wait(lambda:playback()['path']=='relay' and len(items())>=1 and sonos.transport_state=='PLAYING',35,'remote -> local drained boundary')
    identity(2)
    wait(lambda:(base/'bridge.log').read_text()[mixed_log_start:].count('path-handoff path=')>=2,3,'both measured handoff gaps')
    text=(base/'bridge.log').read_text()
    assert 'path-handoff path=stream gap_ms=' in text and 'path-handoff path=relay gap_ms=' in text
    assert sum(r['op']=='HELO' for r in tap.records)==1
    mutations=[r['request'] for r in proxy.records if not (' status ' in r['request'] or r['request'].endswith(' ?'))]
    assert mutations==([] if hires_only else [mac+' pause 1',mac+' pause 0',mac+' stop']),mutations
    gaps=re.findall(r'path-handoff path=(relay|stream) gap_ms=(\d+)',text)
    print('PASS real LMS source boundaries: local -> remote -> local, preserved prior path until end, matching indexes 0/1/2, measured gaps '+repr(gaps)+', no CLI mutation or extra HELO',flush=True)

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
        if not volume_only:cleanup(ownership_restored)
        cleanup(sonos.close)
    if remote:cleanup(remote.close)
    for source in extra_remotes:cleanup(source.close)
    cleanup(lambda:stop(lms));bridge_log.close();lms_log.close()
    (base/'owner-cli.json').write_text(json.dumps(OWNER_CLI,indent=2))
    active_error=sys.exc_info()[1]
    if failures and (active_error is None or isinstance(active_error,SystemExit) and active_error.code in (None,0)):
        raise failures[0]
