#!/usr/bin/env python3
"""Only loopback HTTP/UDP/TCP; never discovers or contacts a real speaker."""
import contextlib
import csv
import ctypes as C
import ctypes.util
import http.client
import importlib.machinery
import importlib.util
import io
import json
import os
import re
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from unittest import mock
import urllib.error
import urllib.parse
import urllib.request
import wave
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape
import sys

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader('queue_probe', str(ROOT / 'scripts/sonos-queue-probe'))
spec = importlib.util.spec_from_loader(loader.name, loader)
p = importlib.util.module_from_spec(spec)
sys.modules[loader.name] = p
loader.exec_module(p)


class FakeHandler(p.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def send(self, body, status=200, headers=None):
        raw = body.encode()
        self.send_response(status)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header('Content-Length', str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        fake = self.server.fake
        if self.path == '/description':
            services = ''.join(f'<service><serviceType>{service}</serviceType>'
                               f'<controlURL>/control</controlURL><eventSubURL>/events</eventSubURL>'
                               f'<SCPDURL>/scpd</SCPDURL></service>' for service in (p.AVT, p.QUEUE, p.ZGT))
            self.send('<root><device><UDN>uuid:RINCON_FAKE</UDN><modelName>Fake Play:1</modelName>'
                      '<softwareVersion>86.10-fake</softwareVersion><serviceList>' + services +
                      '</serviceList><deviceList><device><UDN>uuid:EMBEDDED</UDN>'
                      '<modelName>Embedded renderer</modelName></device></deviceList></device></root>')
        else:
            self.send('', 404)

    def do_SUBSCRIBE(self):
        self.server.fake.callback = self.headers.get('CALLBACK', '').strip('<>') or self.server.fake.callback
        self.send('', headers={'SID': 'uuid:fake-events', 'TIMEOUT': 'Second-300'})
        self.server.fake.notify()

    def do_UNSUBSCRIBE(self):
        self.server.fake.unsubscribed = True
        self.send('')

    def do_POST(self):
        fake = self.server.fake
        node = p.xml(self.rfile.read(int(self.headers['Content-Length']))).find(f'{{{p.SOAP}}}Body')[0]
        action = node.tag.rsplit('}', 1)[-1]
        args = p.fields(node)
        service = node.tag[1:].split('}')[0]
        fake.actions.append((action, args, service))
        if action == fake.fail_action:
            self.send(f'<s:Envelope xmlns:s="{p.SOAP}"><s:Body><s:Fault><detail>'
                      '<UPnPError><errorCode>701</errorCode><errorDescription>Injected failure</errorDescription>'
                      '</UPnPError></detail></s:Fault></s:Body></s:Envelope>', 500)
            return
        values = {}
        if action == 'GetZoneGroupState':
            a = f'<ZoneGroupMember UUID="RINCON_FAKE" ZoneName="Study" Location="{fake.location}"/>'
            b = f'<ZoneGroupMember UUID="RINCON_SECOND" ZoneName="Kitchen" Location="{fake.location}"/>'
            groups = (f'<ZoneGroup Coordinator="RINCON_FAKE">{a}{b if fake.grouped else ""}</ZoneGroup>')
            if not fake.grouped:
                groups += f'<ZoneGroup Coordinator="RINCON_SECOND">{b}</ZoneGroup>'
            values = {'ZoneGroupState': '<ZoneGroups>' + groups + '</ZoneGroups>'}
        elif action == 'Browse':
            assert service == p.QUEUE and args == {'QueueID': '0', 'StartingIndex': '0', 'RequestedCount': '1'}
            values = {'TotalMatches': len(fake.queue), 'NumberReturned': min(1, len(fake.queue)), 'Result': '', 'UpdateID': 1}
        elif action == 'GetMediaInfo':
            values = {'CurrentURI': fake.uri, 'CurrentURIMetaData': '', 'NrTracks': len(fake.queue)}
        elif action == 'GetTransportInfo':
            values = {'CurrentTransportState': fake.state, 'CurrentTransportStatus': fake.status}
        elif action == 'GetPositionInfo':
            if fake.queue:
                item = fake.queue[fake.index]
                values = {'Track': fake.index + 1, 'TrackURI': item['uri'],
                          'RelTime': p.time_text(fake.elapsed), 'TrackDuration': item['duration']}
            else:
                values = {'Track': 0, 'TrackURI': '', 'RelTime': '0:00:00', 'TrackDuration': '0:00:00'}
        elif action == 'GetCrossfadeMode':
            values = {'CrossfadeMode': getattr(fake, 'crossfade', 0)}
        elif action == 'SetCrossfadeMode':
            fake.crossfade = int(args['CrossfadeMode'])
        elif action == 'RemoveAllTracksFromQueue':
            fake.queue.clear()
            fake.index = fake.elapsed = 0
        elif action == 'AddURIToQueue':
            md = p.fields(p.xml(args['EnqueuedURIMetaData'].encode()))
            assert md['class'] == 'object.item.audioItem.musicTrack'
            res = next(e for e in p.xml(args['EnqueuedURIMetaData'].encode()).iter() if e.tag.endswith('}res'))
            assert res.get('protocolInfo').startswith('http-get:*:audio/')
            fake.queue.append({'uri': args['EnqueuedURI'], 'duration': res.get('duration'), 'title': md['title']})
            values = {'NumTracksAdded': 1, 'NewQueueLength': len(fake.queue), 'FirstTrackNumberEnqueued': len(fake.queue)}
        elif action == 'SetAVTransportURI':
            fake.uri = args['CurrentURI']
        elif action == 'Seek':
            if args['Unit'] == 'TRACK_NR':
                fake.index, fake.elapsed = int(args['Target']) - 1, 0
            else:
                fake.elapsed = p.seconds(args['Target'])
                fake.get(fake.index, offset=44 + int(fake.elapsed * 44100) * 4)
        elif action == 'Play':
            fake.state = 'PLAYING'
            fake.get(fake.index)
        elif action == 'Pause':
            fake.state = 'PAUSED_PLAYBACK'
        elif action in ('Next', 'Previous'):
            if action == 'Next':
                fake.index = min(len(fake.queue) - 1, fake.index + 1)
            elif fake.elapsed <= 3:
                fake.index = max(0, fake.index - 1)
            fake.elapsed = 0
            fake.get(fake.index)
        else:
            raise AssertionError(action)
        text = ''.join(f'<{k}>{escape(str(v))}</{k}>' for k, v in values.items())
        self.send(f'<s:Envelope xmlns:s="{p.SOAP}"><s:Body><u:{action}Response xmlns:u="{service}">'
                  f'{text}</u:{action}Response></s:Body></s:Envelope>')
        if action not in ('GetZoneGroupState', 'Browse', 'GetMediaInfo', 'GetPositionInfo', 'GetTransportInfo'):
            fake.notify()


class FakeSonos:
    def __init__(self):
        self.server = p.ThreadingHTTPServer(('127.0.0.1', 0), FakeHandler)
        self.server.fake = self
        self.location = f'http://127.0.0.1:{self.server.server_port}/description'
        self.queue, self.actions, self.gets = [], [], []
        self.uri, self.state, self.status = 'http://saved-original/', 'STOPPED', 'OK'
        self.elapsed, self.index, self.seq = 0, 0, 0
        self.callback, self.fail_action, self.pending = '', '', ''
        self.grouped, self.unsubscribed = False, False
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def get(self, index, offset=0):
        if not self.queue:
            return
        url = self.queue[index]['uri']
        # Only headers/one PCM frame needed to test queue/control dispatch here.
        # Full body/range/pacing and independently decoded audio are tested below.
        with p.OPENER.open(urllib.request.Request(url, headers={'Range': f'bytes={offset}-{offset + 43}'}), timeout=3) as r:
            r.read()
        self.gets.append((url, offset))

    def notify(self):
        if not self.callback:
            return
        change = f'<Event><InstanceID val="0"><TransportState val="{self.state}"/>'
        change += f'<TransportStatus val="{self.status}"/><CurrentTrack val="{self.index + 1}"/></InstanceID></Event>'
        body = '<e:propertyset xmlns:e="urn:schemas-upnp-org:event-1-0"><e:property><LastChange>'
        body += escape(change) + '</LastChange></e:property></e:propertyset>'
        try:
            p.fetch(self.callback, 'NOTIFY', body.encode(), {'NT': 'upnp:event', 'NTS': 'upnp:propchange',
                    'SID': 'uuid:fake-events', 'SEQ': str(self.seq)})
            self.seq += 1
        except (OSError, urllib.error.URLError):
            pass  # final UNSUBSCRIBE and server shutdown can race

    def advance(self, duration, experiment):
        if self.pending:
            experiment.device.call(self.pending)
            self.pending = ''
        observer = p.Observer(experiment.device, experiment.server, experiment.capture, experiment.base)
        for _ in range(int(duration)):
            if self.state == 'PLAYING' and self.queue:
                self.elapsed += 1
                if self.elapsed == 10 and self.index + 1 < len(self.queue):
                    self.get(self.index + 1)
                if self.elapsed >= p.seconds(self.queue[self.index]['duration']):
                    if self.index + 1 < len(self.queue):
                        self.index += 1
                        self.elapsed = 0
                        self.notify()
                    else:
                        self.state = 'STOPPED'
            observer.transport = experiment.device.call('GetTransportInfo')
            observer.sample()


class FakeLMS:
    def __init__(self, mode='stop'):
        self.mode, self.commands = mode, []
        self.sock = socket.socket()
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen()
        self.sock.settimeout(.1)
        self.port = self.sock.getsockname()[1]
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.stop.is_set():
            try:
                connection, _ = self.sock.accept()
            except socket.timeout:
                continue
            with connection:
                command = connection.recv(4096).decode().strip()
                self.commands.append(command)
                response = ('players 0 999 count:2 playerindex:0 playerid:aa name:Study%20(Sonos) '
                            'playerindex:1 playerid:bb name:Kitchen%20(Sonos)') if command.startswith('players ') else command[:-1] + self.mode
                connection.sendall((response + '\n').encode())

    def close(self):
        self.stop.set()
        self.thread.join()
        self.sock.close()


class ProbeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.cap = p.Capture(Path(self.temp.name) / 'run')
        self.http = p.ProbeHTTP(('127.0.0.1', 0), self.cap)
        self.base = f'http://127.0.0.1:{self.http.server_port}'
        self.thread = threading.Thread(target=self.http.serve_forever, daemon=True)
        self.thread.start()
        self.fake = FakeSonos()
        self.device = p.Device(self.fake.location, self.cap.event)

    def tearDown(self):
        self.fake.close()
        self.http.stopping.set()
        self.http.shutdown()
        self.http.server_close()
        self.thread.join()
        self.cap.close()
        self.temp.cleanup()

    def test_discovery_ssdp_topology_identity(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(('127.0.0.1', 0))
        def respond():
            data, address = sock.recvfrom(4096)
            self.assertIn(p.ST.encode(), data)
            reply = f'HTTP/1.1 200 OK\r\nST: {p.ST}\r\nLOCATION: {self.fake.location}\r\n\r\n'
            sock.sendto(reply.encode(), address)
        t = threading.Thread(target=respond)
        t.start()
        locations = p.ssdp_locations(.1, sock.getsockname())
        t.join()
        sock.close()
        device, rooms = p.discover(locations=locations)
        self.assertEqual(device.info['modelName'], 'Fake Play:1')
        self.assertEqual(device.info['softwareVersion'], '86.10-fake')
        self.assertEqual(device.udn, 'RINCON_FAKE')
        self.assertEqual(len(rooms), 2)
        with self.assertRaisesRegex(RuntimeError, 'missing or ambiguous'):
            p.discover(room='Missing', locations=locations)
        self.fake.grouped = True
        with self.assertRaisesRegex(RuntimeError, 'standalone'):
            p.discover(locations=locations)

    def test_lms_playing_paused_unknown_and_running_service_refused(self):
        fake = FakeLMS()
        query = lambda host, command: p.lms_query(host, command, fake.port)
        try:
            result = p.require_idle('127.0.0.1', query=query, service_check=lambda _: 'inactive')
            self.assertEqual(result['player'], 'aa')
            for mode in ('play', 'pause', 'unknown'):
                fake.mode = mode
                with self.assertRaisesRegex(RuntimeError, 'STOP playback'):
                    p.require_idle('127.0.0.1', query=query, service_check=lambda _: 'inactive')
            fake.mode = 'stop'
            with self.assertRaisesRegex(RuntimeError, 'stop the service'):
                p.require_idle('127.0.0.1', query=query, service_check=lambda _: 'active')
            self.assertFalse(any(' play' in c or ' stop' in c for c in fake.commands))
        finally:
            fake.close()

    def test_absent_lms_player_requires_confirmed_stopped_service_and_is_reported(self):
        query = mock.Mock(return_value=['players', '0', '999', 'count:0'])
        for state in ('inactive', 'failed'):
            result = p.require_idle('fake-lms', query=query, service_check=lambda _, state=state: state)
            self.assertTrue(result['idle'])
            self.assertIsNone(result['player'])
            self.assertEqual(result['verification'], 'stopped-service-player-absent')
            self.assertEqual(result['mode'], 'not-listed')
            self.assertEqual(query.call_args.args[1], 'players 0 999')
        experiment = p.Experiment(self.device, self.cap, self.http, self.base)
        p.write_report(self.cap, {'lms_check': result}, experiment, 'preflight')
        self.assertIn('confirmed stopped service guarantees idle', (self.cap.directory/'report.md').read_text())
        for state in ('active', 'activating', 'deactivating', 'unknown', ''):
            with self.assertRaises(RuntimeError):
                p.require_idle('fake-lms', query=query, service_check=lambda _, state=state: state)
        query.return_value = ['playerindex:0', 'playerid:aa', 'name:Study (Sonos)',
                              'playerindex:1', 'playerid:bb', 'name:Study (Sonos)']
        with self.assertRaisesRegex(RuntimeError, 'uniquely verify'):
            p.require_idle('fake-lms', query=query, service_check=lambda _: 'inactive')
        query.side_effect = OSError('LMS unreachable')
        with self.assertRaises(OSError):
            p.require_idle('fake-lms', query=query, service_check=lambda _: 'inactive')

    def test_prefetch_excludes_terminal_rewind_to_first_item(self):
        self.cap.requests.append(dict(mono=120, event='begin', method='GET', path='/b'))
        self.cap.positions.extend([
            dict(send_mono=128, recv_mono=128.1, uri='http://x/a', rel_s=28,
                 duration='0:00:30', state='PLAYING'),
            dict(send_mono=130, recv_mono=130.1, uri='http://x/b', rel_s=0,
                 duration='0:00:30', state='PLAYING'),
            dict(send_mono=220, recv_mono=220.1, uri='http://x/a', rel_s=0,
                 duration='0:00:30', state='PLAYING')])
        result = p.boundary_metrics(self.cap, ['/a', '/b'])[0]
        self.assertAlmostEqual(result['prefetch_lead_s'], 9.55)
        self.assertEqual(result['old_sample_mono'], 128.1)
        # Without successor evidence, a later different URI still ends visit 1.
        self.cap.positions[1]['uri'] = 'http://x/third'
        self.assertAlmostEqual(p.boundary_metrics(self.cap, ['/a','/b'])[0]['prefetch_lead_s'], 9.55)

    def test_three_field_runs_corrected_prefetch_replay(self):
        root = ROOT / 'tests/fixtures/queue-probe-boundaries-2026-10-12'
        expected = {
            '171905': [[3.198, 4.028], [3.804, 3.353], [3.269, 3.631]],
            '174601': [[3.681, 3.559], [4.035, 3.648], [3.105, 3.254]],
            '180503': [[3.122, 3.369], [3.605, 3.647], [3.800, 3.251],
                       [3.356, 3.256], [3.115, 3.517]]}
        for stamp, answers in expected.items():
            with (root/(stamp+'-requests.tsv')).open() as f:
                requests = list(csv.DictReader(f, delimiter='\t'))
            with (root/(stamp+'-positions.tsv')).open() as f:
                positions = list(csv.DictReader(f, delimiter='\t'))
            for r in requests:
                r['mono'] = float(r['mono'])
            for r in positions:
                for key in ('send_mono','recv_mono','rel_s'):
                    r[key] = float(r[key]) if r[key] else None
            self.cap.requests, self.cap.positions = requests, positions
            triples = [(1,2,3),(4,5,6),(17,18,19),(20,21,22),(23,24,25)]
            for ids, answer in zip(triples, answers):
                results = p.boundary_metrics(self.cap, [f'/audio/{n}.flac' for n in ids])
                for result, expected_lead in zip(results,answer):
                    self.assertAlmostEqual(result['prefetch_lead_s'], expected_lead, delta=.001)
                    low, high = result['prefetch_lead_interval_s']
                    self.assertGreater(low, 2)
                    self.assertLess(high, 5)

    def test_http_exact_length_ranges_head_errors_and_close_log(self):
        audio = p.generate_audio(44100, 24, 1, 'wav')
        self.http.media['/audio/a.wav'] = (audio, 0)
        for range_header, code, expected in [('', 200, audio.data), ('bytes=44-67', 206, audio.data[44:68]),
                ('bytes=44-', 206, audio.data[44:]), ('bytes=-24', 206, audio.data[-24:])]:
            request = urllib.request.Request(self.base + '/audio/a.wav', headers={'Range': range_header} if range_header else {})
            with p.OPENER.open(request) as r:
                self.assertEqual(r.status, code)
                self.assertEqual(int(r.headers['Content-Length']), len(expected))
                self.assertEqual(r.read(), expected)
                self.assertEqual(r.headers['Connection'], 'close')
        p.fetch(self.base + '/audio/a.wav', 'HEAD')
        for rng in ('bytes=999999999-', 'bytes=1-2,5-6', 'bad', 'bytes=-0'):
            with self.assertRaises(urllib.error.HTTPError) as e:
                p.fetch(self.base + '/audio/a.wav', headers={'Range': rng})
            self.assertEqual(e.exception.code, 416)
        with self.assertRaises(urllib.error.HTTPError) as e:
            p.fetch(self.base + '/missing')
        self.assertEqual(e.exception.code, 404)
        time.sleep(.02)
        closes = [r for r in self.cap.requests if r['event'] == 'close']
        self.assertEqual(len(closes), 10)
        self.assertTrue(all('bytes' in r and 'mono' in r for r in closes))
        self.assertEqual([r['bytes'] for r in closes[:4]], [len(audio.data), 24, len(audio.data)-44, 24])

    def test_pcm_wave_and_phase_continuity(self):
        for rate in (44100, 48000):
            for bits in (16, 24):
                audio = p.generate_audio(rate, bits, 2, 'wav')
                with wave.open(io.BytesIO(audio.data)) as w:
                    self.assertEqual((w.getframerate(), w.getsampwidth(), w.getnchannels(), w.getnframes()),
                                     (rate, bits//8, 2, rate*2))
                width = bits//8*2
                self.assertEqual(audio.data[44:44+rate*width], audio.data[44+rate*width:])
                self.assertEqual(audio.audio_seconds(44 + 12 * rate * width), 2)  # bounded by EOF
                self.assertEqual(audio.data[44:44+width], b'\0'*width)

    def test_flac_decodes_with_independent_libflac_and_md5(self):
        # The project already requires libFLAC; the probe itself never loads it.
        lib = C.CDLL(ctypes.util.find_library('FLAC'))
        lib.FLAC__stream_decoder_new.restype = C.c_void_p
        lib.FLAC__stream_decoder_get_blocksize.argtypes = [C.c_void_p]
        lib.FLAC__stream_decoder_set_md5_checking.argtypes = [C.c_void_p, C.c_bool]
        lib.FLAC__stream_decoder_process_until_end_of_stream.argtypes = [C.c_void_p]
        lib.FLAC__stream_decoder_finish.argtypes = [C.c_void_p]
        lib.FLAC__stream_decoder_delete.argtypes = [C.c_void_p]
        WRITE = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_void_p, C.POINTER(C.POINTER(C.c_int32)), C.c_void_p)
        ERROR = C.CFUNCTYPE(None, C.c_void_p, C.c_int, C.c_void_p)
        lib.FLAC__stream_decoder_init_file.argtypes = [C.c_void_p, C.c_char_p, WRITE, C.c_void_p, ERROR, C.c_void_p]
        for rate in (44100, 48000):
            for bits in (16, 24):
                audio = p.generate_audio(rate, bits, 1, 'flac')
                path = Path(self.temp.name) / 'test.flac'
                path.write_bytes(audio.data)
                decoded, errors = bytearray(), []
                @WRITE
                def write(decoder, frame, samples, client):
                    n = lib.FLAC__stream_decoder_get_blocksize(decoder)
                    for i in range(n):
                        for channel in (0, 1):
                            decoded.extend(samples[channel][i].to_bytes(bits//8, 'little', signed=True))
                    return 0
                @ERROR
                def error(decoder, code, client):
                    errors.append(code)
                decoder = lib.FLAC__stream_decoder_new()
                try:
                    self.assertTrue(lib.FLAC__stream_decoder_set_md5_checking(decoder, True))
                    self.assertEqual(lib.FLAC__stream_decoder_init_file(decoder, os.fsencode(path), write, None, error, None), 0)
                    self.assertTrue(lib.FLAC__stream_decoder_process_until_end_of_stream(decoder))
                    self.assertTrue(lib.FLAC__stream_decoder_finish(decoder))
                    self.assertEqual(errors, [])
                    self.assertEqual(bytes(decoded), p.tone_second(rate, bits)[0])
                    self.assertEqual(audio.audio_seconds(len(audio.data)), 1)
                finally:
                    lib.FLAC__stream_decoder_delete(decoder)

    def test_paced_http_is_rate_limited_across_reconnects(self):
        for kind in ('wav', 'flac'):
            audio = p.generate_audio(44100, 16, 1, kind)
            self.assertAlmostEqual(audio.audio_seconds(len(audio.data)), 1)
            for speed in (10, 12):
                path = '/audio/' + kind + str(speed)
                self.http.media[path] = (audio, speed)
                start = time.monotonic()
                with p.OPENER.open(self.base + path) as r:
                    self.assertEqual(r.read(), audio.data)
                elapsed = time.monotonic() - start
                self.assertGreaterEqual(elapsed, 1/speed - .015)
                self.assertLess(elapsed, .5)
                before = self.http.media_origin[path]
                p.fetch(self.base + path, headers={'Range': 'bytes=44-47'})
                self.assertEqual(self.http.media_origin[path], before)

    def test_ten_second_pause_does_not_force_http_disconnect(self):
        audio = p.generate_audio(44100, 16, 60, 'wav')
        self.http.media['/audio/pause'] = (audio, 0)
        client = http.client.HTTPConnection('127.0.0.1', self.http.server_port, timeout=20)
        client.request('GET', '/audio/pause')
        response = client.getresponse()
        # Stop consuming a body larger than the socket send buffer, as a paused
        # speaker can do. The header-read timeout must not truncate the body.
        time.sleep(10)
        self.assertEqual(response.read(), audio.data)
        client.close()

    def test_prefetch_interval_position_reset_jumps_stalls_and_errors(self):
        self.cap.positions.extend([
            dict(send_mono=128, recv_mono=128.1, step=1, track='1', uri='http://x/a', rel_s=28,
                 duration='0:00:30', state='PLAYING', status='OK', error=''),
            dict(send_mono=130, recv_mono=130.1, step=1, track='2', uri='http://x/b', rel_s=0,
                 duration='0:00:30', state='PLAYING', status='OK', error=''),
            dict(send_mono=134, recv_mono=134.1, step=1, track='2', uri='http://x/b', rel_s=0,
                 duration='0:00:30', state='PLAYING', status='OK', error=''),
            dict(send_mono=134.25, recv_mono=134.35, step=1, track='2', uri='http://x/b', rel_s=5,
                 duration='0:00:30', state='PLAYING', status='ERROR_CORRUPT_FILE', error='')])
        self.cap.requests.extend([dict(mono=120, event='begin', method='GET', path='/b'),
                                  dict(mono=121, event='progress', path='/b', audio_s=20)])
        metric = p.trial_metrics(self.cap, ['/a', '/b'], 0)
        b = metric['boundaries'][0]
        self.assertAlmostEqual(b['prefetch_lead_s'], 9.55)
        self.assertEqual(b['prefetch_lead_interval_s'], [9, 10.099999999999994])
        self.assertEqual(metric['item_transitions'][0]['first_rel_s'], 0)
        self.assertTrue(metric['possible_stalls'])
        self.assertTrue(metric['large_jumps'])
        self.assertIn('ERROR_CORRUPT_FILE', metric['errors'])
        self.assertFalse(metric['backwards'])  # legitimate per-item reset

    def test_observer_4hz_events_and_unsubscribe(self):
        observer = p.Observer(self.device, self.http, self.cap, self.base)
        observer.start()
        time.sleep(1.05)
        observer.close()
        rows = self.cap.positions
        self.assertGreaterEqual(len(rows), 4)
        intervals = [b['send_mono']-a['send_mono'] for a, b in zip(rows, rows[1:])]
        self.assertTrue(all(.18 <= dt <= .4 for dt in intervals), intervals)
        self.assertTrue(any(e['kind'] == 'notify' for e in self.cap.events))
        self.assertTrue(self.fake.unsubscribed)
        with self.assertRaises(urllib.error.HTTPError):
            p.fetch(self.base + '/events', 'NOTIFY', b'<bad/>',
                    {'NT': 'upnp:event', 'NTS': 'upnp:propchange', 'SID': 'wrong', 'SEQ': '1'})

    def test_optional_crossfade_restores_setting_and_reports_coarse_overlap(self):
        experiment = p.Experiment(self.device, self.cap, self.http, self.base,
                                  ask=lambda _: 'y overlapping beeps', wait=lambda _: None)
        experiment.wait = lambda duration: self.fake.advance(duration, experiment)
        self.fake.crossfade = 0
        with contextlib.redirect_stdout(io.StringIO()):
            experiment.step8()
        self.assertEqual(self.fake.crossfade, 0)
        trial = experiment.results[8][0]
        self.assertEqual(trial['crossfade_mode'], 1)
        paths = trial['metrics']['expected_paths']
        self.assertNotEqual(self.http.media[paths[0]][0].data[44:2044],
                            self.http.media[paths[1]][0].data[44:2044])
        self.assertEqual(len(trial['overlap_estimates']), 2)
        self.assertIn('cannot prove acoustic', trial['measurement_limit'])
        modes = [a[1]['CrossfadeMode'] for a in self.fake.actions if a[0] == 'SetCrossfadeMode']
        self.assertEqual(modes, ['1', '0'])
        # A synthetic 5 s early handoff must be recognized independently of
        # how early the successor was fetched.
        self.cap.positions = [dict(uri='/a', state='PLAYING', rel_s=24,
            duration='0:00:30', send_mono=124, recv_mono=124.1),
            dict(uri='/b', state='PLAYING', rel_s=0, duration='0:00:30',
                 send_mono=125, recv_mono=125.1)]
        overlap = p.overlap_metrics(self.cap, ['/a', '/b'])[0]
        self.assertAlmostEqual(overlap['overlap_mid_s'], 5)
        self.assertLess(overlap['overlap_low_s'], 5)
        self.assertGreater(overlap['overlap_high_s'], 5)
        p.write_report(self.cap, {}, experiment)
        report = (self.cap.directory/'report.md').read_text()
        self.assertIn('Q8: Sonos native crossfade', report)
        self.assertIn('overlap_estimates', report)
        experiment.cleanup()

    def test_length_mismatch_full_get_short_long_range_and_diagnostic_report(self):
        experiment = p.Experiment(self.device, self.cap, self.http, self.base,
                                  ask=lambda _: 'n deliberate missing tail', wait=lambda _: None)
        # Exercise all trial construction/queue actions without a fake HTTP
        # parser rejecting the intentionally truncated response before sampling.
        with mock.patch.object(self.fake, 'get', return_value=None), contextlib.redirect_stdout(io.StringIO()):
            experiment.step9()
        trials = experiment.results[9]
        self.assertEqual(len(trials), 4)
        self.assertEqual([t['length_fault']['available_seconds'] for t in trials], [18, 18, 22, 22])
        for trial in trials:
            path = trial['metrics']['expected_paths'][0]
            audio, _ = self.http.media[path]
            conn = http.client.HTTPConnection('127.0.0.1', self.http.server_port)
            conn.request('GET', path)
            response = conn.getresponse()
            self.assertEqual(int(response.getheader('Content-Length')), audio.entity_length)
            if len(audio.data) < audio.entity_length:
                with self.assertRaises(http.client.IncompleteRead) as caught:
                    response.read()
                self.assertEqual(len(caught.exception.partial), len(audio.data))
            else:
                self.assertEqual(len(response.read()), audio.entity_length)
            conn.close()
            if len(audio.data) > audio.entity_length:
                with socket.create_connection(('127.0.0.1', self.http.server_port)) as raw:
                    raw.sendall(f'GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'.encode())
                    chunks = []
                    while chunk := raw.recv(65536):
                        chunks.append(chunk)
                body = b''.join(chunks).split(b'\r\n\r\n', 1)[1]
                self.assertEqual(len(body), len(audio.data))
            conn = http.client.HTTPConnection('127.0.0.1', self.http.server_port)
            conn.request('GET', path, headers={'Range': 'bytes=0-43'})
            response = conn.getresponse()
            self.assertEqual(response.status, 206)
            self.assertEqual(len(response.read()), 44)
            conn.close()
        time.sleep(.05)
        logs = [r for r in self.cap.requests if r['event'] == 'close' and 'deliberate-length-mismatch' in r.get('detail', '')]
        self.assertTrue(any(int(r['bytes']) > self.http.media[r['path']][0].entity_length for r in logs))
        self.assertTrue(p.verdict(9, trials).startswith('DIAGNOSTIC ONLY'))
        p.write_report(self.cap, {}, experiment)
        report = (self.cap.directory/'report.md').read_text()
        self.assertIn('Q9: Deliberate finite length mismatch', report)
        self.assertIn('next_item_clean', report)
        experiment.cleanup()

    def test_crossfade_fault_retains_original_for_cleanup_retry(self):
        experiment = p.Experiment(self.device, self.cap, self.http, self.base,
                                  ask=lambda _: 'n unsupported', wait=lambda _: None)
        self.fake.fail_action = 'SetCrossfadeMode'
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(RuntimeError):
            experiment.step8()
        self.assertEqual(experiment.original_crossfade, 0)
        experiment.dirty = True
        self.fake.queue = [{'uri': 'test', 'duration': '0:00:01'}]
        with self.assertRaises(RuntimeError):
            experiment.cleanup()
        self.assertEqual(self.fake.queue, [])
        self.assertFalse(experiment.dirty)
        self.fake.fail_action = ''
        experiment.cleanup()
        self.assertIsNone(experiment.original_crossfade)

    def test_all_six_steps_real_soap_queue_http_and_report(self):
        def ask(text):
            if 'then tap' in text:
                self.fake.pending = 'Next' if 'Next' in text else 'Previous'
                return ''
            return 'y fake listening observation'
        experiment = p.Experiment(self.device, self.cap, self.http, self.base, ask=ask)
        experiment.wait = lambda duration: self.fake.advance(duration, experiment)
        initial = self.device.snapshot()
        with contextlib.redirect_stdout(io.StringIO()):
            for method in (experiment.step1, experiment.step2, experiment.step3,
                           experiment.step4, experiment.step5, experiment.step6):
                method()
            experiment.cleanup()
        self.assertFalse(experiment.dirty)
        self.assertEqual(self.fake.queue, [])
        self.assertEqual(set(experiment.results), set(range(1, 7)))
        self.assertEqual(len(experiment.results[3]), 4)
        self.assertEqual(len(experiment.results[4]), 2)
        for q in (1, 2, 6):
            result = experiment.results[q][0]
            self.assertEqual(len(result['metrics']['boundaries']), 2)
            self.assertEqual(len(result['metrics']['visited_paths']), 3)
            self.assertFalse(result['error'])
        self.assertTrue(p.verdict(5, experiment.results[5]).startswith('GO —'))
        self.assertTrue(experiment.results[5][0]['metrics']['ranges'])
        seek = next(r for r in experiment.results[5][0]['metrics']['ranges'] if r.get('wav_time_s') == 12)
        self.assertTrue(seek['frame_aligned'])
        self.assertEqual(seek['seek_target_delta_s'], 0)
        controls = experiment.results[5][0]['controls']
        self.assertEqual([c['label'] for c in controls], ['UPnP seek to 12 seconds', 'resume after ten seconds',
                         'UPnP Next', 'UPnP Previous', 'App Next', 'App Previous', 'FLAC seek to 12 seconds', 'FLAC resume after ten seconds'])
        self.assertTrue(any(e['kind'] == 'pause-ten-seconds' for e in self.cap.events))
        p.write_report(self.cap, initial, experiment)
        report = (self.cap.directory/'report.md').read_text()
        self.assertEqual(len(re.findall(r'^\| Q[1-6]:', report, re.M)), 6)
        self.assertIn('http://saved-original/', report)
        self.assertIn('fake listening observation', report)
        self.assertIn('max_socket_write_ahead_s', report)
        self.assertIn('first_get_mono', report)
        self.assertIn('old queue contents were NOT backed up', report)
        actions = {a[0] for a in self.fake.actions}
        self.assertTrue({'Next', 'Previous', 'Seek', 'AddURIToQueue', 'RemoveAllTracksFromQueue'} <= actions)
        self.assertFalse({'SetVolume', 'Stop', 'ContentDirectory', 'SetNextAVTransportURI'} & actions)

    def test_fault_unknown_verdict_partial_cleanup_and_abort_report(self):
        experiment = p.Experiment(self.device, self.cap, self.http, self.base, ask=lambda _: 'n error heard', wait=lambda _: None)
        self.fake.fail_action = 'AddURIToQueue'
        with contextlib.redirect_stdout(io.StringIO()):
            result = experiment.trial(1, 'failure', [('x', p.generate_audio(44100,16,1,'wav'), 0)], 'listen')
            self.assertTrue(experiment.dirty)
            self.fake.fail_action = ''
            experiment.cleanup()
        self.assertTrue(result['error'])
        self.assertTrue(p.verdict(1, [result]).startswith('NO-GO'))
        self.assertEqual(p.verdict(2, []), 'NOT RUN')
        p.write_report(self.cap, {}, experiment, 'aborted', 'injected')
        self.assertEqual(len(re.findall(r'^\| Q[1-6]:', (self.cap.directory/'report.md').read_text(), re.M)), 6)


    def run_main(self, experiment_class=None, group=False, replace=False, extra=()):
        directory = Path(self.temp.name) / 'main-run'
        capture_class = p.Capture
        args = ['--lms', '127.0.0.1', '--lan-ip', '127.0.0.1', '--port']
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            args.append(str(sock.getsockname()[1]))
        if replace:
            args.append('--replace-queue')
        if group:
            args += ['--group-with', 'Kitchen']
        args += list(extra)
        output = io.StringIO()
        def group_input(_):
            self.fake.grouped = True
            return ''
        with mock.patch.object(p, 'Capture', side_effect=lambda _: capture_class(directory)), \
             mock.patch.object(p, 'require_idle', return_value={'mode': 'stop'}), \
             mock.patch.object(p, 'discover', return_value=(self.device, p.topology(self.device))), \
             mock.patch.object(p, 'Experiment', experiment_class or p.Experiment), \
             mock.patch('builtins.input', side_effect=group_input), \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            result = p.main(args)
        return result, (directory / 'report.md').read_text(), output.getvalue()

    def test_main_opt_in_crossfade_and_mismatch_steps_order_and_report(self):
        calls = []
        class Optional(p.Experiment):
            def step1(inner):
                pass
            step2 = step3 = step4 = step5 = step6 = step1
            def step8(inner):
                calls.append(8)
                inner.results[8] = []
            def step9(inner):
                calls.append(9)
                inner.results[9] = []
        result, report, _ = self.run_main(Optional, extra=('--native-crossfade', '--length-mismatch'))
        self.assertEqual(result, 0)
        self.assertEqual(calls, [8, 9])
        self.assertIn('| Q8: Sonos native crossfade | NOT RUN', report)
        self.assertIn('| Q9: Deliberate finite length mismatch | NOT RUN', report)

    def test_main_refuses_nonempty_queue_without_authorization(self):
        self.fake.queue = [{'uri': 'http://saved/', 'duration': '0:00:30'}]
        result, report, output = self.run_main()
        self.assertEqual(result, 1)
        self.assertIn('Original queue is nonempty', report)
        self.assertIn('Fake Play:1', report)
        self.assertFalse(any(a[0] == 'RemoveAllTracksFromQueue' for a in self.fake.actions))
        self.assertEqual(len(self.fake.queue), 1)
        self.assertEqual(output.count('pct pull 113 '), 4)

    def test_main_interruption_cleans_partial_queue_and_reports(self):
        class Interrupted(p.Experiment):
            def step1(inner):
                inner.capture.step = 1
                inner.queue([('interrupt', p.generate_audio(44100, 16, 1, 'wav'), 0)])
                raise EOFError('owner input closed')
        result, report, output = self.run_main(Interrupted)
        self.assertEqual(result, 1)
        self.assertEqual(self.fake.queue, [])
        self.assertIn('EOFError: owner input closed', report)
        self.assertIn('dirty: False', report)
        self.assertEqual(output.count('pct pull 113 '), 4)

    def test_main_optional_group_runs_only_after_verified_owner_grouping(self):
        fake = self.fake
        class Grouped(p.Experiment):
            def __init__(inner, *args):
                super().__init__(*args, ask=lambda _: 'y clean')
                inner.wait = lambda duration: fake.advance(duration, inner)
            def step1(inner):
                pass
            step2 = step3 = step4 = step5 = step6 = step1
        result, report, output = self.run_main(Grouped, group=True)
        self.assertEqual(result, 0)
        self.assertIn('| Q7: Grouped gapless playback | GO', report)
        self.assertIn('both rate directions', report)
        self.assertEqual(self.fake.queue, [])
        self.assertIn('Restore grouping', output)
        self.assertTrue(any(a[0] == 'RemoveAllTracksFromQueue' for a in self.fake.actions))


if __name__ == '__main__':
    unittest.main(verbosity=2)
