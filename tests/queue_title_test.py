"""Plain queue titles, actual SOAP bytes and direct-process status/log evidence."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import argparse
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from queue_binary_test import Binary
from queue_sonos_fake import Coordinator, Handler

ROOT=Path(__file__).resolve().parents[1]
GOLDEN=ROOT/'tests/fixtures/queue-add-uri.xml'

class Wire(Handler):
    def do_POST(self):
        body=self.rfile.read(int(self.headers['Content-Length']))
        if '#AddURIToQueue' in self.headers['SOAPAction']: self.server.enqueue_wire.append(body)
        self.rfile=io.BytesIO(body)
        super().do_POST()

def build(directory,baseline=None):
    includes=[];sources=[]
    if baseline:
        old=directory/'old/upnp';old.mkdir(parents=True)
        for name in ('queue_metadata.h','queue_metadata.cpp'):
            (old/name).write_bytes(subprocess.check_output(['git','show',baseline+':upnp/'+name],cwd=ROOT))
        includes=['-I'+str(old.parent)]
        sources=[str(old/'queue_metadata.cpp')]
    binary=directory/('before' if baseline else 'after')
    subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra',*includes,'-I.','-Iupnp',
                    'tests/queue_title_wire.cpp',*sources,'libqueuecontroller.a',
                    'upnp/http.o','upnp/xml.o','upnp/soap.o','upnp/discovery.o','-pthread','-o',str(binary)],cwd=ROOT,check=True)
    return binary

def request(binary):
    speaker=Coordinator(handler=Wire)
    speaker.enqueue_wire=[];speaker.crossfade=False;speaker.verified_save=True
    speaker.expected_title_prefix='';speaker.expected_albums=['love (Deluxe Edition)']
    try:
        subprocess.run([str(binary),str(speaker.server_port)],cwd=ROOT,check=True,timeout=10,
                       env={**os.environ,"YENEY_TITLE_FORMAT":"artist-title"})
        assert len(speaker.enqueue_wire)==1
        return speaker.enqueue_wire[0]
    finally:speaker.close()

def integration():
    old=os.environ.get('YENEY_TITLE_FORMAT')
    try:
        for setting in (None,'artist-title','title'):
            if setting is None:os.environ.pop('YENEY_TITLE_FORMAT',None)
            else:os.environ['YENEY_TITLE_FORMAT']=setting
            # The strict fake validates plain title separately; use the standard
            # prefix so the invalid control travels through the real CLI path.
            plain="Title & <live> "+chr(34)+"mix"+chr(34)+chr(10)
            host=Binary(rates=(44100,),titles=[chr(22)+plain])
            try:
                host.state(1);host.until(lambda:len(host.packets('STMs'))==1)
                expected=host.items()[0]['metadata']['title']
                assert expected==plain
                path=host.base/('stream-pacing-'+b'Study'.hex()+'.json')
                host.until(lambda:json.loads(path.read_text())['playback'].get('title')==expected)
                log=(host.base/'bridge.log').read_text()
                titles=re.findall(r'queue item occurrence=\d+ title=(.*)',log)
                assert len(titles)==1 and json.loads(titles[0])==expected,log
                print('PASS queue title direct process: setting='+str(setting)+'; sanitised DIDL/log/status title equal, no systemd dependency',flush=True)
            finally:host.close()
    finally:
        if old is None:os.environ.pop('YENEY_TITLE_FORMAT',None)
        else:os.environ['YENEY_TITLE_FORMAT']=old

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline');parser.add_argument('--write-fixture',action='store_true')
    parser.add_argument('--wire-only',action='store_true')
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='yeney-title-wire-') as temp:
        directory=Path(temp);after=request(build(directory))
        if args.write_fixture:GOLDEN.write_bytes(after)
        assert after==GOLDEN.read_bytes(),'AddURIToQueue wire fixture changed'
        print('PASS queue AddURIToQueue: complete request body matches fixed plain-title wire fixture',flush=True)
        if args.baseline:
            before=request(build(directory,args.baseline))
            old='&lt;dc:title&gt;Michael Bublé - When I Fall In Love &amp;amp; &amp;lt;live&amp;gt;&lt;/dc:title&gt;'.encode()
            new='&lt;dc:title&gt;When I Fall In Love &amp;amp; &amp;lt;live&amp;gt;&lt;/dc:title&gt;'.encode()
            assert before.count(old)==1 and before.replace(old,new,1)==after
            (Path('/tmp')/'yeney-title-before-add-uri.xml').write_bytes(before)
            print('PASS queue SOAP baseline '+args.baseline+': only dc:title removes '+repr('Michael Bublé - ')+' ('+str(len(before)-len(after))+' UTF-8 bytes); every other request-body byte identical',flush=True)
    if not args.wire_only:integration()
