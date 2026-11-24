# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Loopback exercise of the refill probe, including its cleanup paths."""
import contextlib
import hashlib
import json
import io
import math
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from importlib.machinery import SourceFileLoader
from pathlib import Path
import socket
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
from queue_sonos_fake import Coordinator, Position, request
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


class PlaybackPosition(Position):
    def playing_elapsed(self):
        return super().playing_elapsed() / SCALE


class ProbeCoordinator(Coordinator):
    @Coordinator.position.setter
    def position(self, value):
        Coordinator.position.fset(self, value)
        self._position = PlaybackPosition(self, self._position)


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
            assert resource.get('bitsPerSample') in (('16', '24') if compressed else ('24',))
            assert resource.get('nrAudioChannels') == ('1' if compressed else '2')
            assert int(resource.get('size', '0')) > 44 and resource.get('duration') in ('0:03:00', '0:00:30')
            s.items.append(dict(uri=args['EnqueuedURI'], duration=probe.position(resource.get('duration')),
                                bits=int(resource.get('bitsPerSample')), size=int(resource.get('size'))))
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
        if action == 'SetPlayMode':
            assert s.stopped_bridge
            s.play_mode = args['NewPlayMode']
            s.mode_changes.append(s.play_mode)
            return {}, None
        if action in ('Pause', 'Stop', 'SetAVTransportURI', 'Seek', 'Play'):
            if s.join_timer:
                s.join_timer.cancel()
                s.join_timer = None
        result = super().action(service, action, args)
        row = next((row for row in s.items if row['uri'] == s.position['TrackURI']), None)
        if row and 'duration' in row and s.transport_state == 'PLAYING' and (
                action == 'Play' or action == 'Seek' and args['Unit'] == 'REL_TIME'):
            target = probe.position(s.position['RelTime'])
            self.start_audio(row, target)
        return result

    def start_audio(self, row, target):
        s = self.server
        uri, duration = row['uri'], row['duration']

        def fetch():
            try:
                offset = self.media_offset(row, target)
                connection, response = request(uri, byte_range=f'bytes={offset}-' if offset else None)
                with contextlib.closing(connection), contextlib.closing(response):
                    assert response.status == (206 if offset else 200)
                    body = response.read(256)
                    assert len(body) == 256
                    if uri.endswith('.flac'):
                        if offset:
                            assert int.from_bytes(body[:2], 'big') & 0xfffe == 0xfff8
                        else:
                            assert body.startswith(b'fLaC')
                            info = int.from_bytes(body[18:26], 'big')
                            assert ((info >> 36) & 31) + 1 == row['bits']
                    elif not offset:
                        assert body.startswith(b'RIFF') and int.from_bytes(body[34:36], 'little') == 24
            except Exception as exc:
                s.errors.append('probe media request: ' + repr(exc))

        threading.Thread(target=fetch, daemon=True).start()

        def join():
            with s.lock:
                if s.position['TrackURI'] != uri or s.transport_state != 'PLAYING':
                    return
                index = int(s.position['Track'])
                if index < len(s.items) and 'duration' in s.items[index]:
                    following = s.items[index]
                    s.position = dict(Track=str(index + 1), TrackURI=following['uri'], RelTime='00:00:00')
                    self.start_audio(following, 0)

        s.join_timer = threading.Timer(max(0, duration - target) * SCALE, join)
        s.join_timer.daemon = True
        s.join_timer.start()

    @staticmethod
    def media_offset(row, seconds):
        uri = row['uri']
        if seconds < 1:
            return 0
        head, response = request(uri, method='HEAD')
        with contextlib.closing(head), contextlib.closing(response):
            assert response.status == 200
            assert int(response.getheader('Content-Length')) == row['size']
            assert response.read() == b''
        sample = int(seconds * 8000)
        if uri.endswith('.wav'):
            return 44 + sample * 6
        connection, response = request(uri, byte_range='bytes=0-4095')
        with contextlib.closing(connection), contextlib.closing(response):
            header = response.read()
            assert response.status == 206 and header.startswith(b'fLaC')
        cursor = 4
        points = []
        while True:
            flag = header[cursor]
            length = int.from_bytes(header[cursor + 1:cursor + 4], 'big')
            if flag & 0x7f == 3:
                points = list(struct.iter_unpack('>QQH', header[cursor + 4:cursor + 4 + length]))
            cursor += 4 + length
            if flag & 0x80:
                break
        if points:
            return cursor + max(offset for at, offset, _ in points if at <= sample)
        # The fixture knows the fixed frame sizes. Open a separate discovery
        # request to represent reading the header without a seek table.
        frame = sample // 4096
        offset = cursor + sum((1 if i < 128 else 2) + 8 + 4096 * (row['bits'] // 8)
                              for i in range(frame))
        connection, response = request(uri, byte_range=f'bytes={offset}-{offset + 1}')
        with contextlib.closing(connection), contextlib.closing(response):
            assert response.status == 206 and int.from_bytes(response.read(), 'big') & 0xfffe == 0xfff8
        return offset


class Download(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        assert self.path == '/music/42/download'
        self.send_response(200)
        self.send_header('Content-Length', str(len(self.server.audio)))
        self.end_headers()
        self.wfile.write(self.server.audio)


def fixture(work, seconds, rate=8000, channels=1):
    path = work / f'{seconds}.flac'
    period = [round(15000 * math.sin(2 * math.pi * 440 * i / rate)) for i in range(rate)]
    samples = [sample for sample in period for _ in range(channels)]
    pcm = struct.pack(f'<{len(samples)}h', *samples) * seconds
    subprocess.run(['flac', '--silent', '--force-raw-format', '--endian=little', '--sign=signed',
                    f'--input-size={len(pcm)}', f'--channels={channels}', '--bps=16', f'--sample-rate={rate}', '-o', path, '-'],
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
    audio = p.media['packed16']
    path = audio['path']
    frames, rate = audio['frames'], audio['rate']
    item = probe.Item(path, frames / rate, bound=44 + frames * 6, index=audio['index'], frames=frames)
    for numerator in (1, 10, 60, 99):
        target = frames * numerator // 100
        i = max(i for i, sample in enumerate(item.samples) if sample <= target)
        offset = item.offsets[i]
        status, first, last = item.range(f'bytes={offset}-')
        assert (status, first, last) == (206, offset, item.length - 1)
        following = item.samples[i + 1] if i + 1 < len(item.samples) else frames
        assert item.samples[i] <= target < following
        with path.open('rb') as media:
            media.seek(offset)
            assert int.from_bytes(media.read(2), 'big') & 0xfffe == 0xfff8
    with probe.MediaServer('127.0.0.1', p.records) as server:
        server.items['/bound'] = item
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            uri = f'http://127.0.0.1:{server.server_port}/bound'
            offset = item.offsets[len(item.offsets) * 3 // 5]
            connection, response = request(uri, byte_range=f'bytes={offset}-')
            with contextlib.closing(connection), contextlib.closing(response):
                assert response.status == 206
                assert response.getheader('Content-Range') == f'bytes {offset}-{item.length - 1}/{item.length}'
                assert response.getheader('Content-Length') == str(item.length - offset)
                with path.open('rb') as media:
                    media.seek(offset)
                    assert response.read(64) == media.read(64)
            connection, response = request(uri, byte_range=f'bytes={item.actual}-')
            with contextlib.closing(connection), contextlib.closing(response):
                assert response.status == 206
                assert response.getheader('Content-Range') == f'bytes {item.actual}-{item.length - 1}/{item.length}'
                assert response.read(1) == b''
        finally:
            server.shutdown()
            thread.join()

    with probe.MediaServer('127.0.0.1', p.records) as server:
        audio = p.media['wav']
        wav = probe.Item(audio['path'], audio['frames'] / audio['rate'])
        server.items['/wire.wav'] = wav
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            uri = f'http://127.0.0.1:{server.server_port}/wire.wav'
            connection, response = request(uri, method='HEAD', byte_range='bytes=45-103')
            with contextlib.closing(connection), contextlib.closing(response):
                assert response.status == 206 and response.getheader('Content-Length') == '59'
                assert response.getheader('Content-Range') == f'bytes 45-103/{wav.length}'
                assert response.read() == b''
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

    for form in ('wav', 'fixed16', 'packed16', 'packed24'):
        media = p.media[form]
        records = probe.Records()
        with probe.MediaServer('127.0.0.1', records) as server:
            item = probe.Item(media['path'], media['frames'] / media['rate'],
                              index=media['index'], frames=media['frames'])
            server.items['/pace'] = item
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                uri = f'http://127.0.0.1:{server.server_port}/pace'
                count = int(item.burst + item.play_rate * 2 * .35)
                started = time.monotonic()
                connection, response = request(uri)
                with contextlib.closing(connection):
                    assert len(response.read(count)) == count
                assert time.monotonic() - started >= .33
            finally:
                server.closing.set()
                server.shutdown()
                thread.join()


def check_stereo_fixed(work):
    helper = probe.audio_helper(work)
    frames, rate = 2050 * 4096 + 5, 44100
    samples = bytes((0x0d, 0x34, 0x12, 0x07, 0xcb, 0xed))
    pcm, media = work / 'stereo24.pcm', work / 'stereo24.flac'
    with pcm.open('wb') as stream:
        for _ in range(2050):
            stream.write(samples * 4096)
        stream.write(samples * 5)
    proof = tuple(map(int, probe.audio_command(helper, 'fixed', pcm, media, frames, rate, 24, 2).split()))
    _, evidence = probe.verify_flac(helper, media, pcm, 3, 2, frames, work, proof, True)
    assert evidence['predicted'] == evidence['written']
    assert evidence['decoded_bit_exact'] and evidence['range_bit_exact']
    assert evidence['largest_frame'] == 24588
    print('stereo-fixed24', json.dumps(evidence, sort_keys=True))


def check_millisecond_figures():
    fixture = json.loads((ROOT / 'tests/fixtures/refill-millisecond-fixture.json').read_text())
    for case in fixture['cases']:
        answers = probe.playing_answers([
            dict(sent=sent / 1000, received=received / 1000, position_s=position / 1000, state=state)
            for sent, received, position, state in case['rows']])
        gap = probe.join_gap(answers, case['anchor_ms'] / 1000, case['duration_ms'] / 1000)
        offset = probe.position_offset(answers, case['steady_from_ms'] / 1000, case['steady_until_ms'] / 1000)
        assert math.isclose(gap, case['expected_join_ms'], abs_tol=.001)
        assert math.isclose(offset, case['expected_offset_ms'], abs_tol=.001)
        print(f"clock fixture {case['case']} join_gap_ms={gap:.3f} position_offset_ms={offset:.3f}")


def check_field_joins():
    fixture = json.loads((ROOT / 'tests/fixtures/refill-field-joins.json').read_text())
    assert len(fixture) == 26
    failures = []
    for j, (case, join, duration, expected, samples) in enumerate(fixture):
        answers = [dict(sent=sent / 1000, received=received / 1000,
                        position_s=position / 1000, latency_ms=received - sent)
                   for sent, received, position in samples]
        gap = probe.seek_join(answers, 0, duration)
        print(f'archive {1 + j // 13} {case} join {join}: {gap} ms, expected {expected:+.1f} ms')
        if gap is None or abs(gap - expected) > 1.5:
            failures.append((case, join, gap, expected))
    assert not failures, failures


def check_prediction():
    for rate, block, target, expected in (
        (44100, 4096, 369000, 104.40),
        (44100, 4096, 18000, 111.56),
        (44100, 4096, 1024000, 4096 / 44.1),
        (48000, 4096, 20000, (4096 + 2560) / 48),
        (44100, 256, 21000, (256 + 108) / 44.1),
        (44100, 4096, 21013, (8192 - (926673 % 4096)) / 44.1),
    ):
        _, gap = probe.seek_prediction(rate, block, target)
        assert math.isclose(gap, expected, abs_tol=.005), (rate, block, target, gap)
        print(f'{rate} Hz, block {block}, target {target} ms: {gap:.6f} ms')


def check_block_sizes(work):
    helper = probe.audio_helper(work)
    for block in (16, 256, 1024, 4096):
        for mode in ('fixed-noseek', 'packed'):
            frames = 3 * block + 3
            pcm, media = work / 'block.pcm', work / 'block.flac'
            pcm.write_bytes(bytes((0x34, 0x12)) * frames)
            result = probe.audio_command(helper, mode, pcm, media, frames, 44100, 16, 1, block)
            proof = tuple(map(int, result.split())) if mode != 'packed' else None
            index, evidence = probe.verify_flac(helper, media, pcm, 2, 1, frames, work, proof,
                                               mode == 'packed', block=block)
            assert [sample for sample, _ in index] == [0, block, 2 * block, 3 * block]
            assert evidence['decoded_bit_exact'] and evidence['range_bit_exact']
            if proof:
                assert evidence['predicted'] == evidence['written']
            print(f'block {block} {mode}', json.dumps(evidence, sort_keys=True))


class SeekRenderer:
    """Deterministic speaker clock with separate reported and consumed positions."""

    RATE = 44100
    BLOCK = 4096

    def __init__(self, records, fixed_offset):
        self.records = records
        self.fixed_offset = fixed_offset
        self.now = 0.0
        self.number = 1
        self.state = 'STOPPED'
        self.origin = self.base = self.offset = 0.0
        self.sample_at = .25
        self.uris = [f'http://127.0.0.1/row-{i}.flac' for i in range(1, 12)]

    def av(self, action, **args):
        if action == 'Play':
            self.origin = self.now
            self.state = 'PLAYING'
        elif action == 'Pause':
            self.base += self.now - self.origin
            self.state = 'PAUSED'
        elif action == 'Seek':
            assert args['Unit'] == 'REL_TIME'
            self.base = probe.position(args['Target'])
            self.origin = self.now
            sample = int(self.base * self.RATE)
            next_block = (sample + self.BLOCK - 1) // self.BLOCK + 1
            self.offset = .1045 if self.fixed_offset else (next_block * self.BLOCK - sample) / self.RATE
            self.records.rows.append(dict(kind='requests', event='open', time=self.now,
                                          path=f'/row-{self.number}.flac', range='bytes=42-'))

    def advance_clock(self, delay):
        end = self.now + delay
        while self.sample_at <= end:
            at = self.sample_at
            if self.state == 'PLAYING':
                join = self.origin + 40 - self.base - self.offset
                if at >= join:
                    self.number += 1
                    self.origin, self.base, self.offset = join, 0, 0
                position = round((self.base + at - self.origin) * 1000) / 1000
                for protocol in ('websocket', 'UPnP'):
                    self.records.rows.append(dict(kind='questions', time=at + .001,
                                                  sent=at - .001, received=at + .001,
                                                  protocol=protocol, position_s=position, latency_ms=2,
                                                  item=self.uris[self.number - 1], state=self.state))
            self.sample_at += .25
        self.now = end


def check_seek_verdict():
    with mock.patch.object(probe.Experiment, 'choose'), mock.patch.object(probe.Experiment, 'add_items'):
        for fixed_offset in (False, True):
            records = probe.Records()
            speaker = SeekRenderer(records, fixed_offset)
            case = 'fixed16-4096'
            args = SimpleNamespace(set='seek', cases=[case])
            experiment = probe.Experiment(args, None, speaker, records)
            result = dict(case=case, repeat=1)
            experiment.results.append(result)
            experiment.add_items.return_value = speaker.uris
            with (mock.patch.object(experiment, 'transport', side_effect=lambda: (speaker.state, 'OK')),
                  mock.patch.object(experiment, 't', side_effect=lambda: speaker.now),
                  mock.patch.object(probe.time, 'sleep', side_effect=speaker.advance_clock)):
                experiment.measure_chain(result)
            assert len(result['rows']) == 10 and speaker.number == 11
            assert [r['request_count'] for r in result['rows']] == [0, 1, 1, 1, 1, 1, 1, 2, 1, 1]
            if not fixed_offset:
                assert all(abs(row['difference_ms']) < 1 for row in result['rows']), result['rows']
            assert probe.seek_verdict([result])[-1] == ('H fails' if fixed_offset else 'H holds')
            print(experiment.seek_summary(), end='')


def check_named_waits(work):
    class HeldDownload(BaseHTTPRequestHandler):
        def do_GET(self):
            self.server.release.wait(2)

    timer = threading.Timer
    with ThreadingHTTPServer(('127.0.0.1', 0), HeldDownload) as server:
        server.release = threading.Event()
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            messages = []
            with mock.patch.object(probe.threading, 'Timer',
                                   side_effect=lambda delay, *a, **kw: timer(delay / 1000, *a, **kw)):
                try:
                    probe.track('127.0.0.1', server.server_port, 42, work / 'held.flac', messages.append)
                except TimeoutError as exc:
                    assert 'LMS' in str(exc) and '120' in str(exc) and 'track' in str(exc), str(exc)
                    print(exc)
                else:
                    raise AssertionError('track timeout did not surface')
            assert messages == ['waiting for LMS to deliver the track'], messages
            assert not (work / 'held.flac').exists()
        finally:
            server.release.set()
            server.shutdown()
            thread.join()

    reader, silent = socket.socketpair()
    with reader, silent:
        fake = mock.Mock()
        fake.socket.return_value = SimpleNamespace(group='group', household='household',
                                                   socket=reader, send=reader.sendall,
                                                   receive=lambda: reader.recv(1), close=reader.close)
        try:
            probe.ws_check(fake)
        except TimeoutError as exc:
            assert 'speaker websocket' in str(exc) and '3' in str(exc) and 'position' in str(exc), str(exc)
            print(exc)
        else:
            raise AssertionError('speaker timeout did not surface')


def run():
    check_millisecond_figures()
    check_prediction()
    check_seek_verdict()
    with tempfile.TemporaryDirectory(prefix='refill-test-') as directory, DownloadServer() as downloads:
        work = Path(directory)
        check_named_waits(work)
        check_stereo_fixed(work)
        check_block_sizes(work)
        full, short = fixture(work, 180), fixture(work, 2)
        downloads.audio = full
        renderer = ProbeCoordinator(handler=Renderer)
        renderer.crossfade = False
        renderer.realtime = True
        renderer.follow_controls = True
        renderer.fail_play = False
        renderer.join_timer = None
        renderer.stopped_bridge = False
        renderer.mode_changes = []
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
                if action == 'stop':
                    renderer.stopped_bridge = True
                elif action == 'start':
                    assert renderer.play_mode != 'NORMAL' or not getattr(args, 'normal_play_mode', False)
                    renderer.stopped_bridge = False
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
                renderer.play_mode = 'SHUFFLE'
                with contextlib.redirect_stdout(io.StringIO()) as refusal:
                    assert new().check('test-unit') == 1
                assert 'speaker play mode SHUFFLE, crossfade 0' in refusal.getvalue()
                assert 'measurement would refuse' in refusal.getvalue()
                args.normal_play_mode = True
                renderer.crossfade = True
                assert new().check('test-unit') == 1
                renderer.crossfade = False
                assert not renderer.mode_changes
                experiment.run('test-unit')
                assert renderer.play_mode == 'SHUFFLE' and renderer.mode_changes == ['NORMAL', 'SHUFFLE']
                assert len(experiment.results) == 11 and all('error' not in row for row in experiment.results), experiment.results
                assert renderer.items == renderer.original and renderer.volume == 23
                assert calls[-3:] == ['show', 'stop', 'start']
                path = work / 'normal.tar.gz'
                experiment.archive(path)
                assert all('start_advancing_ms' in row and 'join_gap_ms' in row for row in experiment.results)
                assert all('position_offset_ms' in row and row['seek_requests'] > 0 and
                           row['tail_seek_requests'] > 0 for row in experiment.results)
                assert all('second_join_gap_ms' in row for row in experiment.results
                           if row['case'] in ('short', 'short-fixed16'))
                for form in ('fixed16', 'fixed24', 'fixed16-noseek', 'short-fixed16'):
                    evidence = experiment.media[form]['proof']
                    assert evidence['predicted'] == evidence['written']
                    assert evidence['decoded_bit_exact'] and evidence['range_bit_exact']
                    assert evidence['largest_frame'] <= 32768
                    assert bool(evidence['seek_points']) == (form != 'fixed16-noseek')
                    print(form, json.dumps(evidence, sort_keys=True))
                inspect_archive(path)
                check_ranges(experiment)
                args.cases = ['wav', 'packed16']
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


LEGACY_CHECKSUMS = [
    'b0cb1805264d6dcdf7e256016d2927f624e9ded97dd07b476445c62892455d78',
    '945841747b4d452a7fd386c3411b59ad96f5fc58e735e92328a6be876f182c84',
]


def check_listen_writer(work, test):
    helper = probe.audio_helper(work)
    pcm, media = work / 'listen.pcm', work / 'listen.flac'
    if test in ('baseline', 'unchanged'):
        frames = 4099
        pcm.write_bytes(bytes.fromhex('3412 78ef') * frames)
        checksums = []
        for mode in ('fixed', 'fixed-noseek'):
            probe.audio_command(helper, mode, pcm, media, frames, 44100, 16, 2)
            checksums.append(hashlib.sha256(media.read_bytes()).hexdigest())
        print('Legacy refill and seek checksums:', *checksums)
        if test == 'unchanged':
            assert checksums == LEGACY_CHECKSUMS
        return
    for block, rate, bits, first, last in (
        (4096, 44100, 16, 'fff8c918', 'fff87918'),
        (256, 44100, 16, 'fff87918', 'fff87918'),
        (256, 44100, 24, 'fff8791c', 'fff8791c'),
        (256, 48000, 16, 'fff87a18', 'fff87a18'),
    ):
        frames = block + 17
        width = bits // 8
        pcm.write_bytes(bytes(range(width * 2)) * frames)
        for enabled in (False, True):
            flag = '--subset' if test == 'headers' else '--md5'
            args = [flag] if enabled else []
            probe.audio_command(helper, 'fixed-noseek', pcm, media, frames, rate, bits, 2, block, *args)
            raw = media.read_bytes()
            if test == 'headers':
                tail = 42 + 10 + block * width * 2 + (2 if block != 4096 else 0)
                expected = (first, last) if enabled else (
                    'fff8c010' if block == 4096 else 'fff87010', 'fff87010')
                assert (raw[42:46].hex(), raw[tail:tail + 4].hex()) == expected
            else:
                assert raw[26:42] == (hashlib.md5(pcm.read_bytes()).digest() if enabled else bytes(16))
                decoded = work / 'md5-decoded.pcm'
                decoded.unlink(missing_ok=True)
                probe.audio_command(helper, media, decoded)


def check_no_terminal():
    with mock.patch('sys.argv', ['yeney-refill-probe', '--speaker', '127.0.0.1',
                                '--lms-host', '127.0.0.1', '--room', 'Loopback',
                                '--track-id', '42', '--set', 'listen']), \
         mock.patch.object(probe.os, 'open', side_effect=OSError('no terminal')) as terminal, \
         mock.patch.object(probe, 'Speaker') as speaker, \
         contextlib.redirect_stderr(io.StringIO()) as output:
        try:
            probe.main()
        except SystemExit as error:
            assert error.code != 0
        else:
            raise AssertionError('expected refusal')
    speaker.assert_not_called()
    terminal.assert_called_once_with('/dev/tty', probe.os.O_RDWR)
    assert 'terminal' in output.getvalue() and '--answers' in output.getvalue()


class ListenRenderer(Renderer):
    def action(self, service, action, args):
        if action == 'AddURIToQueue':
            item, = ET.fromstring(args['EnqueuedURIMetaData'])
            resource = item[-1]
            assert resource.text == args['EnqueuedURI']
            assert resource.get('sampleFrequency') == '44100'
            assert resource.get('duration') == '0:00:20'
            assert resource.get('nrAudioChannels') == '2'
            self.server.items.append(dict(uri=args['EnqueuedURI'], duration=20,
                                          bits=int(resource.get('bitsPerSample'))))
            self.server.update += 1
            return {}, None
        if action == 'Play':
            assert self.server.position['RelTime'] == '00:00:00'
            self.server.listen_plays.append(self.server.position['TrackURI'])
        return super().action(service, action, args)


def check_listen_speaker(work, test):
    with DownloadServer() as downloads:
        downloads.audio = fixture(work, 81, rate=44100, channels=2)
        renderer = ProbeCoordinator(handler=ListenRenderer)
        renderer.listen_plays = []
        renderer.crossfade = False
        renderer.realtime = True
        renderer.follow_controls = True
        renderer.fail_play = False
        renderer.join_timer = None
        renderer.stopped_bridge = False
        renderer.mode_changes = []
        renderer.missing = set()
        renderer.av_control = AV_CONTROL
        api = LocalApi(renderer, port=0)
        api.rtt_ms = api.spike_ms = 0
        try:
            cases = ['wav', 'packed16', 'fixed16', 'fixed16-subset', 'fixed16-md5',
                     'fixed16-subset-md5', 'fixed16-256-subset', 'fixed24-256-subset']
            answers = 'y,y,n,r,y,n,y,y,y' if test == 'listen' else 'n,y,n,y,n,y,y,y'
            argv = ['yeney-refill-probe', '--speaker', '127.0.0.1', '--lms-host', '127.0.0.1',
                    '--room', 'Study', '--track-id', '42', '--set', 'listen', '--answers', answers]
            with mock.patch('sys.argv', argv), mock.patch.object(probe.os, 'geteuid', return_value=0), \
                 mock.patch.object(probe, 'execute') as execute:
                probe.main()
            args, = execute.call_args.args
            assert args.cases == cases
            records = probe.Records()
            speaker = probe.Speaker('127.0.0.1', records, renderer.server_port, api.server.server_port)
            experiment = probe.Experiment(args, work, speaker, records, SCALE, downloads.server_port)
            with mock.patch.object(probe, 'service', return_value=False):
                experiment.run('test-unit')
            path = work / 'listen.tar.gz'
            experiment.archive(path)
            with tarfile.open(path) as archive:
                rows = json.load(archive.extractfile('answers.json'))
                table = archive.extractfile('summary.txt').read().decode()
            assert [row['case'] for row in rows] == cases
            expected = ['heard' if answer == 'y' else 'silent' for answer in args.answers if answer != 'r']
            assert [row['answer'] for row in rows] == expected
            assert [row['replays'] for row in rows] == [0, 0, 0, int(test == 'listen'), 0, 0, 0, 0]
            shown = [row for row in records.rows if row['kind'] == 'log' and 'NOW PLAYING' in str(row)]
            order = cases.copy()
            numbers = list(range(1, len(cases) + 1))
            if test == 'listen':
                order.insert(3, cases[3])
                numbers.insert(3, 4)
                assert renderer.listen_plays[3] == renderer.listen_plays[4]
            assert [row['message'] for row in shown] == [
                f'NOW PLAYING {case} ({number} of {len(cases)})' for case, number in zip(order, numbers)
            ]
            assert len(renderer.listen_plays) == len(order)
            assert all(attempt['requests'] for run in experiment.results for attempt in run['attempts'])
            assert 'bitDepth=24 sampleRate=44100' in table
            for case, answer in zip(cases, expected):
                assert f'{case}\t1\t{answer}\t' in table
            if test == 'controls':
                assert table.splitlines()[-1] == 'controls failed, the run proves nothing; check volume and room'
            else:
                assert 'rate and depth: silent to heard' in table.splitlines()[-1]
                for case in cases[2:]:
                    audio = experiment.media[case]
                    raw = audio['path'].read_bytes()
                    print(case, raw[:64].hex(' '), raw[42:46].hex(' '), json.dumps(audio['proof']))
            assert renderer.items == renderer.original
        finally:
            api.close()
            renderer.close()


def check_probe04(test, work):
    if test == 'no-terminal':
        check_no_terminal()
    elif test in ('listen', 'controls'):
        check_listen_speaker(work, test)
    else:
        check_listen_writer(work, test)
    print('PASS', test)


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
    import sys

    if sys.argv[1:] == ['--field-joins']:
        check_field_joins()
    elif len(sys.argv) == 3 and sys.argv[1] == '--probe04':
        with tempfile.TemporaryDirectory() as directory:
            check_probe04(sys.argv[2], Path(directory))
    else:
        for test in ('headers', 'md5', 'unchanged', 'listen', 'controls', 'no-terminal'):
            with tempfile.TemporaryDirectory() as directory:
                check_probe04(test, Path(directory))
        run()
