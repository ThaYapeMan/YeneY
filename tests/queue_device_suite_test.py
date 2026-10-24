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
import subprocess
import random
import threading
import unittest
from unittest.mock import patch
from urllib.parse import quote, unquote, quote_from_bytes
from queue_sonos_fake import Coordinator, Handler, didl
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
        self.favourites=[];self.podcasts=[];self.favourite_children={};self.unplayable=set();self.started_urls=[];self.started_url=""
        self.speaker_metadata={};self.title_format='artist-title'
        self.byid={t['id']:t for t in self.library}
        self.urls=['file:///saved/one.flac','file:///saved/two.flac']
        self.before=list(self.urls)
        for i,url in enumerate(self.urls):self.byid[str(9001+i)]=dict(id=str(9001+i),url=url,title='Saved',artist='',album='')
        self.index=1;self.time=12.5;self.mode='pause';self.repeat='2';self.shuffle='0'
        self.volume='37'
        self.prefs=dict(transitionType='4',transitionDuration='7',transitionSmart='1')
        self.play_prior_modes=[]
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
        if cmd[0] in ('favorites','podcasts'):
            ident=next((v[8:] for v in cmd if v.startswith('item_id:')),None)
            rows=(self.favourite_children.get(ident,[]) if ident is not None else self.favourites) if cmd[0]=='favorites' else (self.podcasts if ident is None else [])
            return ['count:'+str(len(rows))]+[k+':'+str(v) for row in rows for k,v in row.items()]
        if cmd[0]=='status':
            entries=list(enumerate(self.urls))
            if cmd[1]=='-':entries=entries[self.index:self.index+1]
            else:entries=entries[int(cmd[1]):int(cmd[1])+int(cmd[2])]
            result=[f'playlist_cur_index:{self.index}',f'time:{self.time}']
            for i,url in entries:
                t=next((t for t in self.byid.values() if t['url']==url),dict(id='',title='Saved'))
                result += [f'playlist index:{i}',f'url:{url}',f'id:{t["id"]}',f'title:{t["title"]}',f'artist:{t.get("artist","")}',f'album:{t.get("album","")}']
            return result
        if cmd[-1]=='?':
            if cmd[0]=='playerpref':return [self.prefs[cmd[1]]]
            key=cmd[-2]
            return [str(dict(index=self.index,time=self.time,tracks=len(self.urls),mode=self.mode,repeat=self.repeat,shuffle=self.shuffle,volume=self.volume)[key])]
        if cmd[0]=='mixer':self.volume=cmd[2]
        elif cmd[0]=='playerpref':self.prefs[cmd[1]]=cmd[2]
        elif cmd[0]=='playlistcontrol':
            ids=next(v for v in cmd if v.startswith('track_id:')).split(':',1)[1].split(',')
            values=[self.byid[i]['url'] for i in ids]
            if cmd[1]=='cmd:load':
                self.urls=values;self.index=0;self.time=0;self.epoch+=1
                self.mode='play'
            else:self.urls+=values
        elif cmd[0]=='playlist':
            if cmd[1]=='index':self.index=(self.index+int(cmd[2]) if cmd[2].startswith(('+','-')) else int(cmd[2]));self.time=0;self.slim.strm('s')
            elif cmd[1]=='clear':self.urls=[];self.index=0;self.time=0
            elif cmd[1]=='add':self.urls.append(cmd[2])
            else:setattr(self,cmd[1],cmd[2])
        elif cmd[0]=='time':self.time=float(cmd[1]);self.slim.strm('f');self.slim.strm('s')
        elif cmd[0]=='pause':self.mode='pause';self.slim.strm('p')
        elif cmd[0] in ('play','stop'):
            if cmd[0]=='play':
                self.play_prior_modes.append(self.mode)
                if self.urls and self.urls[self.index] not in self.unplayable:
                    if self.mode!='pause':
                        self.slim.strm('s');self.started_url=self.urls[self.index];self.started_urls.append(self.started_url)
                    self.slim.strm('u')
            else:self.slim.strm('q')
            self.mode=cmd[0]
            if self.mode=='play' and self.queue:
                self.owned=self.track()['url'].startswith('file://')
                self.backend.sonos.crossfade=not self.owned
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
        if action=='GetVolume':return dict(CurrentVolume=self.server.volume),None
        if action=='SetVolume':
            assert service=='RenderingControl' and args['InstanceID']=='0' and args['Channel']=='Master'
            self.server.volume=int(args['DesiredVolume']);return {},None
        if action=='Stop':
            if model.fault=='external-stop':return {},None
            model.owned=False;self.server.items=[];self.server.crossfade=True
            model.external_stops=getattr(model,'external_stops',0)+1
            model.wire(MAC,'stop')
            return {},None
        if action=='SaveQueue' and self.server.fail_save:
            return {},'800'
        if action=='Browse' and service=='ContentDirectory' and args['ObjectID']!='SQ:':
            entries=self.server.snapshots[args['ObjectID']]
            return dict(Result=didl(entries),NumberReturned=len(entries),TotalMatches=len(entries),UpdateID='1'),None
        if action=='GetPositionInfo':
            if not model.urls or model.mode=='stop':return dict(Track='0',TrackURI='',RelTime='00:00:00',TrackMetaData=''),None
            t=model.track();position=model.time
            if model.fault=='regression' and model.time>4:position=0
            uri=f'http://127.0.0.1/music/queue.wav?epoch={model.epoch}&item={t["id"]}' if model.owned else 'http://127.0.0.1/music/yeney.flac?session=fixture&stream=1'
            from xml.sax.saxutils import escape
            title='Wrong' if model.fault=='identity' else t['artist']+' - '+t['title'] if model.title_format=='artist-title' and t.get('artist') else t['title']
            tags=dict(title=title,album=t['album']);tags.update(model.speaker_metadata.get(t['id'],{}))
            metadata='<DIDL-Lite xmlns:dc="urn:dc" xmlns:upnp="urn:upnp"><item><dc:title>'+escape(tags['title'])+'</dc:title><upnp:album>'+escape(tags['album'])+'</upnp:album></item></DIDL-Lite>'
            return dict(Track=str(model.index+1),TrackURI=uri,RelTime=f'{int(position)//3600:02}:{int(position)//60%60:02}:{int(position)%60:02}',TrackMetaData=metadata),None
        if action=='GetTransportInfo':return dict(CurrentTransportState={'play':'PLAYING','pause':'PAUSED_PLAYBACK','stop':'STOPPED'}[model.mode]),None
        return super().action(service,action,args)


class Backend:
    def __init__(self,model,sonos,root):
        self.model=model;self.sonos=sonos;self.root=root;self.suite=None;self.calls=[];self.pid=100;self.grouped=False;self.running=True;self.signal=None;self.inject=None;self.environment='ORIGINAL=1'
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
        p=dict(state='PLAYING' if self.model.mode=='play' else 'PAUSED' if self.model.mode=='pause' else 'STOPPED',requested='queue' if self.model.queue else 'stream',active='queue' if self.model.queue else 'stream',reason='local-source' if self.model.owned else 'remote-source',path='relay' if self.model.owned else 'stream',occurrence=self.model.index+1,epoch=self.model.epoch,
               rendered_rate=(44100 if not self.model.urls or int(self.model.track().get('samplerate','44100'))%44100==0 else 48000),owned=self.model.owned,mirrored_ahead=3,last_fallback='soap-error' if self.model.fault=='fallback' else '',handoff_gap_ms=-1,latched=False)
        path=self.root/('stream-pacing-'+b'Study'.hex()+'.json')
        path.write_text(json.dumps(dict(pid=self.pid,monotonic_ms=self.now()*1000,playback=p)))
    def run(self,*args):
        self.calls.append(args)
        if args[0]=='systemd-escape':return 'yeney@Study.service'
        if args[0].endswith('/yeney'):return 'Study\tPort\t127.0.0.1\tStudy\t'+('Study,Kitchen' if self.grouped else 'Study')
        if args[0]=='journalctl':return 'core: STMc\nroom Study journal only' if self.model.mode=='play' and self.model.urls and self.model.started_url==self.model.urls[self.model.index] and self.model.started_url not in self.model.unplayable else 'room Study journal only'
        assert args[0]=='systemctl',args
        if args[1]=='show' and 'StateDirectory' in args: return ''
        if args[1]=='show':return str(self.pid) if 'MainPID' in args else self.environment+(' YENEY_PLAYBACK_MODE=queue' if self.suite.dropin and self.suite.dropin.exists() else '')
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
        with contextlib.redirect_stdout(io.StringIO()) as output,contextlib.nullcontext():
            code=self.suite.execute()
        return code,output.getvalue()
    def restored(self):
        self.assertEqual((self.model.urls,self.model.index,self.model.time,self.model.mode,self.model.repeat,self.model.shuffle,self.model.prefs),self.original)
        self.assertFalse(self.suite.dropin.exists());self.assertTrue(self.sonos.crossfade)
        self.assertFalse(self.sonos.items)
        self.assertEqual(self.model.volume,'37')
        self.assertEqual(self.sonos.volume,23)
        self.assertTrue(all(args[-1]=='yeney@Study.service' for args in self.backend.calls if args[0]=='systemctl' and args[1] in ('restart','stop','is-active')))
    def field_album(self):
        clean='Het beste uit pop in je moerstaal, Vol. 2'
        for track in self.model.library[:4]:
            track['album']='\x16'+clean
            self.model.speaker_metadata[track['id']]=dict(album=clean)
        return clean

    def test_field_sanitized_album_identity(self):
        clean=self.field_album()
        def replay():
            self.suite.begin('album')
            tracks=self.suite.selected['album'];self.suite.load(tracks)
            row=self.suite.samples[-1]
            self.assertTrue(row['identity']);self.assertEqual(row['speaker']['metadata']['album'],clean)
            self.assertEqual(row['lms']['id'],tracks[0]['id'])
            self.assertLessEqual(self.suite.current['startup'],3)
        with patch.object(self.suite,'measure',side_effect=replay):code,output=self.run_suite()
        self.assertEqual(code,0,output);self.restored()
        print('PASS device suite 10:16 replay: leading U+0016 LMS album matches sanitized speaker album within startup deadline; ID and restoration unchanged')

    def test_different_album_still_fails(self):
        self.field_album()
        def replay():
            self.suite.begin('album')
            tracks=self.suite.selected['album'];self.suite.load(tracks)
            self.model.speaker_metadata[tracks[0]['id']]['album']='A genuinely different album'
            for _ in range(4):self.suite.sample(tracks)
            self.fail('a persistent wrong album must fail')
        with patch.object(self.suite,'measure',side_effect=replay):code,output=self.run_suite()
        self.assertEqual(code,1,output)
        self.assertIn('speaker/LMS identity mismatch >2 s',output)
        self.assertEqual(self.suite.results[0]['mismatch'],3)
        self.restored()
        print('PASS device suite different album: identity stays false and fails after the unchanged >2 s limit; restoration succeeds')

    def test_configured_title_format(self):
        for configured in (None,'artist-title','title','invalid',''):
            with self.subTest(configured=configured):
                self.backend.environment='ORIGINAL=1'+(' "YENEY_TITLE_FORMAT='+configured+'"' if configured is not None else '')
                selected='title' if configured=='title' else 'artist-title'
                self.model.title_format=selected
                def replay():
                    self.assertEqual(self.suite.title_format,selected)
                    self.suite.begin('album');tracks=self.suite.selected['album'];self.suite.load(tracks)
                    self.assertTrue(self.suite.samples[-1]['identity'])
                    track=tracks[0]
                    wrong=track['title'] if selected=='artist-title' else track['artist']+' - '+track['title']
                    self.model.speaker_metadata[track['id']]=dict(title=wrong)
                    self.assertFalse(self.suite.sample(tracks,preparing=True)['identity'])
                    self.model.speaker_metadata.clear()
                with patch.object(self.suite,'measure',side_effect=replay):code,output=self.run_suite()
                self.assertEqual(code,0,output);self.restored()
                self.suite=m.Suite('Study','127.0.0.1',backend=self.backend,unit_root=self.root/'units',state_root=self.root,output=self.root)
                self.backend.suite=self.suite
        print('PASS device suite title format: room environment selects exactly title or artist-title; unset/invalid/empty use artist-title; wrong format rejected')

    def test_full_sequence(self):
        code,output=self.run_suite();self.assertEqual(code,0,output)
        self.restored();self.assertIn('DEVICE SUITE OK',output)
        self.assertEqual([a[1]['DesiredVolume'] for a in self.sonos.actions('SetVolume')],['10','23'])
        self.assertEqual(self.model.play_prior_modes,['stop','pause','stop'])
        self.assertEqual(self.model.external_stops,1)
        self.assertEqual(next(r for r in self.suite.results if r['name']=='external-stop')['verdict'],'PASS')
        self.assertEqual(self.suite.results[0]['joins'],3)
        self.assertEqual([r['name'] for r in self.suite.results],['album','pause-resume','stop-new-play','external-stop','high-rate-44100','high-rate-48000','remote-source'])
        self.assertTrue(all(r['verdict']=='SKIP' and 'library has no eligible' in r['note'] for r in self.suite.results[4:6]))
        with tarfile.open(self.root/'yeney-queue-device-suite-latest.tar.gz') as tar:
            names={n.split('/',1)[-1] for n in tar.getnames()}
            self.assertTrue({'summary.md','samples.jsonl','joins.jsonl','manifest.tsv','journal.log','backup.json','before.json','result.txt'}<=names,names)
        self.assertEqual(len(list(self.root.glob('yeney-queue-device-suite-20*.tar.gz'))),1)
        self.assertTrue({'p','u','q'}<=set(self.model.slim_commands))
        self.assertEqual(len(self.sonos.actions('SaveQueue')),1)
        self.assertIn('Before YeneY backup verified: SQ:',(self.suite.work/'summary.md').read_text())
        self.assertIn('Q:0 before takeover: entries=1 owned=0 foreign=1',(self.suite.work/'commands.log').read_text())
        print('PASS queue device backup: foreign entries require an identity/content-verified saved queue')
        print('PASS device external-stop: direct SOAP Stop, LMS stopped within 3 s, owned items released')
        print('PASS relay startup measurement: playlist staged stopped with cmd:add; each timed Play starts a new run, resume follows pause')
        print('PASS queue device suite: full loopback CLI/SlimProto/SOAP sequence, album three joins, pause ten seconds, stop/new play, saved/restored volume, archive and exact restoration')
    def test_remote_favourite(self):
        track=dict(id='-42',url='http://radio.invalid/live',title='Radio fixture',artist='',album='',duration='120')
        self.model.speaker_metadata['-42']=dict(title='Previous radio title')
        self.model.byid['-42']=track
        self.model.library.append(track)
        self.model.favourites=[dict(id='0',type='audio',url=track['url'],name=track['title'])]
        code,output=self.run_suite();self.assertEqual(code,0,output)
        self.restored()
        rows={r['name']:r for r in self.suite.results}
        self.assertEqual(rows['remote-source']['verdict'],'PASS')
        self.assertEqual(rows['local-after-remote']['verdict'],'PASS')
        remote=[r for r in self.suite.samples if r['scenario']=='remote-source']
        self.assertTrue(remote and all(r['playback']['path']=='stream' for r in remote))
        self.assertIn('remote-source',(self.suite.work/'manifest.tsv').read_text())
        print('PASS device remote-source: favourite streams within 5 s, local album returns to relay, manifest and owner state restored')

    def test_remote_feed_retry_transaction(self):
        feed='https://plus.bnr.invalid/show.rss?t=fixture'
        urls=['https://audio.invalid/bad.mp3','https://audio.invalid/episode.mp3']
        episodes=[]
        for i,url in enumerate(urls):
            t=dict(id=str(-42-i),url=url,title='Episode '+str(i),artist='',album='',duration='120')
            self.model.byid[t['id']]=t;self.model.library.append(t)
            episodes.append(dict(id='episode'+str(i),type='audio',url=url,title=t['title']))
        self.model.favourites=[dict(id='feed',type='audio',url=feed,name='Boekestijn & De Wijk | Plus')]
        self.model.favourite_children['feed']=episodes
        self.model.unplayable.add(urls[0]);self.suite.remote_argument='Boekestijn & De Wijk | Plus'
        code,output=self.run_suite();self.assertEqual(code,0,output);self.restored()
        rows={r['name']:r for r in self.suite.results}
        self.assertEqual(rows['remote-source']['verdict'],'PASS')
        self.assertEqual(rows['local-after-remote']['verdict'],'PASS')
        self.assertNotIn(urls[0],self.model.started_urls);self.assertIn(urls[1],self.model.started_urls)
        self.assertNotIn((MAC,'playlist','add',feed),self.model.calls)
        self.assertIn('remote candidate rejected:',(self.suite.work/'commands.log').read_text())
        self.assertIn('remote-candidate',(self.suite.work/'manifest.tsv').read_text())
        print('PASS device field RSS favourite: explicit feed browsed through fake LMS CLI, no-strm episode unloaded after 5 s, next audio episode streams, local relay and owner state restored')

    def test_remote_podcast_menu(self):
        self.model.podcasts=[dict(id='episode',type='audio',url='podcast://https://podcast.invalid/episode.mp3',title='Podcast')]
        self.suite.mac=MAC
        choice,source=self.suite.remote_choice()
        self.assertEqual(source,'podcasts')
        self.assertTrue(choice['url'].startswith('podcast://'))
        self.assertEqual(choice['id'],'')
        print('PASS device remote selection: empty favourites falls through to a podcast; remote identity is its URL')

    def test_high_rate_families(self):
        for rate in (176400,192000):
            n=str(len(self.model.byid)+1)
            track=dict(id=n,title='High '+n,artist='Artist <one>',album='High '+n,album_id=n,
                       type='flc',samplerate=str(rate),duration='60',url='file:///music/'+n+'.flac')
            self.model.library.append(track);self.model.byid[n]=track
        self.assertEqual(self.suite.execute(),0)
        rows={r['name']:r for r in self.suite.results}
        for name,target in (('high-rate-44100',44100),('high-rate-48000',48000)):
            self.assertEqual(rows[name]['verdict'],'PASS')
            self.assertIn(str(target)+' Hz',rows[name]['note'])
            self.assertLessEqual(rows[name]['startup'],3)
            self.assertIn('limit 3 s',rows[name]['note'])
        print('PASS device suite high-rate families: 176.4 -> 44.1 and 192 -> 48 kHz, status-verified, restored')

    def test_backup_not_needed(self):
        for label,entries,note in (
                ('empty',[],'queue empty'),
                ('owned',[dict(uri='http://127.0.0.1/music/queue.wav?session=previous&epoch=1&item=1')],'only owned entries')):
            with self.subTest(queue=label):
                self.sonos.items=entries
                self.sonos.fail_save=True  # Match the field failure if SaveQueue is attempted.
                code,output=self.run_suite();self.assertEqual(code,0,output)
                self.restored()
                self.assertFalse(self.sonos.actions('SaveQueue'))
                self.assertEqual(self.suite.results[0]['joins'],3)
                self.assertIn('backup not needed: '+note,(self.suite.work/'summary.md').read_text())
                backup=json.loads((self.suite.work/'backup.json').read_text())
                self.assertFalse(backup['needed'])
                self.assertIn(f'Q:0 before takeover: entries={len(entries)} owned={len(entries)} foreign=0',(self.suite.work/'commands.log').read_text())
                print('PASS queue device backup: '+note+' skips SaveQueue and completes all scenarios')
                self.suite=m.Suite('Study','127.0.0.1',backend=self.backend,unit_root=self.root/'units',state_root=self.root,output=self.root)
                self.backend.suite=self.suite

    def test_named_soap_http_fault(self):
        self.sonos.fail_save=True
        code,output=self.run_suite();self.assertEqual(code,1,output)
        expected='SOAP AVTransport SaveQueue failed: HTTP 500 UPnPError 800 fixture rejection'
        self.assertIn(expected,output)
        log=(self.suite.work/'commands.log').read_text()
        request=log.index('SOAP AVTransport SaveQueue request ')
        outcome=log.index(expected)
        self.assertLess(request,outcome)
        self.assertIn('SOAP Queue Browse succeeded: HTTP 200',log)
        self.assertFalse(any(c[0]=='systemctl' and c[1]=='restart' for c in self.backend.calls))
        print('PASS queue device SOAP: HTTP 500 fault names service/action/code/description; request precedes outcome in log')

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
        for fault in ('identity','fallback','regression','external-stop'):
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

    def test_album_selection(self):
        self.suite.mac=MAC;self.suite.album_choice='5';self.suite.library()
        self.assertEqual(self.suite.selected['album'][0]['album_id'],'5')
        self.suite.album_choice='Album 1';self.suite.library()
        self.assertEqual(self.suite.selected['album'][0]['album_id'],'1')
        self.suite.album_choice='missing'
        with self.assertRaises(m.SuiteError):self.suite.library()
        print('PASS relay device album selection: ID/name/default and ineligible rejection')


class SanitizerParity(unittest.TestCase):
    def test_cpp_xml_sanitize_parity(self):
        source = r'''
#include "upnp/xml.h"
#include <iostream>
#include <iomanip>
int main() {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::string raw;
        for (size_t i=0;i<line.size();i+=2) raw+=char(std::stoul(line.substr(i,2),nullptr,16));
        for (unsigned char c:upnp::xmlSanitize(raw))
            std::cout<<std::hex<<std::setfill('0')<<std::setw(2)<<unsigned(c);
        std::cout<<'\n';
    }
}
'''
        cases=[b'']+[bytes([v]) for v in range(256)]
        cases += [b'\xe2\x82',b'\xe2(\xa1',b'\xe0\x80\x80',b'\xf4\x90\x80\x80',
                  b'\xc0\xaf',b'\x16Het beste uit pop in je moerstaal, Vol. 2']
        cases += [chr(v).encode('utf-8','surrogatepass') for v in
                  (9,10,13,32,0xd7ff,0xd800,0xdfff,0xe000,0xfffd,0xfffe,0xffff,0x10000,0x10ffff)]
        cases += ['é ü 😀'.encode()]
        rng=random.Random(20261021)
        cases += [bytes(rng.randrange(256) for _ in range(rng.randrange(16))) for _ in range(512)]
        with tempfile.TemporaryDirectory(prefix='queue-suite-xml-parity-') as directory:
            directory=Path(directory);cpp=directory/'parity.cpp';cpp.write_text(source)
            binary=directory/'parity'
            subprocess.run(['g++','-std=c++17','-O2','-I',str(ROOT),str(cpp),str(ROOT/'upnp/xml.cpp'),'-o',str(binary)],check=True)
            result=subprocess.check_output([str(binary)],input='\n'.join(raw.hex() for raw in cases)+'\n',text=True)
        expected=result.splitlines()
        self.assertEqual(len(expected),len(cases))
        for raw,clean in zip(cases,expected):
            with self.subTest(raw=raw):
                self.assertEqual(m.xml_sanitize(raw).encode().hex(),clean)
                for name in ('title','artist','album'):
                    wire=quote_from_bytes(name.encode()+b':'+raw,safe='')
                    self.assertEqual(m.cli_word(wire).partition(':')[2].encode().hex(),clean)
                    self.assertEqual(m.cli_word(name.encode()+b':'+raw).partition(':')[2].encode().hex(),clean)
        self.assertEqual(m.cli_word('url%3Apodcast%3A%2F%2Fhttps%3A%2F%2Fexample%2FA%2520B'),
                         'url:podcast://https://example/A%20B')
        self.assertEqual(m.cli_word('id%3A-95857229938624'),'id:-95857229938624')
        self.assertEqual(m.expected_metadata(dict(title='\x16é',artist='\x07Björk',album='\x16Album'),'artist-title'),
                         ('Björk - é','Album'))
        self.assertEqual(m.expected_metadata(dict(title='Title',artist='\x16',album='Album'),'artist-title'),
                         ('Title','Album'))
        print('PASS device suite/C++ XML sanitizer parity: all byte values, forbidden controls/scalars, malformed UTF-8, Unicode and deterministic random inputs; CLI metadata repaired before lossy decoding, URLs and IDs unchanged')


if __name__=='__main__':unittest.main(verbosity=2)
