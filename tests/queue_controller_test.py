"""Queue controller + real finite HTTP store against a loopback Sonos coordinator."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import copy
import os
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import queue
import shlex
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from urllib.parse import urlsplit
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape, quoteattr

ROOT = Path(__file__).resolve().parents[1]
SOAP = 'http://schemas.xmlsoap.org/soap/envelope/'
SERVICES = {name: ('urn:schemas-sonos-com:service:Queue:1' if name == 'Queue' else
                  'urn:schemas-upnp-org:service:'+name+':1')
            for name in ('AVTransport', 'Queue', 'ContentDirectory', 'ZoneGroupTopology')}
PATHS = {name: '/advertised/'+name for name in SERVICES}


def didl(entries, saved=False):
    body = ''
    for item in entries:
        if saved:
            body += ('<container id='+quoteattr(item['id'])+'><dc:title>'+escape(item['title'])+
                     '</dc:title><dc:date>'+item['date']+'</dc:date></container>')
        else:
            body += '<item><res>'+escape(item['uri'])+'</res></item>'
    return ('<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" '
            'xmlns:dc="http://purl.org/dc/elements/1.1/">'+body+'</DIDL-Lite>')


class Coordinator(ThreadingHTTPServer):
    daemon_threads = True
    def __init__(self, address=('127.0.0.1', 0), handler=None):
        super().__init__(address, handler or Handler)
        self.lock = threading.RLock()
        self.original = [{'uri': 'http://foreign.invalid/original.flac'}]
        self.items = copy.deepcopy(self.original)
        self.saved = [dict(id='SQ:1', title='Unrelated favorite', date='2026-10-17T12:00:00Z')]
        self.saved += [dict(id='SQ:'+str(i+2), title=f'Before YeneY 2026-10-18 12:{i:02}',
                            date=f'2026-10-18T12:{i:02}:00Z') for i in range(7)]
        self.update, self.saved_update, self.next_id = 20, 1, 9
        self.crossfade, self.playing, self.uri = True, False, ''
        self.position = dict(Track='0', TrackURI='', RelTime='00:00:00')
        self.transport_state = 'STOPPED'
        self.play_mode = 'NORMAL'
        self.requests, self.errors, self.snapshots = [], [], {}
        self.fail_save = self.hide_save = self.race_add = self.ignore_crossfade = self.race_page = False
        self.last_save, self.verified_save = None, False
        self.busy, self.max_busy, self.grouped = 0, 0, False
        self.worker = threading.Thread(target=self.serve_forever)
        self.worker.start()

    def close(self):
        self.shutdown(); self.server_close(); self.worker.join()
        assert not self.errors, self.errors

    def add_foreign(self, uri='http://foreign.invalid/added-during-play.flac'):
        with self.lock:
            self.items.append({'uri': uri}); self.update += 1

    def actions(self, name):
        with self.lock:
            return [r for r in self.requests if r[0] == name]


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *_): pass

    def send(self, body, status=200):
        body = body.encode()
        self.send_response(status)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers(); self.wfile.write(body)

    def do_GET(self):
        try:
            assert self.path == '/description.xml'
            services = ''.join('<service><serviceType>'+urn+'</serviceType><controlURL>'+PATHS[name]+
                               '</controlURL></service>' for name, urn in SERVICES.items())
            self.send('<root><device><UDN>uuid:RINCON_TEST</UDN><serviceList>'+services+
                      '</serviceList></device></root>')
        except Exception as error:
            self.server.errors.append(repr(error)); self.send('', 500)

    def do_POST(self):
        s = self.server
        with s.lock:
            s.busy += 1; s.max_busy = max(s.max_busy, s.busy)
        try:
            time.sleep(.001)  # reveal overlapping controller RPCs without slowing the suite
            node = ET.fromstring(self.rfile.read(int(self.headers['Content-Length']))).find('{'+SOAP+'}Body')[0]
            urn, action = node.tag[1:].split('}')
            service = next(k for k, v in SERVICES.items() if v == urn)
            args = {child.tag: child.text or '' for child in node}
            assert self.path == PATHS[service]
            assert self.headers['SOAPAction'] == '"'+urn+'#'+action+'"'
            with s.lock:
                s.requests.append((action, args, service, time.monotonic()))
                result, fault = self.action(service, action, args)
                s.busy -= 1
            if fault:
                self.send(f'<s:Envelope xmlns:s="{SOAP}"><s:Body><s:Fault><detail><UPnPError>'
                          f'<errorCode>{fault}</errorCode><errorDescription>fixture rejection</errorDescription>'
                          '</UPnPError></detail></s:Fault></s:Body></s:Envelope>', 500)
            else:
                values = ''.join('<'+k+'>'+escape(str(v))+'</'+k+'>' for k, v in result.items())
                self.send(f'<s:Envelope xmlns:s="{SOAP}"><s:Body><u:{action}Response xmlns:u="{urn}">'
                          f'{values}</u:{action}Response></s:Body></s:Envelope>')
        except Exception as error:
            with s.lock:
                s.errors.append(repr(error)); s.busy = max(0, s.busy-1)
            self.send('', 500)

    def action(self, service, action, a):
        s = self.server
        if action == 'GetZoneGroupState':
            other = ('<ZoneGroupMember UUID="RINCON_OTHER" ZoneName="Other" Location="http://127.0.0.1:'+str(s.server_port)+'/description.xml"/>') if s.grouped else ''
            topology = ('<ZoneGroupState><ZoneGroups><ZoneGroup Coordinator="RINCON_TEST" ID="test">'
                        '<ZoneGroupMember UUID="RINCON_TEST" ZoneName="Study" Location="http://127.0.0.1:'+str(s.server_port)+
                        '/description.xml"/>'+other+'</ZoneGroup></ZoneGroups></ZoneGroupState>')
            return {'ZoneGroupState': topology}, None
        if action == 'Browse':
            if service == 'Queue':
                assert a['QueueID'] == '0'
                if s.race_page and int(a['StartingIndex']):
                    s.race_page = False; s.update += 1
                entries, revision, saved = s.items, s.update, False
            else:
                assert service == 'ContentDirectory' and a['ObjectID'] == 'SQ:'
                assert a['BrowseFlag'] == 'BrowseDirectChildren'
                entries = [p for p in s.saved if not (s.hide_save and p['id'] == s.last_save)]
                revision, saved = s.saved_update, True
                if any(p['id'] == s.last_save for p in entries): s.verified_save = True
            start, count = int(a['StartingIndex']), int(a['RequestedCount'])
            page = entries[start:start+count]
            return dict(Result=didl(page, saved), NumberReturned=len(page), TotalMatches=len(entries), UpdateID=revision), None
        assert service == 'AVTransport' or action == 'DestroyObject'
        if action == 'SaveQueue':
            if s.fail_save: return {}, '701'
            assert a['Title'].startswith('Before YeneY ') and not a['ObjectID']
            item = dict(id='SQ:'+str(s.next_id), title=a['Title'], date='2026-10-19T12:34:00Z')
            s.next_id += 1; s.saved.append(item); s.saved_update += 1
            s.last_save, s.verified_save = item['id'], False
            s.snapshots[item['id']] = copy.deepcopy(s.items)
            return {'AssignedObjectID': item['id']}, None
        if action == 'DestroyObject':
            item = next(p for p in s.saved if p['id'] == a['ObjectID'])
            assert item['title'].startswith('Before YeneY ') and s.verified_save
            s.saved.remove(item); s.saved_update += 1
            return {}, None
        if action == 'GetMediaInfo': return {'CurrentURI': s.uri}, None
        if action == 'GetTransportSettings': return {'PlayMode': s.play_mode}, None
        if action == 'GetPositionInfo': return dict(s.position), None
        if action == 'GetTransportInfo': return {'CurrentTransportState': s.transport_state}, None
        if action == 'GetCrossfadeMode': return {'CrossfadeMode': int(s.crossfade)}, None
        if action == 'SetCrossfadeMode':
            assert s.verified_save
            if not s.ignore_crossfade: s.crossfade = a['CrossfadeMode'] == '1'
            return {}, None
        if action == 'RemoveTrackRangeFromQueue':
            if int(a['UpdateID']) != s.update: return {}, '803'
            assert s.verified_save
            first, count = int(a['StartingIndex'])-1, int(a['NumberOfTracks'])
            del s.items[first:first+count]; s.update += 1
            return {'NewUpdateID': s.update}, None
        if action == 'AddURIToQueue':
            if s.race_add:
                s.race_add = False; s.add_foreign()
            assert not s.crossfade and a['InstanceID'] == '0'
            assert a['DesiredFirstTrackNumberEnqueued'] == '0' and a['EnqueueAsNext'] == '0'
            uri = a['EnqueuedURI']; u = urlsplit(uri)
            assert u.hostname == '127.0.0.1' and u.path == '/music/queue.wav'
            metadata = ET.fromstring(a['EnqueuedURIMetaData'])
            item = next(iter(metadata)); fields = {n.tag.rsplit('}', 1)[-1]: n.text for n in item}
            resource = next(n for n in item if n.tag.rsplit('}', 1)[-1] == 'res')
            assert fields['class'] == 'object.item.audioItem.musicTrack'
            assert fields['title'].startswith(getattr(s, 'expected_title_prefix', 'Artist <one> - Title & '))
            assert fields['album'] in getattr(s, 'expected_albums', ['Album & fixture'])
            assert resource.text == uri and resource.attrib['protocolInfo'] == 'http-get:*:audio/wav:*'
            assert resource.attrib['bitsPerSample'] == '24' and resource.attrib['nrAudioChannels'] == '2'
            s.items.append(dict(uri=uri, metadata=fields, resource=resource.attrib)); s.update += 1
            return dict(NumTracksAdded=1, FirstTrackNumberEnqueued=len(s.items), NewQueueLength=len(s.items), NewUpdateID=s.update), None
        if action == 'SetAVTransportURI':
            assert a['CurrentURI'] == 'x-rincon-queue:RINCON_TEST#0' and s.items
            s.uri = a['CurrentURI']; return {}, None
        if action == 'Seek':
            if a['Unit'] == 'TRACK_NR':
                track = int(a['Target'])
                assert 1 <= track <= len(s.items)
                s.position = dict(Track=str(track), TrackURI=s.items[track-1]['uri'], RelTime='00:00:00')
            else:
                assert a['Unit'] == 'REL_TIME'
                s.position['RelTime'] = ':'.join(f'{int(v):02}' for v in a['Target'].split(':'))
            return {}, None
        if action in ('Pause', 'Stop'):
            s.transport_state = 'PAUSED_PLAYBACK' if action == 'Pause' else 'STOPPED'
            return {}, None
        if action == 'Play':
            assert s.uri == 'x-rincon-queue:RINCON_TEST#0' and not s.crossfade
            s.playing = True
            if getattr(s, 'follow_controls', False):
                s.transport_state = 'PLAYING'
            return {}, None
        raise AssertionError((service, action, a))


class Driver:
    def __init__(self, coordinator, fixture='queue-controller-fixture', extra=()):
        self.tmp = tempfile.TemporaryDirectory(prefix='yeney-controller-')
        self.log = open(Path(self.tmp.name)/'controller.log', 'w+')
        try:
            self.proc = subprocess.Popen([str(ROOT/fixture), self.tmp.name, str(coordinator.server_port), *map(str, extra)],
                                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log, text=True,
                                         env=dict(os.environ, YENEY_TITLE_FORMAT='artist-title'))
        except BaseException:
            self.log.close();self.tmp.cleanup()
            raise
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=lambda: [self.lines.put(line.rstrip()) for line in self.proc.stdout], daemon=True)
        self.reader.start()
        ready = self.read()
        while not ready.startswith('READY '):
            self.log.write(ready+'\n'); ready = self.read()
        self.port = int(ready.split()[1])

    def read(self):
        try: return self.lines.get(timeout=12)
        except queue.Empty:
            self.log.flush(); raise AssertionError(Path(self.log.name).read_text())

    def command(self, text, expected='OK'):
        self.proc.stdin.write(text+'\n'); self.proc.stdin.flush()
        result = self.read()
        assert result.startswith(expected), (text, result, Path(self.log.name).read_text())
        return result

    def status(self):
        fields = shlex.split(self.command('STATUS', 'STATUS'))
        return dict(active=bool(int(fields[1])), epoch=int(fields[2]), mirrored=int(fields[3]), reason=fields[4], fault=fields[5])

    def wait(self, count):
        end = time.monotonic()+5
        while time.monotonic() < end:
            state = self.status()
            assert not state['fault'], state
            if state['mirrored'] == count: return state
            time.sleep(.015)
        raise AssertionError(self.status())

    def put(self, id, kind='P', seconds=8, produced=None, rate=44100):
        frames = rate*seconds
        self.command(f'PUT {id} {kind} {frames} {frames if produced is None else produced} {rate}')

    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.write('EXIT\n'); self.proc.stdin.flush()
            while self.read() != 'BYE': pass
            self.proc.wait(timeout=10)
        assert self.proc.returncode == 0
        self.proc.stdin.close(); self.proc.stdout.close(); self.reader.join(timeout=2)
        self.log.close(); self.tmp.cleanup()


def request(uri, method='GET', byte_range=None):
    u = urlsplit(uri); assert u.hostname == '127.0.0.1'
    connection = http.client.HTTPConnection(u.hostname, u.port, timeout=3)
    connection.request(method, u.path+'?'+u.query, headers={} if byte_range is None else {'Range': byte_range})
    response = connection.getresponse()
    return connection, response


def expected(id, rate, seconds=8):
    data = struct.pack('<4sI4s4sIHHIIHH4sI', b'RIFF', 36+rate*seconds*6, b'WAVE', b'fmt ', 16,
                       1, 2, rate, rate*6, 6, 24, b'data', rate*seconds*6)
    sample = 0x123400+id
    return data+(sample.to_bytes(3, 'little', signed=True)+(-sample).to_bytes(3, 'little', signed=True))*rate*seconds


class ProbeClient:
    """40x test clock, real sockets, 9x paced bursts and a 3.5 s prefetch lead."""
    speed = 40
    def __init__(self): self.offset, self.wall, self.prefetch, self.speeds = 0, time.monotonic(), [], []
    def now(self): return self.offset+(time.monotonic()-self.wall)*self.speed
    def advance(self, position):
        assert position >= self.now(), (position, self.now())
        self.offset, self.wall = position, time.monotonic()
    def fetch(self, uri, id, rate):
        content = expected(id, rate)
        connection, response = request(uri, 'HEAD')
        assert response.status == 200 and int(response.getheader('Content-Length')) == len(content)
        response.read(); connection.close()
        connection, response = request(uri, byte_range='bytes=0-65535')
        assert response.status == 206 and response.read() == content[:65536]
        assert response.getheader('Content-Range') == f'bytes 0-65535/{len(content)}'
        connection.close()  # reconnect to the same immutable entity
        connection, response = request(uri, byte_range='bytes=65536-')
        started = time.monotonic(); body = bytearray(); quantum = rate*6//10
        while True:
            data = response.read(quantum)
            if not data: break
            body.extend(data)
            budget = len(body)/(rate*6*9*self.speed)
            time.sleep(max(0, budget-(time.monotonic()-started)))
        elapsed = time.monotonic()-started
        self.speeds.append(len(body)/(rate*6*elapsed*self.speed))
        assert bytes(body) == content[65536:]
        connection.close()
        # Independent suffix and unaligned readers do not disturb the first reader.
        for byte_range, target in [('bytes=-17', content[-17:]), ('bytes=43-101', content[43:102])]:
            connection, response = request(uri, byte_range=byte_range)
            assert response.status == 206 and response.read() == target
            connection.close()
        assert self.speeds[-1] <= 9.1, self.speeds


class ControllerTests(unittest.TestCase):
    def setUp(self): self.speaker = Coordinator(); self.driver = Driver(self.speaker)
    def tearDown(self):
        try: self.driver.close()
        finally: self.speaker.close()

    def test_window_publication_takeover_and_readers(self):
        s, d = self.speaker, self.driver
        d.put(9); d.put(10, produced=2*44100)
        d.put(11, 'U'); d.put(12); d.put(13, rate=48000)
        self.assertIn('queue-room-already-controlled', d.command('DUPLICATE', 'ERR'))
        self.assertIn('group-rate-undecided', d.command('GROUP', 'ERR'))
        d.command('PLAY 1 10 9 10 11 12 13'); d.wait(1)
        self.assertEqual(s.snapshots['SQ:9'], s.original)
        self.assertEqual(len([p for p in s.saved if p['title'].startswith('Before YeneY ')]), 5)
        self.assertTrue(any(p['title'] == 'Unrelated favorite' for p in s.saved))
        self.assertEqual({r[1]['ObjectID'] for r in s.actions('DestroyObject')}, {'SQ:2', 'SQ:3', 'SQ:4'})
        self.assertFalse(s.crossfade)
        # Progressive promise is already fetchable before its final PCM exists.
        first = s.items[0]['uri']
        connection, response = request(first, 'HEAD')
        self.assertEqual(int(response.getheader('Content-Length')), 44+8*44100*6)
        connection.close()
        time.sleep(.12); self.assertEqual(d.status()['mirrored'], 1)
        self.assertEqual(len(s.actions('AddURIToQueue')), 1)
        d.command('APPEND 10 '+str(6*44100)); d.command('FINISH 10')
        d.command('FINISH 11'); d.wait(4)
        d.command('PLAY 1 10 9 10 11 12 13'); d.command('UPDATE 1 10 11 12 13')
        self.assertIn('queue-stale-epoch', d.command('UPDATE 0 10 11 12 13', 'ERR'))
        self.assertTrue(d.status()['active']); self.assertEqual(d.status()['fault'], '')
        self.assertEqual(len(s.actions('AddURIToQueue')), 4)
        client = ProbeClient()
        for index, (id, rate) in enumerate([(10, 44100), (11, 44100), (12, 44100), (13, 48000)]):
            if index:
                client.advance(index*8-3.5)
                client.prefetch.append(index*8-client.now())
            client.fetch(s.items[index]['uri'], id, rate)
        self.assertTrue(all(3 <= lead <= 4 for lead in client.prefetch), client.prefetch)
        self.assertEqual(len(s.actions('SetAVTransportURI')), 1)
        self.assertEqual(len(s.actions('Play')), 1)
        self.assertEqual(s.max_busy, 1)
        self.assertTrue(s.playing)
        s.add_foreign(); s.add_foreign(first.replace('127.0.0.1', 'foreign.invalid'))
        foreign = [p['uri'] for p in s.items[-2:]]
        d.command('RELEASE stop')
        self.assertEqual([p['uri'] for p in s.items], foreign)
        self.assertTrue(s.crossfade)
        connection, response = request(first, 'HEAD')
        self.assertEqual(response.status, 404); connection.close()
        print('PASS queue controller: verified backup/five retained, current plus three ahead, publication ordering, one transport URI, 3.5 s prefetch, <=9x bursts, independent Range/reconnect readers, mixed native rates and exact owned-only stop release')

    def test_failed_backup_preserves_foreign_queue(self):
        for failure in ('fail_save', 'hide_save'):
            with self.subTest(failure=failure):
                setattr(self.speaker, failure, True)
                self.driver.put(1 if failure == 'fail_save' else 2)
                id = 1 if failure == 'fail_save' else 2
                self.driver.command(f'PLAY {id} {id} {id}', 'ERR')
                self.assertEqual(self.speaker.items, self.speaker.original)
                self.assertTrue(self.speaker.crossfade)
                self.assertFalse(self.speaker.actions('RemoveTrackRangeFromQueue'))
                self.assertFalse(self.speaker.actions('SetAVTransportURI'))
                self.assertFalse(self.speaker.actions('SetCrossfadeMode'))
                if failure == 'fail_save':
                    lines = Path(self.driver.log.name).read_text().splitlines()
                    fault = [line for line in lines if 'queue SOAP action=SaveQueue HTTP=500' in line]
                    self.assertEqual(len(fault), 1, lines)
                    self.assertIn('errorCode="701" errorDescription="fixture rejection"', fault[0])
                    self.assertIn('<InstanceID>0</InstanceID>', fault[0])
                    self.assertIn('<Title>Before YeneY ', fault[0])
                    self.assertIn('<ObjectID></ObjectID>', fault[0])
                setattr(self.speaker, failure, False)
        print('PASS queue SOAP fault diagnostics: action, HTTP status, UPnP code/description and complete request arguments on one line')
        print('PASS queue backup failure: failed SaveQueue and unverified saved identity leave the original queue and CrossfadeMode untouched')

    def test_sealed_current_and_shutdown_release(self):
        d, s = self.driver, self.speaker
        for id in range(1, 5): d.put(id, 'U' if id == 1 else 'P')
        d.command('PLAY 1 1 1 2 3 4'); self.assertEqual(d.status()['mirrored'], 0)
        self.assertFalse(s.actions('SetAVTransportURI'))
        d.command('FINISH 1'); d.wait(4)
        s.add_foreign(); retained=s.items[-1]['uri']
        d.close()
        self.assertEqual(s.items, [{'uri': retained}]); self.assertTrue(s.crossfade)
        print('PASS queue sealed publication/shutdown: no enqueue or Play before seal; service shutdown removes owned assets and restores CrossfadeMode')

    def test_mode_change_and_backup_name_collision(self):
        d, s = self.driver, self.speaker
        s.crossfade = False
        d.put(1); d.command('PLAY 1 1 1'); d.wait(1)
        s.add_foreign(); d.command('RELEASE mode-change')
        self.assertFalse(s.crossfade)
        d.put(2); d.command('PLAY 2 2 2'); d.wait(1)
        self.assertTrue(any(p['title'] == 'Before YeneY 2026-10-19 12:34 (2)' for p in s.saved))
        d.command('RELEASE mode-change'); self.assertFalse(s.crossfade)
        self.assertEqual([r[1]['CrossfadeMode'] for r in s.actions('SetCrossfadeMode')], ['0', '0', '0', '0'])
        print('PASS queue mode-change release: original CrossfadeMode off preserved, repeated sessions and same-minute backups have distinct verified identities')

    def test_same_minute_backup_retention_without_dates(self):
        d, s = self.driver, self.speaker
        s.saved = [s.saved[0]]+[dict(id='SQ:'+str(i), title='Before YeneY 2026-10-19 12:34'+
                    ('' if i == 9 else f' ({i-8})'), date='') for i in range(9, 16)]
        s.next_id = 16
        d.put(1); d.command('PLAY 1 1 1'); d.wait(1)
        self.assertEqual({p['id'] for p in s.saved}, {'SQ:1', 'SQ:12', 'SQ:13', 'SQ:14', 'SQ:15', 'SQ:16'})
        self.assertEqual(next(p['title'] for p in s.saved if p['id'] == 'SQ:16'), 'Before YeneY 2026-10-19 12:34 (8)')
        d.command('RELEASE stop')
        print('PASS queue backup retention: timestamp titles when dates are absent, numeric saved-queue IDs break same-minute ties, collision-safe names and unrelated playlists preserved')

    def test_paginated_snapshots(self):
        d, s = self.driver, self.speaker
        original = [dict(uri=f'http://foreign.invalid/track-{i}.flac') for i in range(129)]
        s.items = copy.deepcopy(original)
        s.saved += [dict(id='SQ:'+str(i), title='Other playlist '+str(i), date='') for i in range(100, 230)]
        unrelated = {p['id'] for p in s.saved if not p['title'].startswith('Before YeneY ')}
        d.put(1); s.race_page = True
        d.command('PLAY 1 1 1', 'ERR')
        self.assertEqual(s.items, original)
        self.assertFalse(s.actions('SaveQueue')); self.assertTrue(s.crossfade)
        d.command('PLAY 2 1 1'); d.wait(1)
        self.assertEqual(s.snapshots['SQ:9'], original)
        self.assertTrue(unrelated <= {p['id'] for p in s.saved})
        self.assertTrue(any(int(r[1]['StartingIndex']) > 0 for r in s.actions('Browse') if r[2] == 'Queue'))
        self.assertTrue(any(int(r[1]['StartingIndex']) > 0 for r in s.actions('Browse') if r[2] == 'ContentDirectory'))
        d.command('RELEASE stop')
        print('PASS queue snapshots: paginated queue/saved playlists, revision changes abort before backup or mutation, unrelated saved playlists retained')

    def test_pending_asset_update(self):
        d, s = self.driver, self.speaker
        d.put(1); d.put(2, 'N', produced=0); d.put(3); d.put(4, 'U')
        d.command('PLAY 1 1 1 2 3 4'); d.wait(1)
        d.put(2); time.sleep(.06)
        self.assertEqual(d.status()['mirrored'], 1)
        d.command('UPDATE 1 1 2 3 4'); d.wait(3)
        d.command('FINISH 4'); d.wait(4)
        self.assertEqual(len(s.actions('AddURIToQueue')), 4)
        d.command('RELEASE stop')
        print('PASS queue pending assets: decoder-created items supplied by asynchronous update, sealed successors watched without blocking, publication stays ordered and deduplicated')

    def test_unconfirmed_crossfade_is_not_owned(self):
        d, s = self.driver, self.speaker
        d.put(1); s.ignore_crossfade = True
        d.command('PLAY 1 1 1', 'ERR')
        self.assertEqual(d.status()['fault'], 'queue-crossfade-unconfirmed')
        self.assertEqual(s.items, s.original)
        self.assertTrue(s.crossfade)
        self.assertFalse(s.actions('AddURIToQueue'))
        print('PASS queue crossfade verification: a successful SOAP reply without the requested mode cannot clear or own the queue')

    def test_updateid_race_and_group_lease(self):
        d, s = self.driver, self.speaker
        d.put(1); s.race_add = True
        d.command('PLAY 1 1 1', 'ERR')
        self.assertEqual(s.items, [{'uri': 'http://foreign.invalid/added-during-play.flac'}])
        self.assertTrue(s.crossfade); self.assertFalse(s.actions('SetAVTransportURI'))
        self.assertIn('queue-enqueue-unconfirmed', d.status()['fault'])
        # A room that became grouped is refused before any queue mutation.
        s.grouped = True
        before = copy.deepcopy(s.items)
        d.command('PLAY 2 1 1', 'ERR')
        self.assertEqual(s.items, before)
        print('PASS queue coordinator fencing: stale UpdateID never retries an enqueue, preserves foreign edits and restores CrossfadeMode; grouped lease blocks mutations')


class PublicationRace(unittest.TestCase):
    def test_replaced_pending_handle_waits_for_host_snapshot(self):
        speaker=Coordinator();driver=Driver(speaker)
        try:
            driver.put(1)
            driver.command('PUT 2 S 0 100 44100')
            driver.command('PLAY 1 1 1 2');driver.wait(1)
            driver.command('PUBLISH 2 100')
            # Deliberately leave the superseded handle in the controller for
            # several worker ticks before delivering its immutable replacement.
            time.sleep(.2)
            status=driver.command('STATUS','STATUS ')
            self.assertTrue(status.startswith('STATUS 1 '),status)
            driver.command('UPDATE 1 1 2');driver.wait(2)
            self.assertEqual(len(speaker.items),2)
            print('PASS queue publication race: superseded unpublished handle waits for refreshed snapshot without fault or fallback',flush=True)
        finally:driver.close();speaker.close()

if __name__ == '__main__': unittest.main(verbosity=2)
