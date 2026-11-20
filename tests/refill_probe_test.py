# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Loopback exercise of the refill probe, including its cleanup paths."""
import contextlib
import io
import math
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from importlib.machinery import SourceFileLoader
from pathlib import Path
import struct
import subprocess
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest.mock as mock
import tarfile
import xml.etree.ElementTree as ET

from queue_binary_test import Device
from queue_sonos_fake import Coordinator, request
from sonos_websocket_fake import LocalApi

ROOT = Path(__file__).resolve().parents[1]
probe = SourceFileLoader('refill_probe', str(ROOT / 'scripts/yeney-refill-probe')).load_module()
SCALE = .025


DESCRIPTIONS = ROOT / 'tests/fixtures/refill-study'
DESCRIPTION_PATH = '/xml/device_description.xml'
AV_CONTROL = '/MediaRenderer/AVTransport/Control'
DOCUMENT = (DESCRIPTIONS / 'device_description.xml').read_text()
DESCRIPTION_ROUTES = {DESCRIPTION_PATH: DOCUMENT}
CONTROL_ROUTES = set()
for node in ET.fromstring(DOCUMENT).iter():
    if node.tag.endswith('}SCPDURL'):
        DESCRIPTION_ROUTES[node.text] = (DESCRIPTIONS / Path(node.text).name).read_text()
    elif node.tag.endswith('}controlURL'):
        CONTROL_ROUTES.add(node.text)


class Renderer(Device):
    def do_GET(self):
        document = DESCRIPTION_ROUTES.get(self.path)
        if document is None:
            self.send_error(404)
            return
        if self.path == DESCRIPTION_PATH:
            document = document.replace(AV_CONTROL, self.server.av_control)
        if self.server.missing and self.path == '/xml/AVTransport1.xml':
            root = ET.fromstring(document)
            for actions in root.iter():
                for action in list(actions):
                    if probe.local(action.tag) == 'action' and any(
                            probe.local(child.tag) == 'name' and child.text in self.server.missing
                            for child in action):
                        actions.remove(action)
            document = ET.tostring(root, encoding='unicode')
        self.send(document)

    def do_POST(self):
        if self.path not in CONTROL_ROUTES:
            self.send_error(404)
            return
        assert list(self.headers) == ['Host', 'Connection', 'Content-Length', 'Content-Type', 'SOAPACTION']
        assert self.headers['Connection'] == 'close'
        assert self.headers['Content-Type'] == 'text/xml'
        super().do_POST()

    def action(self, service, action, args):
        s = self.server
        if action == 'AddURIToQueue':
            item, = ET.fromstring(args['EnqueuedURIMetaData'])
            assert [probe.local(element.tag) for element in item] == ['title', 'creator', 'artist', 'album', 'class', 'res']
            resource = item[-1]
            assert resource.text == args['EnqueuedURI'] and resource.get('sampleFrequency') == '8000'
            compressed = resource.text.endswith('.flac')
            assert resource.get('bitsPerSample') == ('16' if compressed else '24')
            assert resource.get('nrAudioChannels') == ('1' if compressed else '2')
            assert int(resource.get('size', '0')) > 44 and resource.get('duration') == '0:03:00'
            s.items.append({'uri': args['EnqueuedURI']})
            s.update += 1
            return {}, None
        if action == 'RemoveTrackRangeFromQueue':
            assert int(args['UpdateID']) == s.update
            index = int(args['StartingIndex']) - 1
            assert s.items[index]['uri'] not in {row['uri'] for row in s.original}
            del s.items[index]
            s.update += 1
            return {}, None
        if action == 'SetAVTransportURI' and not args.get('CurrentURI'):
            s.uri = ''
            s.position = dict(Track='0', TrackURI='', RelTime='00:00:00')
            return {}, None
        if action == 'Play' and s.fail_play:
            s.fail_play = False
            return {}, '712'
        if action in ('Play', 'Seek') and s.position['TrackURI'].startswith('http://127.'):
            if action == 'Play' or args['Unit'] == 'REL_TIME':
                def fetch(uri, seconds):
                    try:
                        offset = 0
                        if seconds:
                            head, response = request(uri, method='HEAD')
                            with contextlib.closing(head):
                                assert response.status == 200
                                offset = int(response.getheader('Content-Length')) * int(seconds) // 180
                        connection, response = request(uri, byte_range=f'bytes={offset}-' if offset else None)
                        with contextlib.closing(connection):
                            assert response.status == (206 if offset else 200)
                            response.read(256)
                    except Exception as exc:
                        s.errors.append('probe media request: ' + repr(exc))
                seek = action == 'Seek'
                target = probe.position(args['Target']) if seek else 0
                threading.Thread(target=fetch, args=(s.position['TrackURI'], target),
                                 daemon=True).start()
                if seek and target > 160:
                    uri = s.position['TrackURI']
                    following = s.items[int(s.position['Track'])]['uri']

                    def join():
                        if s.position['TrackURI'] == uri and s.transport_state == 'PLAYING':
                            s.position = dict(Track=str(int(s.position['Track']) + 1),
                                              TrackURI=following, RelTime='00:00:00')
                            fetch(following, 0)

                    timer = threading.Timer((180 - target) * SCALE, join)
                    timer.daemon = True
                    timer.start()
        return super().action(service, action, args)


class Download(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        assert self.path == '/music/42/download'
        self.send_response(200)
        self.send_header('Content-Length', str(len(self.server.audio)))
        self.end_headers()
        self.wfile.write(self.server.audio)


def fixture(work, seconds):
    path = work / f'{seconds}.flac'
    period = [round(15000 * math.sin(2 * math.pi * 440 * i / 8000)) for i in range(8000)]
    pcm = struct.pack('<8000h', *period) * seconds
    subprocess.run(['flac', '--silent', '--force-raw-format', '--endian=little', '--sign=signed',
                    f'--input-size={len(pcm)}', '--channels=1', '--bps=16', '--sample-rate=8000', '-o', path, '-'],
                   input=pcm, check=True)
    return path.read_bytes()


def inspect_archive(path):
    names = {'per-second.tsv', 'questions.tsv', 'requests.jsonl', 'speaker.jsonl',
             'summary.txt', 'summary.json', 'probe.log', 'versions.txt'}
    with tarfile.open(path) as archive:
        assert set(archive.getnames()) == names
        for name in names:
            content = archive.extractfile(name).read()
            assert b'RINCON_TEST' not in content and b'127.0.0.1' not in content
            assert b'loopback-household' not in content and b'loopback-group-1' not in content
            assert b'Study' not in content and b'foreign.invalid' not in content
    print('Archive files ' + ', '.join(sorted(names)))


def check_ranges(experiment):
    p = experiment
    item = probe.Item(p.flac, p.duration, bound=p.wav.stat().st_size, index=p.index, frames=p.frames)
    for numerator in (1, 10, 60, 99):
        target = item.length * numerator // 100
        status, first, last, offset = item.range(f'bytes={target}-')
        assert (status, first, last) == (206, target, item.length - 1)
        i = item.offsets.index(offset)
        following = item.samples[i + 1] if i + 1 < len(item.samples) else p.frames
        assert item.samples[i] <= target * p.frames // item.length < following
        with p.flac.open('rb') as media:
            media.seek(offset)
            assert int.from_bytes(media.read(2), 'big') & 0xfffe == 0xfff8
    with probe.MediaServer('127.0.0.1', p.records) as server:
        server.items['/bound'] = item
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            uri = f'http://127.0.0.1:{server.server_port}/bound'
            connection, response = request(uri, byte_range=f'bytes={item.length * 3 // 5}-')
            with contextlib.closing(connection):
                assert response.status == 206
                assert response.getheader('Content-Range') == f'bytes {item.length * 3 // 5}-{item.length - 1}/{item.length}'
                assert response.getheader('Content-Length') == str(item.length - item.length * 3 // 5)
                assert response.read(2)[0] == 0xff
            item.eof.set()
            connection, response = request(uri, byte_range=f'bytes={item.actual}-')
            with contextlib.closing(connection):
                assert response.status == 206
                assert response.getheader('Content-Range') == f'bytes {item.actual}-{item.length - 1}/{item.length}'
                assert response.read(1) == b''
        finally:
            server.shutdown()
            thread.join()

    with probe.MediaServer('127.0.0.1', p.records) as server:
        wav = probe.Item(p.wav, p.duration)
        server.items['/wire.wav'] = wav
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            uri = f'http://127.0.0.1:{server.server_port}/wire.wav'
            for byte_range, status, reason, length in (
                    ('', 200, 'OK', wav.length), ('bytes=45-103', 206, 'Partial Content', 59)):
                connection, response = request(uri, byte_range=byte_range)
                with contextlib.closing(connection):
                    assert (response.version, response.status, response.reason) == (11, status, reason)
                    expected = [('Content-Type', 'audio/wav'), ('Content-Length', str(length)),
                                ('Accept-Ranges', 'bytes'), ('Connection', 'close')]
                    if byte_range:
                        expected.append(('Content-Range', f'bytes 45-103/{wav.length}'))
                    assert response.getheaders() == expected
                    if byte_range:
                        assert len(response.read()) == length
        finally:
            server.shutdown()
            thread.join()

    for path, pace in ((p.wav, 2), (p.wav, 1.25), (p.flac, 2)):
        records = probe.Records()
        with probe.MediaServer('127.0.0.1', records) as server:
            item = probe.Item(path, p.duration, pace=pace, index=p.index, frames=p.frames)
            server.items['/pace'] = item
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                uri = f'http://127.0.0.1:{server.server_port}/pace'
                rate = item.actual / item.duration
                count = item.burst_end + int(rate * pace * .35)
                started = time.monotonic()
                connection, response = request(uri)
                with contextlib.closing(connection):
                    assert len(response.read(count)) == count
                assert time.monotonic() - started >= .33
            finally:
                server.closing.set()
                server.shutdown()
                thread.join()


def run():
    with tempfile.TemporaryDirectory(prefix='refill-test-') as directory, DownloadServer() as downloads:
        work = Path(directory)
        full, short = fixture(work, 180), fixture(work, 2)
        downloads.audio = full
        renderer = Coordinator(handler=Renderer)
        renderer.crossfade = False
        renderer.realtime = True
        renderer.follow_controls = True
        renderer.fail_play = False
        renderer.missing = set()
        renderer.av_control = AV_CONTROL
        api = LocalApi(renderer, port=0)
        api.shape = 'tidy'
        api.rtt_ms = api.spike_ms = 0
        try:
            args = SimpleNamespace(room='Study', lms_host='127.0.0.1', track_id=42, volume=27,
                                   cases=list(probe.CASES), repeats=1)
            calls = []

            def service(name, action):
                calls.append(action)
                return True

            def new():
                records = probe.Records()
                records.private.add('Study')
                speaker = probe.Speaker('127.0.0.1', records, renderer.server_port, api.server.server_port)
                return probe.Experiment(args, work, speaker, records, SCALE, downloads.server_port)

            with mock.patch.object(probe, 'service', service):
                experiment = new()
                assert experiment.check('test-unit') == 0
                assert calls == ['show']
                renderer.av_control = '/MediaRenderer/AVTransport/OtherControl'
                with contextlib.redirect_stdout(io.StringIO()) as output:
                    assert new().check('test-unit') == 1
                assert ("AVTransport advertises controlURL '/MediaRenderer/AVTransport/OtherControl', "
                        "probe uses '/MediaRenderer/AVTransport/Control'") in output.getvalue()
                renderer.av_control = AV_CONTROL
                for label in ('websocket', 'action', 'not-flac', 'short'):
                    api.disabled = label == 'websocket'
                    renderer.missing = {'Seek'} if label == 'action' else set()
                    downloads.audio = b'other data' if label == 'not-flac' else short if label == 'short' else full
                    assert new().check('test-unit') == 1, label
                downloads.audio = full
                experiment.run('test-unit')
                assert len(experiment.results) == 6 and all('error' not in row for row in experiment.results), experiment.results
                assert renderer.items == renderer.original and renderer.volume == 23
                assert calls[-3:] == ['show', 'stop', 'start']
                path = work / 'normal.tar.gz'
                experiment.archive(path)
                assert all('start_advancing_ms' in row and 'join_delta_ms' in row for row in experiment.results)
                inspect_archive(path)
                check_ranges(experiment)
                args.cases = ['wav', 'flac']
                renderer.fail_play = True
                failed = new()
                failed.run('test-unit')
                assert 'error' in failed.results[0] and 'error' not in failed.results[1]
                assert calls[-3:] == ['show', 'stop', 'start'] and renderer.items == renderer.original
                interrupted = new()
                def interrupt(_since, _seconds):
                    time.sleep(.08)
                    raise KeyboardInterrupt

                interrupted.wait = interrupt
                try:
                    interrupted.run('test-unit')
                except KeyboardInterrupt:
                    pass
                else:
                    raise AssertionError('interrupt was swallowed')
                path = work / 'interrupted.tar.gz'
                interrupted.archive(path)
                assert interrupted.series, 'missing partial refill measurements'
                inspect_archive(path)
                assert calls[-3:] == ['show', 'stop', 'start'] and renderer.items == renderer.original
                assert renderer.volume == 23
        finally:
            api.close()
            renderer.close()
    print('PASS refill probe')


class DownloadServer(ThreadingHTTPServer):
    def __init__(self):
        super().__init__(('127.0.0.1', 0), Download)

    def __enter__(self):
        self.thread = threading.Thread(target=self.serve_forever)
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.shutdown()
        self.thread.join()
        self.server_close()


if __name__ == '__main__':
    run()
