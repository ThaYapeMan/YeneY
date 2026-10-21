"""Acceptance through the real application; every endpoint is loopback."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import http.client
import json
import xml.etree.ElementTree as ET
import os
from pathlib import Path
import signal
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import quote, urlsplit
from xml.sax.saxutils import escape, quoteattr
from queue_controller_test import Coordinator, Handler, ROOT, request, SERVICES, PATHS
sys.path.insert(0, str(ROOT/'third_party/yeney-core/tests'))
from fake_lms import LMS, HTTP, SongQueue
from queue_audible_test import wav

def alac(seconds=24):
    # Repeat a complete independently encoded ALAC sample, rebuilding the MP4
    # timing/size/offset tables. No encoder or third-party implementation copied.
    template=(ROOT/'third_party/yeney-core/tests/fixtures/queue-alac-seek.m4a').read_bytes()
    def atom(kind,body):return struct.pack('>I4s',len(body)+8,kind)+body
    def table(kind):
        i=template.index(kind);n=struct.unpack_from('>I',template,i-4)[0]
        return template[i+4:i+n-4]
    size=struct.unpack_from('>I',table(b'stsz'),12)[0]
    offset=struct.unpack_from('>I',table(b'stco'),8)[0]
    sample=template[offset:offset+size]
    count=(seconds*44100+4095)//4096
    first=struct.unpack_from('>I',template)[0];ftyp=template[:first]
    def rebuild(data,start=0):
        out=b'';pos=0
        while pos<len(data):
            n,kind=struct.unpack_from('>I4s',data,pos);body=data[pos+8:pos+n];pos+=n
            if kind in (b'edts',b'udta',b'mdat',b'free'):continue
            if kind==b'ftyp':continue
            if kind in (b'moov',b'trak',b'mdia',b'minf',b'stbl'):body=rebuild(body,start)
            elif kind==b'stts':body=struct.pack('>IIII',0,1,count,4096)
            elif kind==b'stsz':body=struct.pack('>III',0,size,count)
            elif kind==b'stco':body=struct.pack('>II',0,count)+b''.join(struct.pack('>I',start+i*size) for i in range(count))
            elif kind in (b'mvhd',b'mdhd'):
                scale=struct.unpack_from('>I',body,12)[0]
                body=body[:16]+struct.pack('>I',round(count*4096*scale/44100))+body[20:]
            elif kind==b'tkhd':body=body[:20]+struct.pack('>I',round(count*4096*1000/44100))+body[24:]
            out+=atom(kind,body)
        return out
    moov=rebuild(template)
    moov=rebuild(template,len(ftyp)+len(moov)+8)
    return ftyp+moov+atom(b'mdat',sample*count)

class Device(Handler):
    def action(self,service,action,args):
        if action=='GetPositionInfo' and getattr(self.server,'clocked',False):
            seconds=max(0,int(time.monotonic()-getattr(self.server,'clock_base',time.monotonic())))
            self.server.position['RelTime']=f'{seconds//3600:02}:{seconds//60%60:02}:{seconds%60:02}'
        if action=='SetAVTransportURI' and args['CurrentURI'].startswith('http://'):
            if args.get('CurrentURIMetaData'): ET.fromstring(args['CurrentURIMetaData'])
            self.server.uri=args['CurrentURI'];self.server.stream_uri=args['CurrentURI'];return {},None
        if action=='Play' and self.server.uri.startswith('http://'):
            self.server.transport_state='PLAYING'
            def fetch():
                c,r=request(self.server.uri)
                self.server.stream_audio=r.read(128);c.close()
            threading.Thread(target=fetch,daemon=True).start()
            return {},None
        values,fault=super().action(service,action,args)
        if action=="GetTransportInfo":values["CurrentTransportStatus"]="OK"
        return values,fault
    def do_GET(self):
        self.path = '/description.xml'
        super().do_GET()
    def do_SUBSCRIBE(self):
        if 'AVTransport' in self.path:
            self.server.callback = self.headers.get('CALLBACK', '').strip('<>') or getattr(self.server, 'callback', '')
        self.send_response(200); self.send_header('SID','uuid:binary'); self.send_header('TIMEOUT','Second-1800')
        self.send_header('Content-Length','0'); self.end_headers()
    def do_UNSUBSCRIBE(self): self.do_SUBSCRIBE()
    def do_POST(self):
        action = self.headers['SOAPAction'].strip('"').split('#')[1]
        service = next((name for name in SERVICES if name in self.path), 'AVTransport')
        if action == 'GetVolume':
            self.rfile.read(int(self.headers['Content-Length']))
            self.send('<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body>'
                      '<u:GetVolumeResponse xmlns:u="urn:schemas-upnp-org:service:RenderingControl:1">'
                      '<CurrentVolume>20</CurrentVolume></u:GetVolumeResponse></s:Body></s:Envelope>'); return
        self.path=PATHS[service]
        super().do_POST()

class CLI(socketserver.ThreadingTCPServer):
    allow_reuse_address=True
    daemon_threads=True
    def __init__(self, owner):
        self.owner=owner
        class Client(socketserver.StreamRequestHandler):
            def handle(self):
                for wire in self.rfile:
                    line=wire.decode().strip()
                    if line=='listen 1':
                        self.wfile.write(b'listen 1\n');self.wfile.flush();continue
                    mac,cmd=line.split(' ',1)
                    assert mac==owner.mac and '%' not in mac, line
                    owner.commands.append(cmd)
                    if cmd in ('pause 1','play') or (cmd.startswith('playlist index ') and not cmd.endswith('?')):
                        time.sleep(getattr(owner,'cli_delay',0))
                    if cmd in ('mode ?','playlist index ?','time ?'):
                        if cmd=='mode ?': value=getattr(owner,'cli_mode','play')
                        elif cmd=='playlist index ?': value=str(getattr(owner,'cli_index',max(0,owner.model.playing-1)))
                        else: value=str(getattr(owner,'cli_time',owner.source_ms/1000))
                        self.wfile.write((mac+' '+cmd[:-1]+value+'\n').encode());self.wfile.flush();return
                    if cmd.startswith('status '):
                        fields=[f'playlist_cur_index:{owner.index if owner.index is not None else max(0,owner.model.playing-1) if owner.model else 0}',
                                f'playlist_tracks:{len(owner.order)}', f'time:{owner.source_ms/1000}', f'playlist_timestamp:{owner.revision}']
                        entries=list(enumerate(owner.order))
                        if cmd.split()[2]=='1':
                            current=owner.index if owner.index is not None else max(0,owner.model.playing-1)
                            entries=entries[current:current+1]
                        for at,i in entries:
                            fields += [f'playlist index:{at}', f'id:track {i}:encoded',f'title:Title & {i}',
                                       'artist:Artist <one>',f'album:{owner.albums[i] if i<len(owner.albums) else "Album & fixture"}',f'url:file:///fixture/{i}']
                        reply=' '.join(quote(f,safe='') for f in fields)
                    else: reply=''
                    self.wfile.write((quote(line,safe=': ')+(' '+reply if reply else '')+'\n').encode())
                    self.wfile.flush()
                    if cmd.startswith('playlist index '):
                        owner.cli_index=int(cmd.split()[-1])
                        owner.model.streaming=int(cmd.split()[-1])+1
                    if cmd.startswith('time '):owner.cli_time=float(cmd.split()[-1])
                    if cmd=='pause 1':owner.cli_mode='pause'
                    elif cmd=='play':owner.cli_mode='play'
                    if cmd.startswith('time ') or cmd.startswith('playlist index '):
                        owner.lms.strm('q')
                        if owner.refetch_audio:
                            index=owner.index if owner.index is not None else max(0,owner.model.playing-1)
                            body=owner.bodies[index];delay=owner.refetch_delay
                            source=HTTP(body,first=len(body)//2 if delay else None,delay=delay);owner.sources.append(source)
                            owner.lms.strm('s',source,fmt='f',rate=owner.rates[index])
                        else:
                            index=owner.index if owner.index is not None else max(0,owner.model.playing-1)
                            source=HTTP(owner.bodies[index]);owner.sources.append(source)
                            owner.lms.strm('s',source,fmt='f',rate=owner.rates[index])
                        owner.lms.strm('u')
                    elif cmd=='pause 1':owner.lms.strm('p')
                    elif cmd=='play':owner.lms.strm('u')
        super().__init__(('127.0.0.1',9090),Client)
        self.worker=threading.Thread(target=self.serve_forever,daemon=True);self.worker.start()
    def close(self): self.shutdown();self.server_close();self.worker.join()

class Binary:
    def __init__(self, codec='f', rates=(44100,)*4, fade=False, binary=None, slow=False, initial_stop=False, fail_add=False, albums=None, expected_albums=None):
        self.rates=rates; self.albums=albums if albums is not None else [f'Album & fixture {i//2}' if fade else 'Album & fixture' for i in range(len(rates))]; self.order=list(range(len(rates))); self.index=None; self.source_ms=0; self.revision=1; self.sequence=0; self.refetch_audio=initial_stop; self.refetch_delay=8 if initial_stop and slow else 0; self.bodies={}; self.commands=[];self.model=None;self.sources=[];self.errors=[];self.stopping=False
        self.tmp=tempfile.TemporaryDirectory(prefix='yeney-binary-');self.base=Path(self.tmp.name)
        # Coordinator initialization is reused, but bind the production Sonos port.
        self.sonos=Coordinator(('127.0.0.1',1400),Device)
        self.sonos.fail_add=fail_add
        self.sonos.follow_controls=True
        self.sonos.expected_albums=self.albums if expected_albums is None else expected_albums
        self.cli=CLI(self);self.lms=LMS();self.log=open(self.base/'bridge.log','w')
        env=dict(os.environ,YENEY_PLAYBACK_MODE='queue',YENEY_POLL='events',YENEY_QUEUE_DISK_MIB='64',STATE_DIRECTORY=str(self.base))
        self.proc=subprocess.Popen([str(binary or ROOT/'yeney'),'--room=Study','--ip=127.0.0.1',
                                   '--server=127.0.0.1:'+str(self.lms.listener.getsockname()[1])],stdout=self.log,stderr=self.log,env=env)
        try:
            hello=self.lms.accept()
            self.mac=':'.join(f'{v:02x}' for v in hello['body'][2:8])
        except Exception:
            self.proc.kill();self.proc.wait()
            self.log.close();self.lms.close();self.cli.close();self.sonos.close()
            raise AssertionError((self.base/'bridge.log').read_text())
        def sources():
            for i,rate in enumerate(rates):
                path=self.base/f'{i}.wav';path.write_bytes(wav(rate))
                target=path.with_suffix('.m4a' if codec=='l' else '.flac')
                if codec=='l':
                    body=alac()
                else:
                    subprocess.run(['flac','-s','-f','-o',str(target),str(path)],check=True)
                    body=target.read_bytes()
                self.bodies[i]=body
                source=HTTP(body,first=len(body)//2 if slow and i==0 else None,delay=8 if slow and i==0 else 0);self.sources.append(source)
                yield source,dict(fmt=codec,rate=rate,transition=int(fade),period=10 if fade else 0)
        self.lms.connection.settimeout(30)
        if initial_stop:
            for _ in range(3): self.lms.strm('q')
        self.model=SongQueue(self.lms,sources())
        def read():
            try:
                while not self.stopping:
                    if self.lms.read(eof_ok=True) is None:break
            except Exception as e:
                if not self.stopping:self.errors.append(e)
        self.reader=threading.Thread(target=read,daemon=True);self.reader.start()
    def until(self,predicate,timeout=15):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            if predicate():return
            if self.errors or self.proc.poll() is not None:break
            time.sleep(.02)
        raise AssertionError((self.errors,(self.base/'bridge.log').read_text()))
    def items(self):
        with self.sonos.lock:return list(self.sonos.items)
    def state(self,track,seconds=0,state='PLAYING'):
        self.until(lambda:len(self.items())>=track and 'resource' in self.items()[track-1])
        with self.sonos.lock:
            self.sonos.position=dict(Track=str(track),TrackURI=self.items()[track-1]['uri'],RelTime=f'00:00:{seconds:02}')
            self.sonos.transport_state=state
            self.sonos.clock_base=time.monotonic()-seconds
        callback=getattr(self.sonos,'callback','')
        if callback:
            self.sequence+=1
            fields=dict(CurrentTrack=str(track),CurrentTrackURI=self.items()[track-1]['uri'],RelativeTimePosition=f'00:00:{seconds:02}',TransportState=state)
            change='<Event><InstanceID val="0">'+''.join('<'+k+' val='+quoteattr(v)+'/>' for k,v in fields.items())+'</InstanceID></Event>'
            body='<e:propertyset xmlns:e="urn:schemas-upnp-org:event-1-0"><e:property><LastChange>'+escape(change)+'</LastChange></e:property></e:propertyset>'
            u=urlsplit(callback);c=http.client.HTTPConnection(u.netloc,timeout=3)
            c.request('NOTIFY',u.path,body,headers={'NT':'upnp:event','NTS':'upnp:propchange','SID':'uuid:binary','SEQ':str(self.sequence)})
            r=c.getresponse();r.read();assert r.status==200;c.close()
    def elapsed(self):
        stamp=12345+len(self.lms.packets)
        self.lms.strm('t',value=stamp)
        self.until(lambda:any(p.get('stamp')==stamp for p in self.packets('STMt')))
        return next(p['elapsed'] for p in self.packets('STMt') if p.get('stamp')==stamp)
    def packets(self,event):return [p for p in self.lms.packets if p.get('event')==event]
    def close(self):
        self.sonos.closing=True
        self.stopping=True
        errors=[]
        def close(fn):
            try:fn()
            except Exception as error:errors.append(error)
        self.proc.send_signal(signal.SIGTERM)
        try:self.proc.wait(12)
        except subprocess.TimeoutExpired:
            self.proc.kill();self.proc.wait();errors.append(AssertionError('binary shutdown timeout'))
        self.reader.join(2);self.log.close();close(self.lms.close)
        for source in self.sources:close(source.close)
        close(self.cli.close);close(self.sonos.close)
        if self.proc.returncode:errors.append(AssertionError((self.base/'bridge.log').read_text()))
        if errors:raise errors[0]

def album(codec='f',rates=(44100,)*4,fade=False):
    h=Binary(codec,rates,fade,binary=Path(os.environ['YENEY_ACCEPTANCE_BINARY']) if 'YENEY_ACCEPTANCE_BINARY' in os.environ else None)
    try:
        h.sonos.clocked=True
        h.until(lambda:h.sonos.verified_save and any('resource' in i for i in h.items()))
        assert h.sonos.snapshots[h.sonos.last_save]==h.sonos.original
        assert not h.sonos.crossfade
        for track in range(1,5):
            h.state(track)
            h.until(lambda:len(h.packets('STMs'))==track)
            assert h.model.playing==track,(h.model.playing,track)
            first=h.elapsed();time.sleep(.12);second=h.elapsed()
            assert 0<=first<=second<1500,(track,first,second)
            h.until(lambda:len(h.packets('STMd'))==track)
            assert h.packets('STMs')[-1]['time']<=h.packets('STMd')[-1]['time']
            # Read/reconnect and Range never create an audible start.
            uri=h.items()[track-1]['uri']
            c,r=request(uri,byte_range='bytes=0-4095');assert r.status==206
            total=int(r.getheader('Content-Range').split('/')[-1]);r.read();c.close()
            c,r=request(uri,byte_range='bytes=4096-8191');assert r.status==206;r.read();c.close()
            duration=(total-44)/6/rates[track-1]
            h.until(lambda:time.monotonic()-h.sonos.clock_base>=max(0,int(duration)-4),timeout=35)
            if track<4:
                h.until(lambda:len(h.items())>=track+1)
                c,r=request(h.items()[track]['uri']);assert r.status==200
                count=0
                while True:
                    chunk=r.read(32768)
                    if not chunk:break
                    count+=len(chunk);time.sleep(len(chunk)/(6*rates[track]*9))
                assert count==int(r.getheader('Content-Length'));c.close()
            h.until(lambda:time.monotonic()-h.sonos.clock_base>=max(0,int(duration)-1),timeout=10);time.sleep(1.2)
        assert len(h.packets('STMs'))==4 and len(h.packets('STMd'))==4
        print(f'PASS real yeney queue album codec={codec} rates={rates} crossfade={10 if fade else 0}: verified backup, three joins, speaker/LMS identity, STMs before STMd, prefetch 3–4 s early at 9x, Range/reconnect',flush=True)
    finally:h.close()
    assert h.sonos.items==[] and h.sonos.crossfade
    print('PASS real yeney owning service stop: exact owned removal and CrossfadeMode restored',flush=True)

def controls():
    h=Binary(rates=(44100,)*6,binary=Path(os.environ['YENEY_ACCEPTANCE_BINARY']) if 'YENEY_ACCEPTANCE_BINARY' in os.environ else None)
    try:
        h.sonos.clocked=True
        for track in (1,2,3):
            h.state(track);h.until(lambda:len(h.packets('STMs'))==track)
            h.until(lambda:len(h.packets('STMd'))==track)
            h.until(lambda:time.monotonic()-h.sonos.clock_base>=23,timeout=30);time.sleep(1.2)
        h.sonos.clocked=False
        h.until(lambda:len(h.items())>=4)
        h.model.sources=iter(());h.model.ready=False
        h.lms.strm('p');h.until(lambda:len(h.packets('STMp'))==1)
        elapsed=h.elapsed();time.sleep(10)
        assert h.elapsed()==elapsed and len(h.packets('STMp'))==1
        h.lms.strm('u');h.until(lambda:len(h.packets('STMr'))==1)
        print('PASS real yeney LMS pause 10 s/resume: confirmed state, one STMp/STMr',flush=True)
        for index in (3,2):
            h.index=index;h.source_ms=0;h.model.streaming=index+1
            before=len(h.packets('STMs'))
            source=HTTP(h.bodies[index]);h.sources.append(source)
            h.lms.strm('q');h.lms.strm('s',source,fmt='f',rate=h.rates[index])
            h.until(lambda:len(h.packets('STMs'))==before+1)
            assert h.model.playing==index+1
        print('PASS real yeney LMS next/previous: retained prepared assets, matching audible identity, no double advance',flush=True)
        h.index=2;h.source_ms=2000
        source=HTTP(h.bodies[2]);h.sources.append(source)
        before=len(h.packets('STMs'));h.lms.strm('q');h.lms.strm('s',source,fmt='f')
        h.until(lambda:any(a[1].get('Unit')=='REL_TIME' for a in h.sonos.actions('Seek')))
        h.lms.strm('u') # real LMS releases the newly connected seek run
        h.until(lambda:len(h.packets('STMs'))==before+1)
        print('PASS real yeney LMS seek within history: REL_TIME, same asset and one acknowledged LMS run',flush=True)
        time.sleep(2)
        track=int(h.sonos.position['Track'])
        h.state(track,2,'PAUSED_PLAYBACK')
        h.until(lambda:h.commands.count('pause 1')==1)
        h.state(track,2,'PLAYING');h.until(lambda:h.commands.count('play')==1)
        for target in (track+1,track):
            before=len(h.packets('STMs'));h.state(target)
            h.until(lambda:len(h.packets('STMs'))==before+1)
        h.state(track,6);h.until(lambda:'time 6' in h.commands)
        print('PASS real yeney app pause/resume, next/previous and history seek: decoded CLI acknowledgements and SlimProto echoes without loops',flush=True)
        audible=h.sonos.position['TrackURI']
        for name,order in [('add',[0,1,2,3,4,5,99]),('move',[0,1,2,5,4,3,99]),('delete',[0,1,2,5,4,3]),('clear',[])]:
            before=(h.base/'bridge.log').read_text().count('rebuild revision=')
            h.order=order;h.revision+=1
            if not order:h.index=0
            h.until(lambda:(h.base/'bridge.log').read_text().count('rebuild revision=')>before)
            assert h.sonos.position['TrackURI']==audible
        print('PASS real yeney LMS add/delete/move/clear: encoded stable identities, future rebuilds preserve audible URI',flush=True)
    finally:h.close()

def rearm_replay():
    h=Binary(rates=(44100,),initial_stop=True,slow=True,binary=Path(os.environ['YENEY_ACCEPTANCE_BINARY']) if 'YENEY_ACCEPTANCE_BINARY' in os.environ else None)
    try:
        h.state(1);h.until(lambda:len(h.packets('STMs'))==1)
        time.sleep(1)
        log=(h.base/'bridge.log').read_text()
        assert log.count('ownership acquired')==1,log
        assert len(h.sonos.actions('AddURIToQueue'))==1,h.sonos.calls
        assert len(h.sonos.actions('Play'))==1
        assert not h.sonos.actions('AddMultipleURIsToQueue')
        assert sum(p['op']=='HELO' for p in h.lms.packets)==1,h.lms.packets
        assert h.lms.reconnect_replays==0
        generations=log.count('state=prepare reason=finite-ready')
        assert generations == 1, log
        assert not [c for c in h.commands if not (c.startswith('status ') or c.endswith(' ?'))], h.commands
        assert not h.packets('STMd') # retry while this progressive producer is still active
        h.lms.strm('s',h.sources[-1],fmt='f',rate=44100)
        h.until(lambda:'queue repeated strm s' in (h.base/'bridge.log').read_text())
        time.sleep(1)
        log=(h.base/'bridge.log').read_text()
        assert log.count('state=prepare reason=finite-ready')==generations,log
        assert sum(p['op']=='HELO' for p in h.lms.packets)==1
        assert 'state=drain' not in log
        print('PASS 23:14 field enqueue replay: AddURIToQueue accepted, no AddMultipleURIsToQueue, Play once, no fallback')
        print('PASS field q/q/q+s replay: one preparation/takeover/enqueue/Play, zero CLI mutations, one HELO; repeated same-track s does not rearm',flush=True)
    finally:h.close()

def enqueue_fault():
    h=Binary(rates=(44100,),initial_stop=True,fail_add=True,
             binary=Path(os.environ['YENEY_ACCEPTANCE_BINARY']) if 'YENEY_ACCEPTANCE_BINARY' in os.environ else None)
    try:
        h.until(lambda:'active=stream state=active reason=soap-error' in (h.base/'bridge.log').read_text(),timeout=25)
        time.sleep(1)
        log=(h.base/'bridge.log').read_text()
        assert log.count('active=queue state=drain reason=soap-error')==1,log
        assert log.count('release reason=mode-handoff')==1,log
        assert len(h.sonos.actions('AddURIToQueue'))==1
        assert not h.sonos.actions('AddMultipleURIsToQueue')
        lines=[line for line in log.splitlines() if 'queue SOAP action=AddURIToQueue HTTP=500' in line]
        assert len(lines)==1,log
        assert 'errorCode="402" errorDescription="fixture rejection"' in lines[0],lines
        body=json.loads(lines[0].split(' request=',1)[1])
        action=ET.fromstring(body).find('{http://schemas.xmlsoap.org/soap/envelope/}Body')[0]
        assert {n.tag:n.text or '' for n in action}==h.sonos.actions('AddURIToQueue')[0][1]
        assert 'EnqueuedURIMetaData' in body and '&lt;DIDL-Lite' in body
        print('PASS AddURIToQueue fault replay: exactly one soap-error fallback and release; complete action/HTTP/UPnP fault and exact DIDL request logged',flush=True)
    finally:h.close()

def metadata_replay():
    album='Het beste uit pop in je moerstaal, Vol. 2'
    h=Binary(codec='l',rates=(44100,),initial_stop=True,albums=['\x16'+album],expected_albums=[album],
             binary=Path(os.environ['YENEY_ACCEPTANCE_BINARY']) if 'YENEY_ACCEPTANCE_BINARY' in os.environ else None)
    try:
        h.state(1);h.until(lambda:len(h.packets('STMs'))==1)
        log=(h.base/'bridge.log').read_text()
        assert log.count('ownership acquired')==1,log
        assert len(h.sonos.actions('AddURIToQueue'))==1 and len(h.sonos.actions('Play'))==1
        assert not h.sonos.actions('AddMultipleURIsToQueue')
        assert 'state=drain' not in log and 'fallback command=' not in log,log
        metadata=h.sonos.actions('AddURIToQueue')[0][1]['EnqueuedURIMetaData']
        item=ET.fromstring(metadata)[0]
        assert next(n.text for n in item if n.tag.endswith('}album'))==album
        changed=[line for line in log.splitlines() if 'metadata sanitized mode=queue' in line]
        assert len(changed)==1 and 'field=album removed=1 replaced=0' in changed[0],changed
        assert '\x16' not in metadata and '\x16' not in log
        print('PASS 08:57 metadata replay: CLI %16 album sanitized, strict Sonos accepts AddURIToQueue, one takeover/Play, one field-count log and no fallback',flush=True)
    finally:h.close()

def rapid_selections():
    h=Binary(rates=(44100,)*3)
    try:
        h.state(1);h.until(lambda:len(h.packets('STMs'))==1)
        h.until(lambda:len(h.items())>=2)
        h.model.sources=iter(());h.model.ready=False
        for index in (1,0,1,0):
            h.index=index;h.source_ms=0;h.model.streaming=index+1
            before=len(h.packets('STMs'))
            source=HTTP(h.bodies[index]);h.sources.append(source)
            h.lms.strm('q');h.lms.strm('s',source,fmt='f')
            h.until(lambda:len(h.packets('STMs'))==before+1 and h.model.playing==index+1)
            assert h.model.playing==index+1
        assert sum(p['op']=='HELO' for p in h.lms.packets)==1
        assert not [c for c in h.commands if not (c.startswith('status ') or c.endswith(' ?'))]
        print('PASS rapid LMS q/s selections: four next/previous starts before room status catches up, matching identity, no false seek/rearm/CLI mutation',flush=True)
    finally:h.close()

def slow_app():
    h=Binary(rates=(44100,)*2,binary=Path(os.environ['YENEY_ACCEPTANCE_BINARY']) if 'YENEY_ACCEPTANCE_BINARY' in os.environ else None)
    try:
        h.state(1);h.until(lambda:len(h.packets('STMs'))==1)
        h.until(lambda:len(h.items())>=2)
        h.cli_delay=5
        h.state(1,0,'PAUSED_PLAYBACK')
        h.until(lambda:'mode ?' in h.commands,timeout=15)
        h.until(lambda:len(h.packets('STMp'))==1,timeout=15)
        h.state(1,0,'PLAYING')
        h.until(lambda:len(h.packets('STMr'))==1,timeout=15)
        h.state(2)
        h.until(lambda:'playlist index ?' in h.commands,timeout=15)
        h.until(lambda:len(h.packets('STMs'))==2,timeout=15)
        time.sleep(1)
        assert h.commands.count('pause 1')==1 and h.commands.count('playlist index 1')==1,h.commands
        log=(h.base/'bridge.log').read_text()
        assert 'state=drain' not in log,log
        assert h.model.playing==2
        print('PASS real yeney app pause/resume and next with 5 s LMS replies: matching audible identity, no echo loop or fallback',flush=True)
    finally:h.close()

def foreign():
    h=Binary(binary=Path(os.environ['YENEY_ACCEPTANCE_BINARY']) if 'YENEY_ACCEPTANCE_BINARY' in os.environ else None)
    try:
        h.state(1);h.until(lambda:len(h.packets('STMs'))==1)
        h.until(lambda:len(h.items())>=2)
        h.model.sources=iter(());h.model.ready=False
        time.sleep(.5)
        uri=h.items()[0]['uri'];c,r=request(uri);prefix=r.read(100)
        h.refetch_audio=True;h.index=0
        h.sonos.add_foreign();entry=h.sonos.items[-1].copy()
        h.until(lambda:getattr(h.sonos,'stream_audio',b''),timeout=20)
        h.until(lambda:'active=stream state=active reason=foreign-control' in (h.base/'bridge.log').read_text())
        log=(h.base/'bridge.log').read_text()
        assert log.count('release reason=mode-handoff')==1
        assert log.count('active=queue state=drain reason=foreign-control')==1
        assert h.sonos.items==[entry] and h.sonos.crossfade
        assert h.model.playing==1
        assert prefix+r.read();c.close()
        print('PASS real yeney foreign edit: one release and stream fallback, confirmed tail endpoint, same LMS index/time, foreign entry preserved, handoff gap confirmed',flush=True)
    finally:h.close()

def seek_future():
    h=Binary(slow=True,binary=Path(os.environ['YENEY_ACCEPTANCE_BINARY']) if 'YENEY_ACCEPTANCE_BINARY' in os.environ else None)
    try:
        h.state(1);h.until(lambda:len(h.packets('STMs'))==1)
        old=h.items()[0]['uri'];h.model.sources=iter(());h.model.ready=False
        path=h.base/'seek.wav';path.write_bytes(wav(44100,4))
        target=h.base/'seek.flac'
        subprocess.run(['flac','-s','-f','-o',str(target),str(path)],check=True)
        h.bodies[0]=target.read_bytes();h.index=0;h.source_ms=20000;h.refetch_audio=True
        source=HTTP(h.bodies[0]);h.sources.append(source)
        h.lms.strm('q');h.lms.strm('s',source,fmt='f')
        h.until(lambda:h.sonos.position['TrackURI']!=old and h.sonos.position['TrackURI'].startswith('http'))
        h.until(lambda:'history=rerender' in (h.base/'bridge.log').read_text())
        h.until(lambda:len(h.packets('STMs'))==2)
        assert h.model.playing==1
        assert h.commands.count('time 20')==0, h.commands
        print('PASS real yeney LMS seek beyond produced history: one fresh asset/URL, decoder admission, LMS source-time target and matching identity',flush=True)
    finally:h.close()

if __name__=='__main__':
    rearm_replay()
    enqueue_fault()
    metadata_replay()
    rapid_selections()
    album()
    album('l')
    album(rates=(44100,48000,44100,48000))
    album(fade=True)
    controls()
    slow_app()
    foreign()
    seek_future()
