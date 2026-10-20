"""Remote artwork through real stream metadata/transport and queue snapshot DIDL.

Only loopback endpoints. The podcast CLI fields are the owner's supplied record;
its audio URL ellipses are retained verbatim, never fetched.
"""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import os
from pathlib import Path
import socketserver
import subprocess
import sys
import tempfile
import threading
from urllib.parse import quote, unquote
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
namespace = {'__file__': str(ROOT/'tests/player_engine_test.py')}
exec((ROOT/'tests/player_engine_test.py').read_text().split('\nwith tempfile.TemporaryDirectory')[0], namespace)
Run, pcm = namespace['Run'], namespace['pcm']
os.environ['YENEY_TITLE_FORMAT'] = 'artist-title'
BASE = 'http://127.0.0.1:9000'
podcast = (ROOT/'tests/fixtures/remote-podcast-cli.txt').read_text().strip()
podcast_art = ('/imageproxy/https%3A%2F%2Fwww.omnycontent.com%2Fd%2Fplaylist%2F'
               'fb07d9e2-a1c2-43d8-90e1-b33701153e67%2F54921250-344a-4ad4-a089-b34400e01852%2F'
               '170e283e-c1e7-4c26-b8cf-b34400e0185f%2Fimage.jpg%3Ft%3D1756128960%26size%3DLarge/image.jpg')

def fields(**values):
    return ' '.join(quote(k+':'+v, safe='') for k, v in values.items())

common = dict(title='Title & <one>', artist='Artist & two', album='Album <three>')
absolute = 'https://images.example/art%20one.jpg?size=Large&v=2'
cases = [
    ('podcast', podcast, BASE+podcast_art),
    ('local', fields(id='123', remote='0', url='file:///local.flac', **common), BASE+'/music/123/cover.jpg'),
    ('absolute', fields(id='-2', remote='1', artwork_url=absolute, **common),
     BASE+'/imageproxy/'+quote(absolute, safe='')+'/image.jpg'),
    ('radio', fields(id='-3', remote='1', artwork_url='plugins/Radio/icon.png?x=1&y=2', **common),
     BASE+'/plugins/Radio/icon.png?x=1&y=2'),
    ('missing', fields(id='-4', remote='1', **common), ''),
    ('remote-positive-id', fields(id='999', remote='1', artwork_url='/remote.png', **common), BASE+'/remote.png'),
    ('url-fallback', fields(id='temporary', url='radio://station', artwork_url='/station.png', **common), BASE+'/station.png'),
    ('local-zero', fields(id='0', remote='0', artwork_url='/ignored.png', **common), BASE+'/music/0/cover.jpg'),
]

class CLI(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def handle_error(self, request, client_address):
        self.errors.append(sys.exc_info()[1])

class Client(socketserver.StreamRequestHandler):
    def handle(self):
        for wire in self.rfile:
            line = wire.decode().strip()
            if line == 'listen 1':
                self.wfile.write(b'listen 1\n');self.wfile.flush();continue
            mac, command = line.split(' ', 1)
            assert mac.count(':') == 5 and '%' not in mac, line
            if command in ('pause 1', 'stop'):
                self.wfile.write((line+'\n').encode())
                self.wfile.flush()
                continue
            assert command == 'status - 1 tags:aAlKuj', line
            self.server.commands.append(command)
            self.wfile.write((line+' '+self.server.record+'\n').encode())
            self.wfile.flush()

def check(didl, record, artwork):
    root = ET.fromstring(didl)
    item = next(iter(root))
    values = {n.tag.rsplit('}', 1)[-1]: n.text or '' for n in item}
    expected = dict(unquote(t).split(':', 1) for t in record.split())
    title = expected['title']
    if expected.get('artist'): title = expected['artist']+' - '+title
    assert values['title'] == title, values
    assert values.get('creator', '') == expected.get('artist', ''), values
    assert values.get('album', '') == expected.get('album', ''), values
    if artwork:
        assert values['albumArtURI'] == artwork, (values, artwork)
        assert '<upnp:albumArtURI>'+namespace['escape'](artwork)+'</upnp:albumArtURI>' in didl, didl
    else:
        assert 'albumArtURI' not in values and 'albumArtURI' not in didl, didl

with tempfile.TemporaryDirectory(prefix='yeney-artwork-') as directory:
    directory = Path(directory)
    fixture = directory/'queue-artwork'
    subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-I.',
                    'tests/artwork_queue_fixture.cpp', 'upnp/queue_metadata.cpp', 'upnp/xml.cpp',
                    '-o', str(fixture)], cwd=ROOT, check=True)
    for name, record, artwork in cases:
        didl = subprocess.check_output([str(fixture)], input=record+'\n', text=True)
        check(didl, record, artwork)
        print('PASS queue artwork: '+name, flush=True)
        with CLI(('127.0.0.1', 9090), Client) as cli:
            cli.record, cli.commands, cli.errors = record, [], []
            worker = threading.Thread(target=cli.serve_forever, daemon=True)
            worker.start()
            run = None
            try:
                run = Run(name, directory)
                body, _ = pcm(seconds=1)
                run.start(body, fmt='p')
                run.until(lambda: any(action=='SetAVTransportURI' for action, _ in run.device.requests))
                soap = next(wire for action, wire in run.device.requests if action=='SetAVTransportURI')
                root = ET.fromstring(soap)
                didl = next(n.text for n in root.iter() if n.tag.rsplit('}', 1)[-1]=='CurrentURIMetaData')
                check(didl, record, artwork)
                assert cli.commands, cli.commands
            finally:
                if run: run.close()
                cli.shutdown()
                worker.join()
            assert not cli.errors, cli.errors
        print('PASS stream artwork (real yeney/CLI/SOAP): '+name, flush=True)

# Check the production QueueRoom asset binding too, beyond snapshot/DIDL units.
# Keep this fixture's existing title/album assertions; only artwork fields differ.
sys.path.insert(0, str(ROOT/'tests'))
import queue_binary_test as queue_binary

class QueueClient(socketserver.StreamRequestHandler):
    def handle(self):
        for wire in self.rfile:
            line = wire.decode().strip()
            if line == 'listen 1':
                self.wfile.write(b'listen 1\n');self.wfile.flush();continue
            mac, command = line.split(' ', 1)
            assert mac == self.server.owner.mac and '%' not in mac, line
            if command.startswith('status '):
                record = 'playlist_tracks:4 playlist_cur_index:0 time:0 playlist_timestamp:1 '
                for i in range(4):
                    record += fields(**{'playlist index': str(i)})+' '
                    record += podcast+' '+fields(title=f'Title & {i}', artist='Artist <one>',
                                                  album='Album & fixture')+' '
                self.wfile.write((line+' '+record+'\n').encode())
            else:
                raise AssertionError('unexpected queue CLI command: '+line)
            self.wfile.flush()

class QueueCLI(CLI):
    def __init__(self, owner):
        self.owner, self.errors = owner, []
        super().__init__(('127.0.0.1', 9090), QueueClient)
        self.worker = threading.Thread(target=self.serve_forever, daemon=True)
        self.worker.start()

    def close(self):
        self.shutdown()
        self.server_close()
        self.worker.join()
        assert not self.errors, self.errors

queue_binary.CLI = QueueCLI
queue = queue_binary.Binary()
try:
    queue.until(lambda: any('resource' in item for item in queue.items()))
    item = next(item for item in queue.items() if 'resource' in item)
    assert item['metadata']['albumArtURI'] == BASE+podcast_art, item
finally:
    queue.close()
print('PASS queue artwork (real yeney QueueRoom/CLI/SOAP): podcast', flush=True)
