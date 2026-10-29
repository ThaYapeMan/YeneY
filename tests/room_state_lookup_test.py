"""Installed-layout regression: real yeney, rendered units, loopback speaker/LMS."""
import io
import json
import os
from pathlib import Path
import runpy
import socket
import subprocess
import tempfile
tempfile.tempdir = (os.environ.get('TMPDIR') or '/var/tmp')
import time
import unittest
from unittest.mock import Mock
from xml.sax.saxutils import quoteattr
from installer_test import installer, Commands, mock_lms, ROOT
from queue_binary_test import Device
from queue_sonos_fake import Coordinator

HELPER = runpy.run_path(str(ROOT/'scripts/yeney_room_state.py'))
PACING = runpy.run_path(str(ROOT/'scripts/yeney-stream-pacing-suite'))
DEVICE = runpy.run_path(str(ROOT/'scripts/yeney-queue-device-suite'))

class Speaker(Device):
    def action(self, service, action, args):
        if action == 'GetZoneGroupState':
            return {'ZoneGroupState': '<ZoneGroupState><ZoneGroups><ZoneGroup Coordinator="RINCON_TEST" ID="test"><ZoneGroupMember UUID="RINCON_TEST" ZoneName='+quoteattr(self.server.room)+' Location="http://127.0.0.1:1400/description.xml"/></ZoneGroup></ZoneGroups></ZoneGroupState>'}, None
        return super().action(service, action, args)

class LookupComplete(Exception): pass

class LookupTests(unittest.TestCase):
    def test_fallback_and_fail_closed(self):
        self.assertEqual(HELPER['state_directory'](''), Path('/var/lib/yeney'))
        self.assertIn('install -m 0644 scripts/yeney_room_state.py /usr/local/bin/yeney_room_state.py', (ROOT/'Makefile').read_text())
        for value in ('yeney/a yeney/b', '../escape', '/absolute', r'yeney/room\x20name', '"quoted"'):
            with self.assertRaises(ValueError): HELPER['state_directory'](value)
        def error(*args): raise OSError('systemd unavailable')
        with self.assertRaises(OSError): HELPER['room_state']('Study', error)
        print('PASS shared room lookup: only empty StateDirectory falls back; command failures and ambiguous paths fail closed')

    def test_installed_layout_real_binary_both_modes(self):
        sonos = Coordinator(('127.0.0.1',1400), Speaker)
        try:
            with tempfile.TemporaryDirectory(prefix='yeney-room-layout-') as tmp:
                base=Path(tmp); legacy=base/'var/lib/yeney'; legacy.mkdir(parents=True)
                config=base/'etc/yeney'; config.mkdir(parents=True); units=base/'etc/systemd/system'
                names=['Study','Sonos Port','Küche']
                (config/'config').write_text('LMS_SERVER=host\n'+''.join('room.'+n+'=yes\n' for n in names))
                runner=Commands('\n'.join(names)+'\n')
                installer.install(ROOT, config, units, [], runner, io.StringIO(), io.StringIO(), lms_check=mock_lms, state_root=legacy)
                for room in names:
                    def backend_run(*args):
                        if args[0]=='systemd-escape': return subprocess.check_output(args,text=True).strip()
                        if args[0:2] == ('systemctl','is-active'): raise LookupComplete()
                        self.assertEqual(args[0:2], ('systemctl','show'))
                        if 'MainPID' in args: return str(proc.pid)
                        self.assertIn('StateDirectory',args)
                        text=(units/(args[2]+'.d')/'zz-yeney-state.conf').read_text()
                        return text.split('StateDirectory=')[-1].strip()
                    unit, directory=HELPER['room_state'](room,backend_run,state_base=base/'var/lib',legacy_root=legacy)
                    self.assertNotEqual(directory.name,room); directory.mkdir(exist_ok=True)
                    filename='stream-pacing-'+room.encode().hex()+'.json'
                    (legacy/filename).write_text('STALE GLOBAL FILE MUST NEVER BE READ')
                    for mode in ('queue','stream'):
                        sonos.room=room
                        with socket.socket() as lms, (base/'bridge.log').open('w') as log:
                            lms.bind(('127.0.0.1',0)); lms.listen(); lms.settimeout(6)
                            env=dict(os.environ,STATE_DIRECTORY=str(directory),YENEY_PLAYBACK_MODE=mode,YENEY_LMS_ICON='',YENEY_STREAM_PACING='sonos',YENEY_TIMING_PROBE='0',YENEY_TIMING_PUBLISH='0')
                            proc=subprocess.Popen([str(ROOT/'yeney'),'--room='+room,'--ip=127.0.0.1','--server=127.0.0.1:'+str(lms.getsockname()[1])],env=env,stdout=log,stderr=log)
                            connection=None
                            try:
                                connection,_=lms.accept()
                                deadline=time.monotonic()+6
                                while time.monotonic()<deadline:
                                    if (directory/filename).exists():
                                        snap=json.loads((directory/filename).read_text())
                                        if snap['pid']==proc.pid: break
                                    time.sleep(.02)
                                else: self.fail((base/'bridge.log').read_text())
                                backend=Mock(); backend.run=backend_run; backend.now=time.monotonic
                                for module in (PACING,DEVICE):
                                    suite=module['Suite'](room,'127.0.0.1',backend=backend,cli=Mock(),state_root=legacy,state_base=base/'var/lib',output=base/'out')
                                    if module is DEVICE: suite.lock_root=base/'suite-locks'
                                    try:
                                        # Execute each installed tool's actual startup lookup, stopping
                                        # before it can mutate a service or contact an LMS player.
                                        with self.assertRaises(LookupComplete): suite.setup()
                                        self.assertEqual(suite.state_root,directory)
                                        suite.pid=proc.pid
                                        if module is DEVICE:
                                            suite.current=None if mode=='queue' else 'stream-reference'
                                            state=suite.diagnostic()
                                            self.assertEqual(state['active'],mode)
                                        else:
                                            suite.isolated=lambda:None; suite.query=lambda *a:'play'
                                            self.assertEqual(suite.sample()['pid'],proc.pid)
                                    finally:
                                        suite.logfile.close()
                                        for held in getattr(suite,'locks',[]): held.close()
                                self.assertEqual((legacy/filename).read_text(),'STALE GLOBAL FILE MUST NEVER BE READ')
                                print(f'PASS installed layout real yeney {room!r} {mode}: rendered StateDirectory, device readiness and pacing lookup, stale global decoy ignored',flush=True)
                            finally:
                                proc.terminate()
                                try: proc.wait(timeout=10)
                                except subprocess.TimeoutExpired: proc.kill(); proc.wait(); raise
                                if connection: connection.close()
        finally: sonos.close()

    def test_drift_persistence_follows_state_directory(self):
        with tempfile.TemporaryDirectory(prefix='yeney-drift-room-') as tmp:
            root=Path(tmp); cpp=root/'test.cpp'; exe=root/'test'
            cpp.write_text('#include "timing_drift_state.h"\nint main(){timing_probe::DriftState s;s.speaker("test");return s.save({1,1,time(nullptr)})?0:1;}\n')
            subprocess.run(['g++','-std=c++17','-I.',str(cpp),'-o',str(exe)],cwd=ROOT,check=True)
            subprocess.run([str(exe)],env=dict(os.environ,STATE_DIRECTORY=str(root)),check=True)
            self.assertTrue((root/'drift-74657374.state').is_file())
        print('PASS drift persistence: default timing state uses STATE_DIRECTORY')

    def test_stale_cleanup_is_scoped(self):
        with tempfile.TemporaryDirectory(prefix='yeney-state-cleanup-') as tmp:
            root=Path(tmp)/'yeney'; root.mkdir(); own=root/'safe-hash'; own.mkdir()
            status=root/('stream-pacing-'+b'Study'.hex()+'.json'); status.write_text('old')
            link=root/'owner-alias'; link.symlink_to(own)
            filelink=root/'owner-status'; filelink.symlink_to(own/'status.json')
            keep=root/'drift.state'; keep.write_text('keep')
            other=root/'foreign-link'; other.symlink_to(Path(tmp)/'elsewhere')
            foreign=root/('stream-pacing-'+b'Other'.hex()+'.json'); foreign.write_text('other')
            def run(args,**kwargs):
                return subprocess.CompletedProcess(args,0,'yeney@Study.service' if args[0]=='systemd-escape' else 'yeney/safe-hash','')
            installer.clean_legacy_status(root,['Study'],run,io.StringIO())
            self.assertFalse(status.exists()); self.assertFalse(link.is_symlink()); self.assertFalse(filelink.is_symlink())
            self.assertEqual(keep.read_text(),'keep'); self.assertTrue(other.is_symlink()); self.assertEqual(foreign.read_text(),'other')
            status.write_text('manual legacy room')
            def no_state(args,**kwargs):
                return subprocess.CompletedProcess(args,0,'yeney@Study.service' if args[0]=='systemd-escape' else '', '')
            installer.clean_legacy_status(root,['Study'],no_state,io.StringIO())
            self.assertEqual(status.read_text(),'manual legacy room')
        print('PASS installer stale cleanup: matching status and room-target symlinks removed; other rooms, drift files and unrelated symlinks preserved')

if __name__=='__main__': unittest.main(verbosity=2)
