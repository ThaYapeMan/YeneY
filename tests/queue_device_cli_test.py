"""Slow and late LMS responses: real loopback sockets, no device/network access."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import contextlib
import io
import json
import socketserver
import tempfile
import threading
import time
import unittest
from urllib.parse import quote,unquote
from unittest.mock import patch
import queue_device_suite_test as fixtures
from queue_device_suite_test import m, MAC, DummyWS


class Delayed(socketserver.ThreadingTCPServer):
    allow_reuse_address=True
    daemon_threads=False
    def __init__(self, model, delays=None):
        self.model=model;self.delays=delays or {};self.requests=[];self.late=threading.Event()
        self.drop=set();self.reject=set();self.serial=0
        owner=self
        class Client(socketserver.StreamRequestHandler):
            def handle(self):
                owner.serial+=1;connection=owner.serial
                wire=self.rfile.readline().decode().strip()
                args=[unquote(t) for t in wire.split()];command=tuple(args[1:])
                owner.requests.append((connection,wire,args))
                delay=owner.delays.get(command,0)
                if delay:time.sleep(delay)
                if command in owner.drop:return
                if command in owner.reject:
                    result=['error:rejected']
                else:result=model.wire(*args)
                prefix=args[:-1] if args[-1]=='?' else args
                try:self.wfile.write((' '.join(quote(v,safe='') for v in prefix+result)+'\n').encode())
                except ConnectionError:pass
                finally:
                    if delay:owner.late.set()
        super().__init__(('127.0.0.1',0),Client)
        self.worker=threading.Thread(target=self.serve_forever,name='device-delayed-cli');self.worker.start()
    def close(self):self.shutdown();self.server_close();self.worker.join()


@contextlib.contextmanager
def fixture():
    f=fixtures.Tests('test_full_sequence');f.setUp()
    try:yield f
    finally:f.tearDown()


class Tests(unittest.TestCase):
    def test_slow_load_2_and_20_seconds(self):
        with fixture() as f:
            tracks=f.model.library[:4]
            command=('playlistcontrol','cmd:load','track_id:'+','.join(t['id'] for t in tracks))
            server=Delayed(f.model)
            try:
                f.suite.cli=m.CLI('127.0.0.1',f.suite.log,server.server_address[1])
                f.suite.mac=MAC;f.suite.speaker=m.Speaker('127.0.0.1',f.suite.log)
                f.suite.pid=f.backend.pid;f.model.queue=True;f.backend.snapshot()
                for delay in (2,20):
                    server.delays[command]=delay
                    f.suite.begin('slow-load');started=time.monotonic()
                    f.suite.load(tracks)
                    self.assertGreaterEqual(time.monotonic()-started,delay)
                    self.assertLessEqual(f.suite.current['startup'],3)
                self.assertTrue(all('%' not in args[0] for _,_,args in server.requests))
            finally:server.close()
        print('PASS queue device CLI: real 2 s and 20 s loads complete; encoded echoes decode; startup clock begins after verified readiness')
    def test_timeout_and_late_reply(self):
        with fixture() as f:
            server=Delayed(f.model,{('mode','?'):2})
            try:
                client=m.CLI('127.0.0.1',lambda _:None,server.server_address[1],query_timeout=.1)
                with self.assertRaises(m.CLITimeout):client.call(MAC,'mode','?')
                server.delays.clear()
                self.assertEqual(client.call(MAC,'playlist','index','?')[-1],'1')
                self.assertTrue(server.late.wait(3))
                self.assertEqual(client.call(MAC,'mode','?')[-1],'pause')
                self.assertEqual(len({n for n,_,_ in server.requests}),3)
            finally:server.close()
        print('PASS queue device CLI: timeout discards socket; late reply cannot match later requests or fresh cleanup client')
    def test_fourteen_ids_and_smart_restored(self):
        with fixture() as f:
            entries=[]
            for i in range(14):
                entry=dict(id=str(50000+i),url=f'file:///saved/{i}%20track.flac',title='Saved')
                f.model.byid[entry['id']]=entry;entries.append(entry)
            f.model.urls=[e['url'] for e in entries];f.model.index=6;f.model.time=17.5
            original=(list(f.model.urls),dict(f.model.prefs))
            with contextlib.redirect_stdout(io.StringIO()) as output,patch.object(m,'Sampler',DummyWS):
                code=f.suite.execute()
            self.assertEqual(code,0,output.getvalue())
            self.assertEqual(f.model.urls,original[0]);self.assertEqual(f.model.prefs,original[1])
            self.assertTrue(any(args[1:]==('playlistcontrol','cmd:load','track_id:'+','.join(e['id'] for e in entries)) for args in f.model.calls))
            self.assertFalse(any(args[1:3]==('playlist','add') for args in f.model.calls))
            self.assertEqual(f.model.prefs['transitionSmart'],'1')
        print('PASS queue device restore: full 14-track playlist loaded by IDs/order, index/time and every preference including transitionSmart verified')
    def test_stopped_state_restore_never_autoplays(self):
        with fixture() as f:
            f.model.mode='stop'
            original=list(f.model.urls)
            with contextlib.redirect_stdout(io.StringIO()) as output,patch.object(m,'Sampler',DummyWS):
                code=f.suite.execute()
            self.assertEqual(code,0,output.getvalue())
            self.assertEqual(f.model.mode,'stop');self.assertEqual(f.model.urls,original)
            restored=next(args for args in f.model.calls if args[1:]==('playlistcontrol','cmd:add','track_id:9001,9002'))
            tail=f.model.calls[f.model.calls.index(restored):]
            self.assertFalse(any(args[1:3]==('playlistcontrol','cmd:load') for args in tail))
            self.assertFalse(any(args[1:]==('play',) for args in tail))
            self.assertTrue(any(args[1:]==('stop',) for args in tail))
            stop_at=max(i for i,args in enumerate(tail) if args[1:]==('stop',))
            self.assertTrue(any(args[1:]==('mode','?') for args in tail[stop_at+1:]))
            self.assertEqual(tail[-1][1:3],('status','0'))
        print('PASS queue device stopped restore: ordered ID add without load, final stop acknowledged and query-verified, original playlist retained, no autoplaying load or Play request')
    def test_best_effort_and_url_fallback(self):
        with fixture() as f:
            original_url='file:///saved/A%20track.flac'
            f.model.urls=[original_url];f.model.index=0
            server=Delayed(f.model)
            server.reject.add(('playerpref','transitionType','4'))
            try:
                f.suite.cli=m.CLI('127.0.0.1',f.suite.log,server.server_address[1])
                with contextlib.redirect_stdout(io.StringIO()) as output,patch.object(m,'Sampler',DummyWS):
                    code=f.suite.execute()
                self.assertEqual(code,1,output.getvalue())
                self.assertIn('NOT RESTORED: transitionType (expected 4, got 0)',output.getvalue())
                self.assertIn('NOT RESTORED: transitionType (expected 4, got 0)',(f.suite.work/'summary.md').read_text())
                self.assertTrue(any('NOT RESTORED: transitionType (expected 4, got 0)'==v for v in f.suite.not_restored))
                self.assertEqual(f.model.prefs['transitionSmart'],'1')
                self.assertEqual(f.model.prefs['transitionDuration'],'7')
                writes=[wire for _,wire,args in server.requests if args[1:3]==['playlist','add']]
                self.assertEqual(len(writes),1);self.assertIn('A%20track.flac',writes[0]);self.assertNotIn('%2520',writes[0])
                failures=[args for _,_,args in server.requests if tuple(args[1:]) in server.reject]
                self.assertEqual(len(failures),2)
            finally:server.close()
        print('PASS queue device restore: failed preference retries once on fresh sockets, later items restored, exact URL fallback avoids double encoding, NOT RESTORED records expected/actual')
    def test_failed_playlist_item_does_not_abort_preferences(self):
        with fixture() as f:
            server=Delayed(f.model)
            entries=[dict(id='9001',url=f.model.urls[0]),dict(id='99999',url='file:///missing'),dict(id='9002',url=f.model.urls[1])]
            server.reject.update({('playlistcontrol','cmd:load','track_id:9001,99999,9002'),('playlistcontrol','cmd:add','track_id:99999')})
            try:
                f.suite.cli=m.CLI('127.0.0.1',f.suite.log,server.server_address[1]);f.suite.mac=MAC
                f.suite.speaker=m.Speaker('127.0.0.1',f.suite.log)
                f.suite.before=dict(entries=entries,index='1',time='12.5',repeat='2',shuffle='0',mode='pause',prefs=dict(transitionType='4',transitionDuration='7',transitionSmart='1'))
                f.model.prefs['transitionSmart']='0'
                f.suite.restore_lms()
                self.assertEqual(f.model.urls,f.model.before)
                self.assertEqual(f.model.prefs['transitionSmart'],'1')
                self.assertTrue(any('playlist item 1 99999' in line for line in f.suite.not_restored))
                self.assertEqual(sum(args[1:]==['playlistcontrol','cmd:add','track_id:99999'] for _,_,args in server.requests),2)
            finally:server.close()
        print('PASS queue device restore: a rejected playlist item retries once, remaining IDs and all preferences still restore')


if __name__=='__main__':unittest.main(verbosity=2)
