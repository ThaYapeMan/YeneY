"""Owner runner transaction tests: fake commands/CLI, temporary paths only."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import contextlib
import importlib.machinery
import importlib.util
import io
import json
from pathlib import Path
import signal
import socket
import tarfile
import tempfile
import threading
import unittest
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader('pacing_suite',str(ROOT/'scripts/yeney-stream-pacing-suite'))
spec = importlib.util.spec_from_loader(loader.name,loader)
module = importlib.util.module_from_spec(spec);loader.exec_module(module)
MAC = 'aa:bb:cc:dd:ee:ff'


class FakeCLI:
    def __init__(self):
        self.calls=[]; self.synced=False; self.empty=False; self.favorite=True
        self.state=dict(index='1',time='12.5',repeat='0',mode='pause',tracks='3')

    def call(self,*args):
        self.calls.append(args)
        if args[0]=='players':
            return ['players','0','999','playerindex:0','playerid:'+MAC,'name:Study (Sonos)','connected:1']
        if args[0]=='syncgroups':
            return ['syncgroups','sync_members:'+MAC+(',11:22:33:44:55:66' if self.synced else '')]
        if args[0]=='favorites':
            assert args[-1]=='want_url:1'
            return ['id:1','hasitems:0','url:http://127.0.0.1/radio'] if self.favorite else []
        if args[0]=='playlists': return ['id:12']
        assert args[0]==MAC, args
        cmd=args[1:]
        if cmd[-1]=='?':
            return list(args[:-1])+[self.state[cmd[-2]]]
        if cmd[0]=='playlist':
            key=cmd[1]
            if key in ('repeat','index'): self.state[key]=cmd[2]
            elif key=='play': self.state.update(tracks='1',mode='play')
            elif key=='clear': self.state.update(tracks='0',index='0',time='0',mode='stop')
            else: raise AssertionError(args)
        elif cmd[0]=='playlistcontrol': self.state.update(tracks='10',mode='play')
        elif cmd[0]=='time': self.state['time']=cmd[1]
        elif cmd[0]=='pause': self.state['mode']='pause'
        elif cmd[0] in ('play','stop'): self.state['mode']=cmd[0]
        else: raise AssertionError(args)
        return list(args)


class FakeBackend:
    def __init__(self,cli):
        self.cli=cli;self.calls=[];self.time=1.;self.pid=10;self.grouped=False;self.restarts=0
        self.fail_restart=False;self.interrupt=False;self.fault=None;self.suite=None;self.midgroup=False

    def run(self,*args):
        self.calls.append(args)
        if args[0]=='systemd-escape': return 'yeney@Study.service'
        if args[0].endswith('/yeney'):
            return 'Study\tPort\t127.0.0.1\tStudy\t'+('Study,Kitchen' if self.grouped else 'Study')
        if args[0]=='journalctl': return 'fake Study journal'
        assert args[0]=='systemctl',args
        if args[1]=='show':
            if 'MainPID' in args: return str(self.pid)
            return 'YENEY_EXISTING=kept'+(' YENEY_STREAM_PACING=sonos' if self.suite.dropin and self.suite.dropin.exists() else '')
        if args[1]=='restart':
            assert args[-1]=='yeney@Study.service'
            self.restarts+=1;self.pid+=1
            if self.fail_restart and self.restarts==1: raise module.SuiteError('injected restart failure')
        else: assert args[1] in ('is-active','daemon-reload','stop'),args
        return ''

    def now(self): return self.time
    def sleep(self,seconds):
        self.time+=seconds
        if self.midgroup and self.time>=30: self.grouped=True
        if self.interrupt and self.time>=30:
            self.interrupt=False; raise KeyboardInterrupt()
        s=dict(pid=self.pid,monotonic_ms=int(self.time*1000),mode='sonos',start_lead_ms=1000,
               overwrites=0,dropped=0,socket_backpressure_ms=self.time,socket_backpressure_calls=1,
               encoded_ms=self.time*1000+1500,audible_ms=self.time*1000,lead_ms=1500,
               valid=True,connection_id=1,locked=True)
        if self.fault=='drop' and self.time>=20: s['dropped']=1
        if self.fault=='lead' and self.time>=130: s['lead_ms']=2501
        if self.fault=='pid': s['pid']=999
        if self.fault=='wall': s['mode']='wall'
        if self.fault=='unlock' and self.time>=130: s['locked']=False
        if self.fault!='missing':
            path=self.suite.state_root/('stream-pacing-'+self.suite.room.encode().hex()+'.json')
            path.write_text(json.dumps(s))


class SuiteTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='pacing-owner-test-');self.root=Path(self.temp.name)
        self.cli=FakeCLI();self.backend=FakeBackend(self.cli)
        for directory in ('units','state','out'): (self.root/directory).mkdir()
        self.suite=module.Suite('Study','127.0.0.1',.04,backend=self.backend,cli=self.cli,
                               unit_root=self.root/'units',state_root=self.root/'state',output=self.root/'out')
        self.backend.suite=self.suite
        self.handlers={s:signal.getsignal(s) for s in (signal.SIGINT,signal.SIGTERM)}

    def tearDown(self):
        for s,h in self.handlers.items(): signal.signal(s,h)
        if not self.suite.logfile.closed: self.suite.logfile.close()
        self.temp.cleanup()

    def execute(self):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            result=self.suite.execute()
        archive=self.root/'out/yeney-stream-pacing-latest.tar.gz'
        with tarfile.open(archive) as tar:
            verdict=tar.extractfile('yeney-stream-pacing/result.txt').read().decode().strip()
        self.assertEqual(output.getvalue().splitlines()[-1],verdict)
        return result,verdict

    def restored(self):
        self.assertFalse(self.suite.dropin and self.suite.dropin.exists())
        self.assertEqual(self.cli.state,dict(index='1',time='12.5',repeat='0',mode='pause',tracks='3'))
        for args in self.backend.calls:
            if args[0]=='systemctl' and args[1] not in ('daemon-reload',):
                self.assertIn('yeney@Study.service',args)
        self.assertEqual(self.backend.restarts,2)

    def test_success(self):
        self.assertEqual(self.execute(),(0,'PACING OK'));self.restored()
        qualified=[s for s in self.suite.samples if s['qualified']]
        self.assertTrue(qualified)
        stamps=[s['elapsed_s'] for s in self.suite.samples]
        self.assertTrue(all(b-a==10 for a,b in zip(stamps[1:],stamps[2:])))
        print('PASS owner pacing suite: ten-second samples, qualified lead/counters, room-only override/restarts, LMS state restored, verdict archived')

    def test_sonos_group_refused(self):
        self.backend.grouped=True
        code,verdict=self.execute();self.assertEqual(code,1);self.assertIn('grouped',verdict)
        self.assertEqual(self.backend.restarts,0);self.assertFalse(self.suite.override)
        self.assertFalse(any(c[0]==MAC and c[-1]!='?' for c in self.cli.calls))

    def test_lms_group_refused(self):
        self.cli.synced=True
        code,verdict=self.execute();self.assertEqual(code,1);self.assertIn('synced',verdict)
        self.assertEqual(self.backend.restarts,0);self.assertFalse(self.suite.override)

    def test_group_appears_mid_run(self):
        self.backend.midgroup=True
        code,verdict=self.execute();self.assertEqual(code,1)
        self.assertIn('safe restart deferred',verdict)
        self.assertFalse(self.suite.dropin.exists())
        self.assertEqual(self.backend.restarts,1)
        self.assertIn(('systemctl','stop','yeney@Study.service'),self.backend.calls)
        print('PASS owner pacing suite: a newly grouped room is not restarted into group playback; target-only config cleanup and explicit deferred restoration')

    def test_install_and_documented_full_path(self):
        path='/usr/local/bin/yeney-stream-pacing-suite'
        self.assertIn('install -m 0755 scripts/yeney-stream-pacing-suite '+path,(ROOT/'Makefile').read_text())
        docs=(ROOT/'DEVICE-VERIFICATION.md').read_text()
        self.assertIn(path+' --room Study --server 192.168.178.23 --hours 2',docs)
        self.assertIn('pct pull 113 /tmp/yeney-stream-pacing-latest.tar.gz /tmp/yeney-stream-pacing-latest.tar.gz',docs)
        self.suite.logfile.close()

    def test_existing_override_preserved(self):
        path=self.root/'units/yeney@Study.service.d/pacing-suite.conf'
        path.parent.mkdir();path.write_text('owner content\n')
        code,_=self.execute();self.assertEqual(code,1);self.assertEqual(path.read_text(),'owner content\n')
        self.assertEqual(self.backend.restarts,0)

    def test_ctrl_c_restores(self):
        self.backend.interrupt=True
        code,verdict=self.execute();self.assertEqual(code,1);self.assertIn('interrupted',verdict);self.restored()
        print('PASS owner pacing suite: Ctrl-C/error cleanup preserves other rooms and archives PACING FAILED')

    def test_archive_error_still_fails_after_restoration(self):
        with patch.object(module.tarfile,'open',side_effect=OSError('injected archive failure')):
            with contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(self.suite.execute(),1)
        self.assertIn('PACING FAILED:',output.getvalue())
        self.assertIn('archive write failed',output.getvalue())
        self.restored()

    def test_partial_restart_failure_restores(self):
        self.backend.fail_restart=True
        code,verdict=self.execute();self.assertEqual(code,1);self.assertIn('restart failure',verdict);self.restored()

    def test_faults(self):
        # Separate temporary transactions for every post-start failure.
        for fault in ('drop','lead','pid','wall','unlock','missing'):
            with self.subTest(fault=fault):
                cli=FakeCLI();backend=FakeBackend(cli);backend.fault=fault
                suite=module.Suite('Study','127.0.0.1',.04,backend=backend,cli=cli,
                                  unit_root=self.root/'units',state_root=self.root/'state',output=self.root/'out')
                backend.suite=suite
                for p in (self.root/'state').glob('*.json'): p.unlink()
                with contextlib.redirect_stdout(io.StringIO()): self.assertEqual(suite.execute(),1)
                self.assertFalse(suite.dropin.exists());self.assertEqual(backend.restarts,2)
                self.assertEqual(cli.state['mode'],'pause')
        print('PASS owner pacing suite: loss, lead violation, stale/wrong PID, wall mode, lost lock and missing diagnostics fail with restoration')

    def test_empty_playlist_radio_and_saved_playlist(self):
        for favorite in (True,False):
            self.cli.state.update(index='0',time='0',mode='stop',tracks='0');self.cli.favorite=favorite
            code,verdict=self.execute();self.assertEqual((code,verdict),(0,'PACING OK'))
            self.assertEqual(self.cli.state['tracks'],'0');self.assertEqual(self.cli.state['mode'],'stop')
            # A second transaction needs fresh diagnostics for its new PID.
            self.suite=module.Suite('Study','127.0.0.1',.04,backend=self.backend,cli=self.cli,
                                   unit_root=self.root/'units',state_root=self.root/'state',output=self.root/'out')
            self.backend.suite=self.suite
            for p in (self.root/'state').glob('*.json'):p.unlink()
        self.suite.logfile.close()

    def test_loopback_cli_raw_mac(self):
        listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(1)
        requests=[]
        def peer():
            with listener.accept()[0] as conn:
                request=conn.recv(4096);requests.append(request)
                conn.sendall((MAC+' playlist index 2\n').encode())
        worker=threading.Thread(target=peer);worker.start()
        connect=socket.create_connection
        with patch.object(module.socket,'create_connection',lambda *_a,**_k:connect(listener.getsockname(),timeout=2)):
            self.assertEqual(module.CLI('127.0.0.1',lambda _:None).call(MAC,'playlist','index','?')[-1],'2')
        worker.join();listener.close()
        self.assertEqual(requests,[(MAC+' playlist index ?\n').encode()])
        print('PASS owner pacing CLI: loopback-only protocol, raw MAC and verified response echo')


if __name__=='__main__': unittest.main()
