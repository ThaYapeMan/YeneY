"""LMS percent decode -> production DIDL/stream transport; loopback only."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import http.client
from http.server import ThreadingHTTPServer
import os
from pathlib import Path
import socketserver
import subprocess
import tempfile
tempfile.tempdir = (os.environ.get('TMPDIR') or '/var/tmp')
import threading
from urllib.parse import quote_from_bytes
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape
from queue_sonos_fake import Coordinator, PATHS, SERVICES, SOAP

ROOT = Path(__file__).resolve().parents[1]
namespace = {'__file__': str(ROOT/'tests/player_engine_test.py')}
exec((ROOT/'tests/player_engine_test.py').read_text().split('\nwith tempfile.TemporaryDirectory')[0], namespace)
Run, pcm, Speaker = (namespace[k] for k in ('Run', 'pcm', 'Speaker'))
os.environ['YENEY_TITLE_FORMAT'] = 'artist-title'
os.environ['YENEY_PLAYBACK_MODE'] = 'stream'

ALBUM = 'Het beste uit pop in je moerstaal, Vol. 2'
forbidden = bytes(c for c in range(32) if c not in (9, 10, 13))
forbidden += b'\xef\xbf\xbe\xef\xbf\xbf\xed\xa0\x80\xed\xbf\xbf'
cases = [
    ('field-album', (b'Title', b'Artist', b'\x16'+ALBUM.encode()),
     ('Title', 'Artist', ALBUM), ['field=album removed=1 replaced=0']),
    ('forbidden', (b'\x16Title', b'\x07Artist', b'Album'+forbidden),
     ('Title', 'Artist', 'Album'), ['field=title removed=1 replaced=0',
                                  'field=artist removed=1 replaced=0',
                                  'field=album removed=33 replaced=0']),
    ('invalid-utf8', (b'Ti\xfftle', b'Artist', b'Alb\xffum\xe2\x82'),
     ('Ti\ufffdtle', 'Artist', 'Alb\ufffdum\ufffd'),
     ['field=title removed=0 replaced=1', 'field=album removed=0 replaced=2']),
    ('valid-unicode', tuple(s.encode() for s in ('é ü 😀', 'Björk', 'München & <été>')),
     ('é ü 😀', 'Björk', 'München & <été>'), []),
]


def record(values):
    fields = dict(id=b'123', remote=b'0', url=b'file:///local.flac',
                  **dict(zip(('title', 'artist', 'album'), values)))
    return ' '.join(quote_from_bytes(key.encode()+b':'+value, safe='') for key, value in fields.items())


def check(didl, expected, queue=False):
    item = ET.fromstring(didl)[0]  # Expat rejects XML 1.0 forbidden characters.
    values = {n.tag.rsplit('}', 1)[-1]: n.text or '' for n in item}
    title, artist, album = expected
    assert values['title'] == (title if queue else artist+' - '+title), values
    assert values['creator'] == artist and values['album'] == album, values


class CLI(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class Client(socketserver.StreamRequestHandler):
    def handle(self):
        for wire in self.rfile:
            line = wire.decode().strip()
            if line == 'listen 1':
                self.wfile.write(b'listen 1\n'); self.wfile.flush(); continue
            mac, command = line.split(' ', 1)
            assert mac.count(':') == 5 and '%' not in mac, line
            if command in ('pause 1', 'stop'):
                reply = line
            else:
                assert command == 'status - 1 tags:aAlKuj', line
                self.server.commands.append(command)
                reply = line+' '+self.server.record
            self.wfile.write((reply+'\n').encode()); self.wfile.flush()


def cli_paths():
    with tempfile.TemporaryDirectory(prefix='yeney-xml-metadata-') as directory:
        directory = Path(directory)
        fixture = directory/'queue-metadata'
        subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-I.',
                        'tests/artwork_queue_fixture.cpp', 'upnp/queue_metadata.cpp', 'upnp/xml.cpp',
                        '-o', str(fixture)], cwd=ROOT, check=True)
        for name, raw, expected, changes in cases:
            wire = record(raw)
            didl = subprocess.check_output([str(fixture)], input=wire+'\n', text=True)
            check(didl, expected, queue=True)
            print('PASS queue XML metadata via LMS CLI decode: '+name, flush=True)
            with CLI(('127.0.0.1', 9090), Client) as cli:
                cli.record, cli.commands = wire, []
                worker = threading.Thread(target=cli.serve_forever); worker.start()
                run = None
                try:
                    run = Run(name, directory)
                    body, _ = pcm(seconds=1)
                    run.start(body, fmt='p')
                    run.until(lambda: any(a=='Play' for a, _ in run.device.requests))
                    requests = [b for a, b in run.device.requests if a=='SetAVTransportURI']
                    assert len(requests) == 1
                    envelope = ET.fromstring(requests[0])
                    didl = next(n.text for n in envelope.iter() if n.tag.endswith('CurrentURIMetaData'))
                    check(didl, expected)
                    log = (run.base/'bridge.log').read_text()
                    lines = [line for line in log.splitlines() if 'metadata sanitized mode=stream' in line]
                    assert len(lines) == bool(changes), lines
                    for change in changes: assert change in lines[0], lines
                    assert "PlaySqueezeBox: title='"+expected[1]+' - '+expected[0]+"'" in log, log
                    assert cli.commands and not run.device.errors, run.device.errors
                finally:
                    if run: run.close()
                    cli.shutdown(); worker.join()
            print('PASS stream XML metadata via real yeney/CLI/strict Sonos: '+name+
                  '; one field-count log iff changed and matching display title', flush=True)


def post_fault(server, path, action, args):
    fields = ''.join('<'+key+'>'+escape(value)+'</'+key+'>' for key, value in args.items())
    urn = SERVICES['AVTransport']
    body = f'<s:Envelope xmlns:s="{SOAP}"><s:Body><u:{action} xmlns:u="{urn}">{fields}</u:{action}></s:Body></s:Envelope>'
    conn = http.client.HTTPConnection('127.0.0.1', server.server_port, timeout=3)
    try:
        conn.request('POST', path, body.encode(), {'SOAPAction': f'"{urn}#{action}"'})
        response = conn.getresponse(); fault = ET.fromstring(response.read())
        assert response.status == 500
        assert next(n.text for n in fault.iter() if n.tag=='errorCode') == '402'
    finally: conn.close()


def strict_fakes():
    sonos = Coordinator()
    stream = ThreadingHTTPServer(('127.0.0.1', 0), Speaker)
    stream.mode, stream.counts, stream.requests, stream.errors, stream.golden = 'ab', {}, [], [], {}
    stream.current_uri = ''
    worker = threading.Thread(target=stream.serve_forever); worker.start()
    try:
        sonos.crossfade = False
        for bad in ('\x16', '&#x16;', '\ufffe', '&#xD800;'):
            metadata = '<DIDL-Lite><item><album>'+bad+'</album><res>http://127.0.0.1/music/queue.wav</res></item></DIDL-Lite>'
            post_fault(sonos, PATHS['AVTransport'], 'AddURIToQueue',
                       dict(InstanceID='0', EnqueuedURI='http://127.0.0.1/music/queue.wav',
                            EnqueuedURIMetaData=metadata, DesiredFirstTrackNumberEnqueued='0', EnqueueAsNext='0'))
            post_fault(stream, '/MediaRenderer/AVTransport/Control', 'SetAVTransportURI',
                       dict(InstanceID='0', CurrentURI='http://127.0.0.1/music/stream.flac', CurrentURIMetaData=metadata))
        assert sonos.items == sonos.original and stream.current_uri == ''
        assert not stream.errors, stream.errors
        print('PASS strict Sonos XML 1.0 fakes: outer/nested invalid controls, FFFE and surrogate references fault 402 without queue or stream mutation', flush=True)
    finally:
        sonos.close(); stream.shutdown(); stream.server_close(); worker.join()


if __name__ == '__main__':
    strict_fakes()
    cli_paths()
