# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Material preference/asset tests: temporary state and loopback endpoints only."""
import hashlib
import io
import json
import os
from pathlib import Path
import runpy
import select
import signal
import shutil
from unittest.mock import patch
import socketserver
import subprocess
import sys
import tempfile
tempfile.tempdir = (os.environ.get('TMPDIR') or '/var/tmp')
import threading
import time
import unittest
from urllib.parse import quote, unquote
import xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parents[1]
MAC='94:9f:3e:fa:ba:66'
KEY='plugin.material-skin:icon'
INSTALL=runpy.run_path(str(ROOT/'scripts/yeney-install-lms-icon'))['install_asset']

class CLI(socketserver.ThreadingTCPServer):
    allow_reuse_address=True
    daemon_threads=True
    def __init__(self, value='', mode='ok'):
        self.value=value;self.mode=mode;self.requests=[];self.release=threading.Event()
        outer=self
        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                wire=self.rfile.readline().decode().rstrip('\n');outer.requests.append(wire)
                if wire=='players 0 1000':response=wire+' count%3A1 playerid%3A'+quote(MAC,safe='')
                elif wire.startswith(MAC+' playerpref '+KEY+' '):
                    token=wire.split(' ',3)[3]
                    if token=='?':
                        if outer.mode=='timeout':outer.release.wait(5)
                        if outer.mode=='malformed':response='wrong-player playerpref '+KEY+' %7B%7D'
                        else:response=quote(MAC,safe='')+' playerpref '+KEY+' '+quote(outer.value,safe='')
                    else:
                        outer.value=unquote(token)
                        if outer.mode=='unconfirmed':return
                        response=quote(MAC,safe='')+' playerpref '+KEY+' '+quote(outer.value,safe='')
                else:raise AssertionError(wire)
                try:self.wfile.write((response+'\n').encode())
                except (BrokenPipeError,ConnectionResetError):pass
        super().__init__(('127.0.0.1',0),Handler)
        self.thread=threading.Thread(target=self.serve_forever);self.thread.start()
    def close(self):self.release.set();self.shutdown();self.server_close();self.thread.join()

class Client:
    def __init__(self, value='', setting=None, previous=None, mode='ok'):
        self.tmp=tempfile.TemporaryDirectory(prefix='yeney-icon-test-');self.state=Path(self.tmp.name)
        self.file=self.state/('material-icon-'+MAC+'.json')
        if previous is not None:self.file.write_text(previous)
        self.server=CLI(value,mode);env=dict(os.environ,STATE_DIRECTORY=str(self.state))
        if setting is None:env.pop('YENEY_LMS_ICON',None)
        else:env['YENEY_LMS_ICON']=setting
        self.proc=subprocess.Popen([str(ROOT/'lms-icon-test'),str(self.server.server_address[1])],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        self.buffer=b'';self.lines=[]
    def connect(self):
        self.proc.stdin.write(b'connect\n');self.proc.stdin.flush();end=time.monotonic()+5
        while time.monotonic()<end:
            if b'\n' not in self.buffer:
                if not select.select([self.proc.stdout],[],[],.1)[0]:continue
                self.buffer+=os.read(self.proc.stdout.fileno(),4096)
            while b'\n' in self.buffer:
                line,self.buffer=self.buffer.split(b'\n',1);line=line.decode();self.lines.append(line)
                if 'warning invalid' not in line:return line
        raise AssertionError(self.lines)
    def close(self):
        self.proc.stdin.write(b'quit\n');self.proc.stdin.flush();self.proc.wait(3)
        self.proc.stdin.close();self.proc.stdout.close();self.server.close();self.tmp.cleanup()

class Preferences(unittest.TestCase):
    def case(self,**kwargs):
        c=Client(**kwargs);self.addCleanup(c.close);return c
    def test_default_and_wire(self):
        c=self.case();self.assertIn('set svg=sonos',c.connect())
        self.assertEqual(json.loads(c.file.read_text()),{'svg':'sonos'})
        self.assertEqual(c.server.requests[0],'players 0 1000')
        self.assertEqual(c.server.requests[1],MAC+' playerpref '+KEY+' ?')
        self.assertEqual(c.server.requests[2],MAC+' playerpref '+KEY+' %7B%22svg%22%3A%22sonos%22%7D')
        self.assertEqual(len(c.server.requests),3)
        print('PASS LMS icon default: sonos; fresh CLI per command, raw MAC, encoded JSON, confirmed atomic ownership state',flush=True)
    def test_empty_and_invalid(self):
        for setting in ('','bad/name','x'*65):
            with self.subTest(setting=setting):
                c=self.case(setting=setting,previous='{"svg":"sonos"}')
                self.assertIn('disabled',c.connect());self.assertIn('disabled',c.connect())
                self.assertEqual(c.server.requests,[]);self.assertEqual(c.file.read_text(),'{"svg":"sonos"}')
                self.assertEqual(sum('warning invalid' in line for line in c.lines),int(bool(setting)))
        print('PASS LMS icon disabled/invalid: no CLI traffic, one invalid warning, persisted icon untouched',flush=True)
    def test_update_owned(self):
        c=self.case(value='{ "s\\u0076g": "so\\u006eos" }',setting='speaker_2-A',previous='{ "svg" : "sonos" }')
        self.assertIn('set svg=speaker_2-A',c.connect());self.assertEqual(json.loads(c.file.read_text()),{'svg':'speaker_2-A'})
        print('PASS LMS icon semantic JSON ownership: escaped keys/values and whitespace compare equally; configuration changes update confirmed state',flush=True)
    def test_preserve_and_already_correct(self):
        for value,outcome in (('{"svg":"owner"}','owner selection preserved'),('{"svg":"sonos"}','already configured'),('{"svg":"sonos","extra":true}','owner selection preserved'),('["owner",1,null]','owner selection preserved')):
            c=self.case(value=value);self.assertIn(outcome,c.connect());self.assertEqual(len(c.server.requests),2);self.assertFalse(c.file.exists())
        print('PASS LMS icon owner/correct preferences: no redundant writes and no ownership claimed for pre-existing values',flush=True)
    def test_exactly_one_decode(self):
        c=self.case(value='{"svg":"so%6Eos"}')
        self.assertIn('owner selection preserved',c.connect());self.assertFalse(c.file.exists())
        print('PASS LMS icon CLI responses decoded exactly once; inner percent escapes preserved',flush=True)
    def test_failures(self):
        for mode in ('timeout','malformed','unconfirmed'):
            c=self.case(mode=mode);before=time.monotonic();result=c.connect()
            self.assertNotIn('set svg=',result);self.assertFalse(c.file.exists());self.assertLess(time.monotonic()-before,4)
        c=self.case(value='{invalid');self.assertIn('invalid response',c.connect());self.assertFalse(c.file.exists())
        print('PASS LMS icon timeout/malformed/unconfirmed write: bounded nonfatal outcome, no persisted ownership',flush=True)

class Assets(unittest.TestCase):
    def test_svg(self):
        svg=ET.parse(ROOT/'assets/sonos.svg').getroot()
        self.assertEqual(svg.attrib,dict(width='24',height='24',viewBox='0 0 24 24'))
        self.assertEqual(len(svg),1);path=svg[0]
        self.assertEqual(path.tag,'{http://www.w3.org/2000/svg}path')
        self.assertEqual(path.attrib['fill'],'#000');self.assertEqual(path.attrib['fill-rule'],'evenodd')
        self.assertEqual(path.attrib['transform'],'translate(2.5 2.5) scale(.791666667)')
        self.assertEqual(hashlib.sha256(''.join(path.attrib['d'].split()).encode()).hexdigest(),'76b9d61d1600b8eb29ffc2c32ec55cfa3551d00b758adbb12a9f8943e99191d6')
        print('PASS bundled SVG: authoritative geometry hash, 24x24 dimensions/viewBox, path-only #000 even-odd cut-outs',flush=True)
    def test_install(self):
        with tempfile.TemporaryDirectory() as d:
            prefs=Path(d);asset=ROOT/'assets/sonos.svg';messages=[]
            INSTALL(prefs,asset,messages.append);target=prefs/'material-skin/images/sonos.svg';inode=target.stat().st_ino
            INSTALL(prefs,asset,messages.append);self.assertEqual(target.stat().st_ino,inode)
            old=b'<svg>previous managed version</svg>';target.write_bytes(old)
            (target.parent/'.yeney-sonos-svg.json').write_text(json.dumps(dict(installer='yeney',sha256=hashlib.sha256(old).hexdigest())))
            INSTALL(prefs,asset,messages.append);self.assertEqual(target.read_bytes(),asset.read_bytes());self.assertEqual(target.stat().st_mode&0o777,0o644)
            target.write_bytes(b'owner')
            with self.assertRaisesRegex(ValueError,'conflict'):INSTALL(prefs,asset,messages.append)
            self.assertEqual(target.read_bytes(),b'owner')
        print('PASS LMS-side asset install: temporary prefs, idempotent, atomic managed update, readable permissions, owner conflicts preserved',flush=True)
    def test_owner_identical_and_symlink(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);images=p/'material-skin/images';images.mkdir(parents=True);asset=ROOT/'assets/sonos.svg'
            target=images/'sonos.svg';target.write_bytes(asset.read_bytes());INSTALL(p,asset,lambda s:None)
            self.assertFalse((images/'.yeney-sonos-svg.json').exists())
            target.unlink();target.symlink_to(asset)
            with self.assertRaisesRegex(ValueError,'symlink'):INSTALL(p,asset,lambda s:None)
    def test_workflow_option(self):
        from installer_test import installer,Commands,mock_lms
        with tempfile.TemporaryDirectory() as d:
            prefs=Path(d)/'prefs';prefs.mkdir();config=Path(d)/'config';units=Path(d)/'units';out=io.StringIO()
            installer.install(ROOT,config,units,['--yes','--lms-prefs-dir='+str(prefs)],Commands(),out,io.StringIO(),lms_check=mock_lms)
            self.assertEqual((config/'lms-prefs-dir').read_text().strip(),str(prefs))
            target=prefs/'material-skin/images/sonos.svg';self.assertTrue(target.exists())
            installer.install(ROOT,config,units,['--yes'],Commands(),out,io.StringIO(),lms_check=mock_lms)
            self.assertIn('already present',out.getvalue())
        print('PASS normal installer workflow: explicit LMS prefs path saved and reused on later installs',flush=True)
    def test_packaging(self):
        make=(ROOT/'Makefile').read_text();script=(ROOT/'scripts/install_devices.py').read_text()
        for text in ('install -m 0644 assets/sonos.svg /usr/local/share/yeney/sonos.svg','install -m 0755 scripts/yeney-install-lms-icon /usr/local/bin/yeney-install-lms-icon','--lms-prefs-dir='):self.assertIn(text,make)
        self.assertIn("prefs_record = config_dir / 'lms-prefs-dir'",script)
        with tempfile.TemporaryDirectory() as d:
            r=subprocess.run([sys.executable,str(ROOT/'scripts/yeney-install-lms-icon'),'--lms-prefs-dir',d],capture_output=True,text=True)
            self.assertEqual(r.returncode,0,r.stderr);self.assertTrue((Path(d)/'material-skin/images/sonos.svg').exists())
            installed=Path(d)/'usr/local';(installed/'bin').mkdir(parents=True);(installed/'share/yeney').mkdir(parents=True)
            shutil.copyfile(ROOT/'scripts/yeney-install-lms-icon',installed/'bin/yeney-install-lms-icon')
            shutil.copyfile(ROOT/'assets/sonos.svg',installed/'share/yeney/sonos.svg')
            prefs=Path(d)/'separate-host-prefs';prefs.mkdir()
            r=subprocess.run([sys.executable,str(installed/'bin/yeney-install-lms-icon'),'--lms-prefs-dir',str(prefs)],capture_output=True,text=True)
            self.assertEqual(r.returncode,0,r.stderr);self.assertTrue((prefs/'material-skin/images/sonos.svg').exists())
        print('PASS icon packaging: bundled asset and LMS-side command installed by make; explicit prefs option persisted, no guessed directory',flush=True)

class Runtime(unittest.TestCase):
    def test_playback_survives_icon_failure(self):
        import queue_binary_test as fixture
        original=fixture.CLI
        for failure in ('timeout','malformed'):
            release=threading.Event();requests=[]
            class IconCLI(original):
                def __init__(self,owner):
                    super().__init__(owner)
                    old=self.RequestHandlerClass
                    class Handler(old):
                        def handle(self):
                            first=self.rfile.readline();wire=first.decode().strip()
                            if wire=='players 0 1000':
                                requests.append(wire)
                                self.wfile.write(('players 0 1000 playerid:'+owner.mac+' count:1\n').encode());self.wfile.flush()
                            elif 'playerpref '+KEY in wire:
                                requests.append(wire)
                                if failure=='timeout':release.wait(5)
                                else:
                                    self.wfile.write(b'malformed reply\n');self.wfile.flush()
                            else:
                                stream=self.rfile;self.rfile=io.BytesIO(first)
                                try:super().handle()
                                finally:self.rfile=stream
                    self.RequestHandlerClass=Handler
            env=dict(os.environ);env.pop('YENEY_LMS_ICON',None)
            with patch.dict(os.environ,env,clear=True),patch.object(fixture,'CLI',IconCLI):
                h=fixture.Binary(rates=(44100,),seconds=8)
                try:
                    h.until(lambda:len(h.packets('STMs'))==1,3)
                    h.until(lambda:'LMS icon: preference query failed' in (h.base/'bridge.log').read_text(),5)
                    self.assertEqual(len(h.sonos.actions('Play')),1);self.assertEqual(len(h.sonos.actions('AddURIToQueue')),1)
                    self.assertTrue(requests[0].startswith('players '));self.assertEqual(len([p for p in h.lms.packets if p.get('op')=='HELO']),1)
                    self.assertFalse(list(h.base.glob('material-icon-*.json')))
                finally:release.set();h.close()
        print('PASS real yeney icon failures: timed-out/malformed CLI never blocks queue audio, one enqueue/Play/HELO, no persisted ownership',flush=True)

class Connection(unittest.TestCase):
    def test_ready(self):
        sys.path.insert(0,str(ROOT/'third_party/yeney-core/tests'))
        from fake_lms import LMS
        lms=LMS();p=subprocess.Popen([str(ROOT/'lms-icon-connection-test'),str(lms.listener.getsockname()[1])],stdout=subprocess.PIPE)
        try:
            for connection in range(2):
                lms.accept();self.assertFalse(select.select([p.stdout],[],[],.1)[0])
                lms.send('aude',bytes([0,1]));self.assertTrue(select.select([p.stdout],[],[],2)[0]);self.assertEqual(p.stdout.readline(),b'READY 127.0.0.1\n')
                lms.send('aude',bytes([0,1]));self.assertFalse(select.select([p.stdout],[],[],.1)[0])
                lms.connection.close()
            print('PASS LMS registration observer: once per genuine connection, only after HELO reply, resolved peer, no capability resend',flush=True)
        finally:p.terminate();p.wait(3);p.stdout.close();lms.close()

if __name__=='__main__':unittest.main(verbosity=2)
