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
from queue_sonos_fake import Coordinator, Handler, ROOT, request, SERVICES, PATHS
sys.path.insert(0, str(ROOT/'third_party/yeney-core/tests'))
from fake_lms import LMS, HTTP, SongQueue
from relay_audio import wav

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
        if action=="Play" and getattr(self.server,"stopped_play",False):self.server.transport_state="STOPPED"
        if action=="GetTransportInfo":values["CurrentTransportStatus"]=getattr(self.server,"transport_status","OK")
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
                    if not cmd.startswith('status ') and not cmd.endswith('?'):
                        reasons={'stop':('speaker-stop','speaker-source-change'),'pause 1':('speaker-pause',),'pause 0':('speaker-resume',)}
                        assert cmd in reasons,('unapproved LMS mutation',cmd)
                        text=(owner.base/'bridge.log').read_text()
                        assert any('LMS mutation reason='+r+' command='+cmd in text for r in reasons[cmd]),('mutation without speaker reason',cmd)
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
                    elif cmd in ('play','pause 0'):owner.cli_mode='play'
                    elif cmd=='stop':owner.cli_mode='stop'
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
                    elif cmd in ('play','pause 0'):owner.lms.strm('u')
                    elif cmd=='stop':owner.lms.strm('q')
        super().__init__(('127.0.0.1',9090),Client)
        self.worker=threading.Thread(target=self.serve_forever,daemon=True);self.worker.start()
    def close(self): self.shutdown();self.server_close();self.worker.join()

class Binary:
    def __init__(self, codec='f', rates=(44100,)*4, fade=False, binary=None, slow=False, initial_stop=False, fail_add=False, albums=None, expected_albums=None, stopped_play=False, seconds=24):
        self.rates=rates; self.albums=albums if albums is not None else [f'Album & fixture {i//2}' if fade else 'Album & fixture' for i in range(len(rates))]; self.order=list(range(len(rates))); self.index=None; self.source_ms=0; self.revision=1; self.sequence=0; self.refetch_audio=initial_stop; self.refetch_delay=8 if initial_stop and slow else 0; self.bodies={}; self.commands=[];self.model=None;self.sources=[];self.errors=[];self.stopping=False
        self.tmp=tempfile.TemporaryDirectory(prefix='yeney-binary-');self.base=Path(self.tmp.name)
        # Coordinator initialization is reused, but bind the production Sonos port.
        self.sonos=Coordinator(('127.0.0.1',1400),Device)
        self.sonos.stopped_play=stopped_play
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
                path=self.base/f'{i}.wav';path.write_bytes(wav(rate,seconds))
                target=path.with_suffix('.m4a' if codec=='l' else '.flac')
                if codec=='l':
                    body=alac()
                else:
                    subprocess.run(['flac','-s','-f','-o',str(target),str(path)],check=True)
                    body=target.read_bytes()
                self.bodies[i]=body
                source=HTTP(body,first=16 if slow and i==0 else None,delay=8 if slow and i==0 else 0);self.sources.append(source)
                yield source,dict(fmt=codec,rate=rate if rate in (44100,48000) else 44100,transition=int(fade),period=10 if fade else 0)
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

def relay_replay():
    h=Binary(rates=(44100,),initial_stop=True,albums=['\x16Het beste uit pop in je moerstaal, Vol. 2'],expected_albums=['Het beste uit pop in je moerstaal, Vol. 2'])
    try:
        h.state(1);h.until(lambda:len(h.packets('STMs'))==1)
        assert len(h.sonos.actions('AddURIToQueue'))==len(h.sonos.actions('Play'))==1
        assert len([p for p in h.lms.packets if p.get('op')=='HELO'])==1
        assert all(c.startswith('status ') for c in h.commands),h.commands
        log=(h.base/'bridge.log').read_text()
        assert 'metadata sanitized mode=queue' in log,log
        status=json.loads((h.base/('stream-pacing-'+b'Study'.hex()+'.json')).read_text())['playback']
        assert status['state']=='PLAYING' and status['owned'] and status['current_owned_item']==h.sonos.position['TrackURI'],status
        h.lms.strm('p');h.until(lambda:len(h.packets('STMp'))==1)
        h.lms.strm('u');h.until(lambda:len(h.packets('STMr'))==1)
        h.lms.strm('q');h.until(lambda:h.sonos.transport_state=='STOPPED' and not h.items())
        assert all(c.startswith('status ') for c in h.commands),h.commands
        print('PASS relay field replay: q/q/q+s, sanitized album, one enqueue/Play/HELO, synchronous status, pause/resume/stop, no CLI mutation',flush=True)
    finally:h.close()

def preparing_repeat():
    h=Binary(rates=(44100,),slow=True,initial_stop=True)
    try:
        h.until(lambda:len(h.sources)==1)
        h.lms.strm('s',h.sources[0],fmt='f',rate=44100)
        h.state(1);h.until(lambda:len(h.packets('STMs'))==1)
        assert len(h.sonos.actions('AddURIToQueue'))==len(h.sonos.actions('Play'))==1
        assert 'queue repeated strm s' in (h.base/'bridge.log').read_text()
        print('PASS relay repeated preparing start: one producer, enqueue and Play despite identical strm s before publication',flush=True)
    finally:h.close()

def idle_start():
    h=Binary(rates=(44100,))
    try:
        h.state(1);h.until(lambda:len(h.packets('STMs'))==1)
        h.until(lambda:len(h.packets('STMd'))==1)
        h.state(1,24,'STOPPED');h.until(lambda:len(h.packets('STMu'))==1)
        source=HTTP(h.bodies[0]);h.sources.append(source)
        h.model.streaming=1
        h.lms.strm('s',source,fmt='f',rate=44100)
        h.until(lambda:len(h.packets('STMs'))==2)
        assert len(h.sonos.actions('Play'))==2 and len(h.items())==1
        assert all(c.startswith('status ') for c in h.commands),h.commands
        print('PASS relay idle start: final speaker STOPPED sends STMu once; new strm s without q releases old items and starts once',flush=True)
    finally:h.close()

def rate_guard():
    h=Binary(rates=(96000,))
    try:
        h.until(lambda:len(h.packets("STMn"))==1)
        assert not h.sonos.actions("AddURIToQueue")
        assert "unsupported rate" in (h.base/"bridge.log").read_text()
        print("PASS queue above-cap 96000 Hz: STMn once, no AddURIToQueue")
    finally:h.close()

def speaker_faults():
    for status in ('ERROR_UNSUPPORTED_FREQ','OK'):
        h=Binary(rates=(44100,),stopped_play=True)
        try:
            h.until(lambda:len(h.sonos.actions('Play'))==1)
            with h.sonos.lock:
                h.sonos.transport_state='STOPPED'
                h.sonos.transport_status=status
            h.until(lambda:len(h.packets('STMn'))==1,8)
            time.sleep(.7)
            assert len(h.packets('STMn'))==1 and not h.items()
            text=(h.base/'bridge.log').read_text()
            assert 'playback-fault action=Play uri=' in text and 'TransportStatus='+status in text
            assert 'state=STOPPED audible' not in text
            print('PASS speaker playback fault: '+status+', Stop/release/STMn once')
        finally:h.close()

def polling():
    h=Binary(rates=(44100,44100))
    try:
        h.until(lambda:len(h.packets('STMs'))==1)
        h.state(1,10)
        time.sleep(3.6)
        for action in ('GetPositionInfo','GetTransportInfo','GetMediaInfo'):
            before=len(h.sonos.actions(action));time.sleep(2)
            count=len(h.sonos.actions(action))-before
            assert count<=4,(action,count)
        h.state(1,21)
        before=len(h.sonos.actions('GetPositionInfo'));time.sleep(.8)
        assert len(h.sonos.actions('GetPositionInfo'))-before>=4
        h.until(lambda:len(h.items())==2)
        h.state(2,0);h.until(lambda:len(h.packets('STMs'))==2,2)
        text=(h.base/'bridge.log').read_text()
        assert 'SOAP action=GetPositionInfo HTTP=200' not in text
        assert 'SOAP action=GetTransportInfo HTTP=200' not in text
        assert 'SOAP action=GetMediaInfo HTTP=200' not in text
        assert 'queue queue' not in text
        for field in ('TransportState','TransportStatus','CurrentTrack','CurrentTrackURI','NumberOfTracks','AVTransportURI'):
            assert 'field='+field+' old=' in text,field
        assert 'speaker-change source=GENA' in text and 'relay-change state=' in text
        assert text.count('field=TransportStatus old=')==1,text
        print('PASS queue polling: steady <=2/s per action, 100 ms end/join window, timely STMs, no successful-poll logs or duplicate prefix')
    finally:h.close()

def external_actions():
    for kind in ('port-stop','source','pause'):
        h=Binary(rates=(44100,),seconds=274 if kind=='port-stop' else 24)
        try:
            h.state(1)
            h.until(lambda:len(h.packets('STMs'))==1)
            h.state(1,122 if kind=='port-stop' else 5)
            time.sleep(3.4)
            if kind=='pause':
                h.state(1,5,'PAUSED_PLAYBACK')
                h.until(lambda:h.commands.count('pause 1')==1)
                h.state(1,5,'PLAYING')
                h.until(lambda:h.commands.count('pause 0')==1)
                time.sleep(.7)
                assert [c for c in h.commands if not c.startswith('status ')]==['pause 1','pause 0'],h.commands
            else:
                stops=len(h.sonos.actions('Stop'))
                with h.sonos.lock:
                    h.sonos.position.update(TrackURI='',RelTime='00:00:00')
                    h.sonos.transport_state='PLAYING' if kind=='source' else 'STOPPED'
                    if kind=='source':h.sonos.uri='http://new-service.invalid/music'
                h.until(lambda:h.commands.count('stop')==1 and not h.items(),3)
                time.sleep(.7)
                assert h.commands.count('stop')==1 and len(h.sonos.actions('Stop'))==stops,h.commands
                status=json.loads((h.base/('stream-pacing-'+b'Study'.hex()+'.json')).read_text())['playback']
                assert status['state']=='STOPPED' and not status['owned'],status
                if kind=='source':assert h.sonos.uri=='http://new-service.invalid/music' and h.sonos.transport_state=='PLAYING'
            print('PASS speaker-originated '+kind+': one intended CLI action, raw MAC/decoded echo, release or pause/resume, no command loop')
        finally:h.close()

def natural_empty_end():
    h=Binary(rates=(44100,))
    try:
        h.state(1);h.until(lambda:len(h.packets('STMs'))==1)
        h.state(1,23)
        time.sleep(.7) # allow one steady poll to retain the final RelTime
        time.sleep(3.2)
        with h.sonos.lock:
            h.sonos.position.update(TrackURI='',RelTime='00:00:00');h.sonos.transport_state='STOPPED'
        h.until(lambda:len(h.packets('STMu'))==1)
        time.sleep(.7)
        assert all(c.startswith('status ') for c in h.commands),h.commands
        print('PASS natural final end with empty URI: STMu once, no LMS stop mutation')
    finally:h.close()

def soap_fault():
    h=Binary(rates=(44100,),fail_add=True)
    try:
        h.until(lambda:len(h.packets('STMn'))==1)
        time.sleep(.3)
        assert len(h.packets('STMn'))==1 and not h.sonos.actions('Play')
        assert not h.items() and h.sonos.crossfade
        log=(h.base/'bridge.log').read_text()
        assert 'errorCode=' in log and 'request=' in log,log
        assert all(c.startswith('status ') for c in h.commands),h.commands
        print('PASS relay SOAP fault: full diagnostic, Stop/release, STMn once, no fallback or mutation loop',flush=True)
    finally:h.close()

if __name__=='__main__':
    relay_replay();preparing_repeat();idle_start();soap_fault();rate_guard();speaker_faults();polling();external_actions();natural_empty_end()
