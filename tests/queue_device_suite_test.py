"""Owner device transaction against loopback CLI/SlimProto/SOAP and fake systemd."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import contextlib
import copy
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import socket
import socketserver
import struct
import sys
import tarfile
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.parse import quote, unquote
from queue_controller_test import Coordinator, Handler, didl
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'third_party/yeney-core/tests'))
from fake_lms import LMS
loader=importlib.machinery.SourceFileLoader('queue_device_suite',str(ROOT/'scripts/yeney-queue-device-suite'))
spec=importlib.util.spec_from_loader(loader.name,loader)
m=importlib.util.module_from_spec(spec);loader.exec_module(m)
MAC='aa:bb:cc:dd:ee:ff'


class Model:
    def __init__(self):
        self.library=[]
        for album,kind,count,rate in [(1,'alc',4,44100),(2,'alc',1,44100),(3,'alc',1,44100),(4,'alc',1,44100),(5,'flc',4,44100),(6,'flc',1,48000)]:
            for i in range(count):
                n=str(len(self.library)+1)
                self.library.append(dict(id=n,title='Title & '+n,artist='Artist <one>',album='Album '+str(album),album_id=str(album),
                                         type=kind,samplerate=str(rate),duration='60',url='file:///music/'+n+'.m4a'))
        self.byid={t['id']:t for t in self.library}
        self.urls=['file:///saved/one.flac','file:///saved/two.flac']
        self.before=list(self.urls)
        self.index=1;self.time=12.5;self.mode='pause';self.repeat='2';self.shuffle='0'
        self.prefs=dict(transitionType='4',transitionDuration='7',transitionSmart='1')
        self.calls=[];self.clock=100.;self.queue=False;self.epoch=0;self.owned=False;self.fault=None;self.backend=None
        self.slim=LMS()
        self.peer=socket.create_connection(self.slim.listener.getsockname())
        self.peer.sendall(b'HELO'+struct.pack('!I',36)+bytes(36));self.slim.accept()
        self.slim_commands=[];self.peer.settimeout(.2);self.stopping=False
        def consume():
            pending=bytearray()
            while not self.stopping:
                try:
                    data=self.peer.recv(4096)
                    if not data:break
                    pending.extend(data)
                    while len(pending)>=2:
                        n=struct.unpack('!H',pending[:2])[0]
                        if len(pending)<n+2:break
                        frame=bytes(pending[2:n+2]);del pending[:n+2]
                        assert frame[:4]==b'strm'
                        self.slim_commands.append(chr(frame[4]))
                except socket.timeout:pass
                except OSError:
                    if not self.stopping:raise
                    break
        self.worker=threading.Thread(target=consume,name='device-fake-slim');self.worker.start()
    def close(self):
        self.stopping=True;self.peer.close();self.slim.close();self.worker.join(3)
        assert not self.worker.is_alive()
    def track(self):
        return next((t for t in self.library if t['url']==self.urls[self.index]),dict(id='saved',title='Saved',artist='',album=''))
    def wire(self,*args):
        self.calls.append(args)
        if args[0]=='players':return ['playerindex:0','playerid:'+MAC,'name:Study (Sonos)','connected:1']
        if args[0]=='syncgroups':return ['sync_members:'+MAC]
        if args[0]=='titles':return ['count:'+str(len(self.library))]+[k+':'+v for t in self.library[int(args[1]):int(args[1])+int(args[2])] for k,v in t.items()]
        assert args[0]==MAC and '%' not in args[0],args
        cmd=args[1:]
        if cmd[0]=='status':
            entries=list(enumerate(self.urls))
            if cmd[1]=='-':entries=entries[self.index:self.index+1]
            else:entries=entries[int(cmd[1]):int(cmd[1])+int(cmd[2])]
            result=[f'playlist_cur_index:{self.index}',f'time:{self.time}']
            for i,url in entries:
                t=next((t for t in self.library if t['url']==url),dict(id='saved',title='Saved'))
                result += [f'playlist index:{i}',f'url:{url}',f'id:{t["id"]}',f'title:{t["title"]}']
            return result
        if cmd[-1]=='?':
            if cmd[0]=='playerpref':return [self.prefs[cmd[1]]]
            key=cmd[-2]
            return [str(dict(index=self.index,time=self.time,tracks=len(self.urls),mode=self.mode,repeat=self.repeat,shuffle=self.shuffle)[key])]
        if cmd[0]=='playerpref':self.prefs[cmd[1]]=cmd[2]
        elif cmd[0]=='playlistcontrol':
            ids=cmd[-1].split(':')[1].split(',');self.urls=[self.byid[i]['url'] for i in ids];self.index=0;self.time=0;self.epoch+=1
        elif cmd[0]=='playlist':
            if cmd[1]=='index':self.index=(self.index+int(cmd[2]) if cmd[2].startswith(('+','-')) else int(cmd[2]));self.time=0;self.slim.strm('s')
            elif cmd[1]=='clear':self.urls=[];self.index=0;self.time=0
            elif cmd[1]=='add':self.urls.append(cmd[2])
            else:setattr(self,cmd[1],cmd[2])
        elif cmd[0]=='time':self.time=float(cmd[1]);self.slim.strm('f');self.slim.strm('s')
        elif cmd[0]=='pause':self.mode='pause';self.slim.strm('p')
        elif cmd[0] in ('play','stop'):
            self.mode=cmd[0];self.slim.strm('u' if cmd[0]=='play' else 'q')
            if self.mode=='play' and self.queue:self.owned=True;self.backend.sonos.crossfade=False
        else:raise AssertionError(args)
        self.backend.snapshot()
        return []


class CLI(socketserver.ThreadingTCPServer):
    allow_reuse_address=True;daemon_threads=True
    def __init__(self,model):
        class Client(socketserver.StreamRequestHandler):
            def handle(self):
                words=[unquote(t) for t in self.rfile.readline().decode().split()]
                response=model.wire(*words)
                prefix=words[:-1] if words[-1]=='?' else words
                self.wfile.write((' '.join(quote(w,safe=':?,') for w in prefix+response)+'\n').encode())
        super().__init__(('127.0.0.1',9090),Client)
        self.worker=threading.Thread(target=self.serve_forever,name='device-fake-cli');self.worker.start()
    def close(self):self.shutdown();self.server_close();self.worker.join()


class SOAP(Handler):
    def do_GET(self):self.path='/description.xml';super().do_GET()
    def action(self,service,action,args):
        model=self.server.model
        if action=='Browse' and service=='ContentDirectory' and args['ObjectID']!='SQ:':
            entries=self.server.snapshots[args['ObjectID']]
            return dict(Result=didl(entries),NumberReturned=len(entries),TotalMatches=len(entries),UpdateID='1'),None
        if action=='GetPositionInfo':
            if not model.urls or model.mode=='stop':return dict(Track='0',TrackURI='',RelTime='00:00:00',TrackMetaData=''),None
            t=model.track();position=model.time
            if model.fault=='regression' and model.time>4:position=0
            uri=f'http://127.0.0.1/music/queue.wav?epoch={model.epoch}&item={t["id"]}' if model.queue else 'http://127.0.0.1/music/stream.flac'
            from xml.sax.saxutils import escape
            title='Wrong' if model.fault=='identity' else t['title']
            metadata='<DIDL-Lite xmlns:dc="urn:dc" xmlns:upnp="urn:upnp"><item><dc:title>'+escape(title)+'</dc:title><upnp:album>'+escape(t['album'])+'</upnp:album></item></DIDL-Lite>'
            return dict(Track=str(model.index+1),TrackURI=uri,RelTime=f'{int(position)//3600:02}:{int(position)//60%60:02}:{int(position)%60:02}',TrackMetaData=metadata),None
        if action=='GetTransportInfo':return dict(CurrentTransportState={'play':'PLAYING','pause':'PAUSED_PLAYBACK','stop':'STOPPED'}[model.mode]),None
        return super().action(service,action,args)


class Backend:
    def __init__(self,model,sonos,root):
        self.model=model;self.sonos=sonos;self.root=root;self.suite=None;self.calls=[];self.pid=100;self.grouped=False;self.running=True;self.signal=None;self.inject=None
    def now(self):return self.model.clock
    def sleep(self,duration):
        self.model.clock+=duration
        if self.signal:
            number,self.signal=self.signal,None;os.kill(os.getpid(),number)
        if self.inject:
            fault,self.inject=self.inject,None;raise RuntimeError(fault)
        if self.model.mode=='play':
            self.model.time+=duration
            length=50 if self.model.prefs['transitionType']=='1' else 60
            if self.model.queue and self.model.time>=length and self.model.index+1<len(self.model.urls):
                self.model.time-=length;self.model.index+=1
        self.snapshot()
    def snapshot(self):
        if self.model.owned:
            self.sonos.items=[dict(uri='http://127.0.0.1/music/queue.wav?session=fixture')]
        p=dict(requested='queue' if self.model.queue else 'stream',active='queue' if self.model.queue else 'stream',reason='queue-opt-in',occurrence=self.model.index+1,epoch=self.model.epoch,
               owned=self.model.owned,mirrored_ahead=3,last_fallback='soap-error' if self.model.fault=='fallback' else '',handoff_gap_ms=-1,latched=False)
        path=self.root/('stream-pacing-'+b'Study'.hex()+'.json')
        path.write_text(json.dumps(dict(pid=self.pid,monotonic_ms=self.now()*1000,playback=p)))
    def run(self,*args):
        self.calls.append(args)
        if args[0]=='systemd-escape':return 'yeney@Study.service'
        if args[0].endswith('/yeney'):return 'Study\tPort\t127.0.0.1\tStudy\t'+('Study,Kitchen' if self.grouped else 'Study')
        if args[0]=='journalctl':return 'room Study journal only'
        assert args[0]=='systemctl',args
        if args[1]=='show':return str(self.pid) if 'MainPID' in args else ('YENEY_PLAYBACK_MODE=queue' if self.suite.dropin and self.suite.dropin.exists() else 'ORIGINAL=1')
        if args[1]=='is-active' and not self.running:raise m.SuiteError('not running')
        if args[1] in ('restart','stop'):
            assert args[-1]=='yeney@Study.service'
            self.model.owned=False;self.sonos.items=[];self.sonos.crossfade=True
            self.model.queue=bool(self.suite.dropin and self.suite.dropin.exists())
            self.pid+=1;self.snapshot()
        return ''


class DummyWS:
    def __init__(self,*args):self.rows=[]
    def start(self):pass
    def close(self):pass


class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='queue-device-test-');self.root=Path(self.tmp.name)
        self.model=Model();self.sonos=Coordinator(('127.0.0.1',1400),SOAP);self.sonos.model=self.model
        self.cli=CLI(self.model);self.backend=Backend(self.model,self.sonos,self.root);self.model.backend=self.backend
        self.original=copy.deepcopy((self.model.urls,self.model.index,self.model.time,self.model.mode,self.model.repeat,self.model.shuffle,self.model.prefs))
        self.suite=m.Suite('Study','127.0.0.1',backend=self.backend,unit_root=self.root/'units',state_root=self.root,output=self.root)
        self.backend.suite=self.suite
    def tearDown(self):
        self.suite.logfile.close();self.cli.close();self.sonos.close();self.model.close();self.tmp.cleanup()
    def run_suite(self):
        with contextlib.redirect_stdout(io.StringIO()) as output,patch.object(m,'Sampler',DummyWS):
            code=self.suite.execute()
        return code,output.getvalue()
    def restored(self):
        self.assertEqual((self.model.urls,self.model.index,self.model.time,self.model.mode,self.model.repeat,self.model.shuffle,self.model.prefs),self.original)
        self.assertFalse(self.suite.dropin.exists());self.assertTrue(self.sonos.crossfade)
        self.assertFalse(self.sonos.items)
        self.assertFalse(any('volume' in args for args in self.model.calls))
        self.assertTrue(all(args[-1]=='yeney@Study.service' for args in self.backend.calls if args[0]=='systemctl' and args[1] in ('restart','stop','is-active')))
    def test_full_sequence(self):
        code,output=self.run_suite();self.assertEqual(code,0,output)
        self.restored();self.assertIn('DEVICE SUITE OK',output)
        self.assertEqual([r['joins'] for r in self.suite.results[:4]],[3]*4)
        self.assertEqual([r['name'] for r in self.suite.results],['alac-off','flac-off','alac-crossfade','mixed-rate','lms-controls','resume-after-stop','stream-reference-60s'])
        self.assertIn('WS POSITION: unavailable',output)
        with tarfile.open(self.root/'yeney-queue-device-suite-latest.tar.gz') as tar:
            names={n.split('/',1)[-1] for n in tar.getnames()}
            self.assertTrue({'summary.md','samples.jsonl','joins.jsonl','seeks.jsonl','manifest.tsv','journal.log','backup.json','before.json','result.txt','websocket-summary.json'}<=names,names)
        self.assertEqual(len(list(self.root.glob('yeney-queue-device-suite-20*.tar.gz'))),1)
        self.assertTrue({'p','u','s','f','q'}<=set(self.model.slim_commands))
        print('PASS queue device suite: full loopback CLI/SlimProto/SOAP sequence, three joins per album, controls, 60 s stream reference, archive and exact restoration')
    def test_signal_and_error_cleanup(self):
        for event in (signal.SIGINT,signal.SIGHUP,signal.SIGTERM,'injected error'):
            with self.subTest(event=event):
                if isinstance(event,int):self.backend.signal=event
                else:self.backend.inject=event
                code,output=self.run_suite();self.assertEqual(code,1,output);self.restored()
                # New transaction workspace/flags, same untouched saved room state.
                self.suite=m.Suite('Study','127.0.0.1',backend=self.backend,unit_root=self.root/'units',state_root=self.root,output=self.root)
                self.backend.suite=self.suite
        print('PASS queue device suite: Ctrl-C, SIGHUP, SIGTERM and errors restore only the selected room')
    def test_failures(self):
        for fault in ('identity','fallback','regression'):
            with self.subTest(fault=fault):
                self.model.fault=fault
                code,output=self.run_suite();self.assertEqual(code,1,output);self.restored()
                self.suite=m.Suite('Study','127.0.0.1',backend=self.backend,unit_root=self.root/'units',state_root=self.root,output=self.root);self.backend.suite=self.suite
        print('PASS queue device suite: identity mismatch, fallback and RelTime regression fail without losing restoration')
    def test_refusals(self):
        self.backend.grouped=True
        code,_=self.run_suite();self.assertEqual(code,1);self.assertFalse(any(c[0]=='systemctl' and c[1]=='restart' for c in self.backend.calls))
        self.assertEqual(self.model.urls,self.original[0])
        self.suite=m.Suite('Study','127.0.0.1',backend=self.backend,unit_root=self.root/'units',state_root=self.root,output=self.root);self.backend.suite=self.suite
        self.backend.grouped=False;self.backend.running=False
        code,_=self.run_suite();self.assertEqual(code,1)
        self.suite=m.Suite('Study','127.0.0.1',backend=self.backend,unit_root=self.root/'units',state_root=self.root,output=self.root);self.backend.suite=self.suite
        self.backend.running=True
        fd=os.open(self.root/('yeney-stream-pacing-'+b'Study'.hex()+'.lock'),os.O_CREAT|os.O_RDWR,0o600)
        import fcntl
        fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
        try:code,output=self.run_suite();self.assertEqual(code,1);self.assertIn('lock',output)
        finally:os.close(fd)
        print('PASS queue device suite: grouped/inactive room and existing suite lock refuse before mutations')
    def test_preexisting_dropin_and_backup_failure(self):
        directory=self.suite.unit_root/'yeney@Study.service.d';directory.mkdir(parents=True)
        existing=directory/'queue-suite.conf';existing.write_text('OWNER CONTENT\n')
        code,output=self.run_suite();self.assertEqual(code,1,output)
        self.assertEqual(existing.read_text(),'OWNER CONTENT\n')
        self.assertFalse(any(c[0]=='systemctl' and c[1]=='restart' for c in self.backend.calls))
        self.suite=m.Suite('Study','127.0.0.1',backend=self.backend,unit_root=self.root/'units',state_root=self.root,output=self.root);self.backend.suite=self.suite
        self.sonos.hide_save=True
        code,output=self.run_suite();self.assertEqual(code,1,output)
        self.assertIn('backup identity unverified',output)
        self.assertEqual(self.model.urls,self.original[0])
        print('PASS queue device suite: existing drop-in preserved and unverifiable backup prevents takeover')

    def test_seek_requires_speaker_evidence(self):
        with patch.object(self.suite,'setup',lambda:None),patch.object(self.suite,'cleanup',lambda:None):
            self.suite.mac=MAC;self.suite.pid=self.backend.pid
            self.model.queue=True;self.model.owned=True
            tracks=self.model.library[:4];self.model.urls=[t['url'] for t in tracks];self.model.index=0;self.model.time=0
            self.suite.begin('seek-evidence');self.backend.snapshot()
            # LMS has acknowledged the target; SOAP stays at zero.
            original=self.suite.speaker
            self.suite.speaker=m.Speaker('127.0.0.1',self.suite.log)
            real=self.suite.speaker.evidence
            def unchanged():
                value=real();value['rel']=0;return value
            self.model.time=30
            with patch.object(self.suite.speaker,'evidence',unchanged):
                with self.assertRaisesRegex(m.SuiteError,'deadline'):
                    self.suite.wait(tracks,lambda r:abs(r['speaker']['rel']-30)<=2,3,discontinuity=True)
            self.suite.speaker=original
        print('PASS queue device suite: CLI seek acknowledgement without settled speaker position cannot pass')

    def test_no_48(self):
        self.model.library=[t for t in self.model.library if t['samplerate']!='48000']
        self.suite.mac=MAC;self.suite.library()
        self.assertEqual(self.suite.results[0]['verdict'],'SKIP')
        self.suite.logfile.close()
        print('PASS queue device suite: absent 48 kHz library is an explicit non-failing skip')


if __name__=='__main__':unittest.main(verbosity=2)
