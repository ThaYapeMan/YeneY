"""Loopback speaker: test fixed wire fixtures, YeneY control and bridge polling."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os
import http.client
from urllib.parse import urlsplit
import subprocess
import tempfile
import threading
import time
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
SOAP = 'http://schemas.xmlsoap.org/soap/envelope/'
class Speaker(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *args): pass
    def send(self, data, status=200):
        data = data.encode()
        self.send_response(status)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(data)
    def do_SUBSCRIBE(self):
        self.send_response(200)
        self.send_header('SID', 'uuid:mock-subscription')
        self.send_header('TIMEOUT', 'Second-3600')
        self.send_header('Content-Length', '0')
        self.end_headers()
    def do_UNSUBSCRIBE(self):
        self.send('')
    def do_POST(self):
        try:
            self.handle_post()
        except (BrokenPipeError, ConnectionResetError):
            pass  # deadline tests intentionally close before the response
        except Exception as error:
            self.server.errors.append(error)
            print("mock speaker error:", repr(error), flush=True)
            self.close_connection = True
    def handle_post(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        if self.path == '/slow':
            time.sleep(.3)
            self.send('too late')
            return
        if self.path in ('/chunked', '/bad-chunk'):
            self.send_response(200)
            self.send_header('Transfer-Encoding', 'chunked')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(b'5;foo=bar\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: yes\r\n\r\n'
                            if self.path == '/chunked' else b'xyz\r\nx\r\n0\r\n\r\n')
            return
        if self.path == '/truncated':
            self.send_response(200)
            self.send_header('Content-Length', '100')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(b'short')
            return
        action = self.headers['SOAPAction'].strip('"').split('#')[1]
        self.server.counts[action] = self.server.counts.get(action, 0) + 1
        count = self.server.counts[action]
        self.server.requests.append((action, body))
        node = ET.fromstring(body).find(f'{{{SOAP}}}Body')[0]
        service = 'ZoneGroupTopology' if action == 'GetZoneGroupState' else 'RenderingControl' if action == 'GetVolume' else 'AVTransport'
        assert node.tag == f'{{urn:schemas-upnp-org:service:{service}:1}}{action}'
        assert self.path == ('/ZoneGroupTopology/Control' if service == 'ZoneGroupTopology'
                             else '/MediaRenderer/RenderingControl/Control' if service == 'RenderingControl' else '/MediaRenderer/AVTransport/Control')
        if service == 'AVTransport':
            assert node.findtext('InstanceID') == '0'
            if action in self.server.golden:
                assert body == self.server.golden[action], action
        if action == 'SetAVTransportURI' and self.server.mode in ('control', 'golden'):
            assert body == (ROOT / 'tests/fixtures/radio-set-uri.xml').read_bytes()
        if action == 'Play':
            assert node.findtext('Speed') == '1'
            if self.server.mode in ('control', 'delayed-play') and count == 1: time.sleep(6)
            if self.server.mode == 'timeout-playing': time.sleep(21)
        if action == 'SetAVTransportURI': self.server.current_uri = node.findtext('CurrentURI')
        if action == 'Stop' and self.server.mode in ('get-first', 'event-first'): self.server.stopped = True
        values = ''
        fault = self.server.mode == 'control' and (action == 'Pause' or action == 'Play' and count == 2)
        if fault:
            if action == 'Pause': time.sleep(.2)
            self.send(f'<s:Envelope xmlns:s="{SOAP}"><s:Body><s:Fault><faultcode>s:Client</faultcode>'
                      '<detail><UPnPError><errorCode>701</errorCode><errorDescription>Transition not available</errorDescription>'
                      '</UPnPError></detail></s:Fault></s:Body></s:Envelope>', 500)
            return
        if action == 'GetZoneGroupState':
            topology = (ROOT / 'tests/fixtures/topology.xml').read_text()
            if self.server.mode in ('control', 'lifecycle') and count >= 3:
                topology = topology.replace('Coordinator="RINCON_00112233445501400"',
                                            'Coordinator="RINCON_66778899AABB01400"')
            values = '<ZoneGroupState>' + escape(topology) + '</ZoneGroupState>'
        if action == 'GetTransportInfo':
            state = 'PLAYING'
            if self.server.mode == 'stopped-media-info':
                state = {2: 'STOPPED', 3: 'PAUSED_PLAYBACK'}.get(count, 'PLAYING')
            if self.server.mode == 'poll':
                state = {3: 'PAUSED_PLAYBACK', 4: 'TRANSITIONING'}.get(count, 'PLAYING')
            if self.server.mode in ('get-first', 'event-first') and getattr(self.server, 'stopped', False): state = 'STOPPED'
            if self.server.mode == 'stale' and count >= 2:
                self.notify('PLAYING', sid='uuid:wrong', expected=412)
                self.notify('TRANSITIONING')
                time.sleep(.1)
                state = 'STOPPED'
            values = f'<CurrentTransportState>{state}</CurrentTransportState><CurrentTransportStatus>OK</CurrentTransportStatus>'
        if action == 'GetPositionInfo': values = '<RelTime>0:02:03</RelTime><TrackDuration>0:04:56</TrackDuration><TrackMetaData>' + escape('<DIDL-Lite><item><dc:title xmlns:dc="urn:dc">Title from speaker</dc:title></item></DIDL-Lite>') + '</TrackMetaData>'
        if action == 'GetVolume':
            assert node.findtext('Channel') == 'Master'
            values = '<CurrentVolume>37</CurrentVolume>'
        if action == 'GetMediaInfo':
            uri = self.server.current_uri if self.server.mode == 'timeout-playing' else 'http://external/?a=1&b=2'
            values = '<CurrentURI>' + escape(uri) + '</CurrentURI><TrackURI>wrong</TrackURI>'
        self.send(f'<s:Envelope xmlns:s="{SOAP}"><s:Body><u:{action}Response '
                  f'xmlns:u="urn:schemas-upnp-org:service:{service}:1">{values}</u:{action}Response></s:Body></s:Envelope>')


def run(mode, command, golden=None):
    server = ThreadingHTTPServer(('0.0.0.0', 0), Speaker)
    server.current_uri = ''  # GetMediaInfo can precede the first SetAVTransportURI.
    server.mode, server.counts, server.requests, server.errors, server.golden = mode, {}, [], [], golden or {}
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        result = subprocess.run([*command, str(server.server_port)], cwd=ROOT, check=True, timeout=40, capture_output=True, text=True, env={**os.environ, "SONOS_LMS_YENEY_POLL": "legacy"})
        print(result.stdout, end='')
        server.output = result.stdout
        assert not server.errors, server.errors
        return server
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

golden = {'SetAVTransportURI': (ROOT / 'tests/fixtures/radio-set-uri.xml').read_bytes()}
for action in ('Play', 'Pause', 'Stop', 'GetTransportInfo', 'GetPositionInfo', 'GetMediaInfo'):
    args = '<InstanceID>0</InstanceID>' + ('<Speed>1</Speed>' if action in ('Play', 'Pause', 'Stop') else '')
    golden[action] = ('<?xml version="1.0" encoding="utf-8"?>'
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
        f'<s:Body><u:{action} xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
        f'{args}</u:{action}></s:Body></s:Envelope>').encode()
control = run('control', [str(ROOT / 'own-control-test')], golden)
assert control.counts['GetPositionInfo'] == 3
assert control.counts['GetVolume'] == 2
assert control.counts['GetTransportInfo'] == 3
assert control.counts['GetZoneGroupState'] == 3
for action, body in control.requests:
    if action in golden: assert body == golden[action]
print('PASS: all seven own AVTransport request bodies match fixed SOAP wire fixtures')

source = (ROOT / 'sonos-lms.cpp').read_text()
def production_function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

with tempfile.TemporaryDirectory(prefix='sonos-play-timeout-') as temp:
    temp = Path(temp)
    (temp / 'production_play_timeout.inc').write_text('\n'.join(production_function(s) for s in (
        'static bool alreadyPlayingCurrentStream(unsigned stream)\n',
        'static bool PlaySqueezeBoxLocked(unsigned stream_id, bool resetPosition)\n',
        'static void dispatchStreamStart(')))
    executable = temp / 'play-timeout-test'
    subprocess.run(['g++', '-O2', '-Wall', '-Wextra', '-I', str(ROOT), '-I', str(temp),
                    str(ROOT / 'tests/play_timeout_fixture.cpp'),
                    *[str(ROOT / ('upnp/' + name + '.cpp')) for name in ('http_server', 'gena', 'own_speaker_control', 'xml', 'soap', 'http', 'discovery')],
                    '-lpthread', '-lcrypto', '-o', str(executable)], check=True)
    for mode in ('delayed-play', 'timeout-playing'):
        tested = run(mode, [str(executable)])
        assert tested.counts['Play'] == tested.counts['SetAVTransportURI'] == 1
        assert tested.output.count('PlaySqueezeBox: title=') == 1
        if mode == 'timeout-playing':
            assert 'yeney: SOAP action=Play error=timeout HTTP=0' in tested.output
            assert 'PlayStream(stream 7): device already playing current stream; no retry' in tested.output
        else:
            assert 'failed' not in tested.output
        print(f'PASS: {mode}: one PlaySqueezeBox, one SetAVTransportURI, one Play; no track restart')

with tempfile.TemporaryDirectory(prefix='sonos-own-poll-') as temp:
    temp = Path(temp)
    (temp / 'production_own_poll.inc').write_text('\n'.join(production_function(s) for s in (
        'static void ObserveDeviceTransport(', 'void ResumeSqueezeBox(', 'void refreshStatus(')))
    executable = temp / 'poll-test'
    subprocess.run(['g++', '-O2', '-Wall', '-Wextra', '-I', str(ROOT), '-I', str(temp),
                    str(ROOT / 'tests/own_poll_fixture.cpp'), str(ROOT / 'sonos-status.cpp'),
                    *[str(ROOT / ('upnp/' + name + '.cpp')) for name in ('http_server', 'gena', 'own_speaker_control', 'xml', 'soap', 'http', 'discovery')],
                    '-lpthread', '-lcrypto', '-o', str(executable)], check=True)
    run('poll', [str(executable)])
    settings = temp / 'settings.cpp'
    settings.write_text('''#include "upnp/backend.h"
#include <cassert>
int main(int argc, char** argv) {
    const auto expected = upnp::Backend::Own;
    assert(upnp::backend() == expected);
    setenv("SONOS_LMS_UPNP", "changed", 1);
    assert(upnp::backend() == expected);
}''')
    subprocess.run(['g++', '-I', str(ROOT), str(settings), '-o', str(temp / 'settings')], check=True)
    for value in (None, 'noson', 'yeney', 'own', '', 'invalid'):
        env = dict(os.environ)
        env.pop('SONOS_LMS_UPNP', None)
        if value is not None: env['SONOS_LMS_UPNP'] = value
        result = subprocess.run([str(temp / 'settings')], env=env,
                                check=True, capture_output=True, text=True)
        expected_log = 'UPnP layer: yeney (alias own)' if value == 'own' else 'UPnP layer: yeney'
        expected = (f"SONOS_LMS_UPNP={value} is no longer supported; using YeneY\n" if value in ('noson', '', 'invalid') else '') + expected_log + '\n'
        assert result.stdout == expected, result.stdout
        print(f'PASS: SONOS_LMS_UPNP={value!r}: {expected_log}; exact warning and read-once selection')
        assert result.stdout.count('UPnP layer:') == 1
        assert result.stdout.count('is no longer supported') == int(value in ('noson', '', 'invalid'))
    print('PASS: SONOS_LMS_UPNP defaults, validation and read-once startup logging')


class EventSpeaker(Speaker):
    def do_POST(self):
        if self.path == '/test-notify':
            self.rfile.read(int(self.headers['Content-Length']))
            self.notify('TRANSITIONING')
            self.send('')
        else:
            super().do_POST()
    def setup(self):
        super().setup()
        self.server = getattr(self.server, 'root', self.server)
    def do_SUBSCRIBE(self):
        path = self.path
        assert path in ('/MediaRenderer/AVTransport/Event', '/MediaRenderer/RenderingControl/Event', '/ZoneGroupTopology/Event')
        now = time.monotonic()
        sid = self.headers.get('SID')
        self.server.subscriptions.append((now, dict(self.headers), self.headers['Host'], path))
        assert self.headers['TIMEOUT'] == 'Second-300'
        if self.server.mode == 'fallback' or (self.server.mode == 'partial' and 'RenderingControl' in path):
            self.send('', 503)
            return
        slot = self.server.services.setdefault(path, {'fresh': 0, 'renewals': 0, 'sid': ''})
        suffix = '/avt' if 'AVTransport' in path else '/rc' if 'RenderingControl' in path else '/zgt'
        if sid:
            assert 'CALLBACK' not in self.headers and 'NT' not in self.headers
            assert sid == slot['sid']
            slot['renewals'] += 1
            if slot['renewals'] == 1 and self.server.mode == 'lifecycle' and suffix == '/avt':
                self.send('', 412)
                return
        else:
            assert self.headers['NT'] == 'upnp:event'
            callback = self.headers['CALLBACK']
            assert callback.startswith(('<http://127.0.0.1:', '<http://127.0.0.2:')) and callback.endswith(suffix + '>')
            slot['callback'] = callback[1:-1]
            slot['fresh'] += 1
            slot['sid'] = f'uuid:{suffix[1:]}-{slot["fresh"]}'
            if suffix == '/avt':
                self.server.callback = slot['callback']
                self.server.sid = slot['sid']
        if not sid and self.server.mode in ('events', 'partial'):
            def initial():
                try:
                    fixture = 'sonos-lastchange.xml' if suffix == '/avt' else 'rendering-lastchange.xml' if suffix == '/rc' else 'zone-group-notify.xml'
                    body = (ROOT / 'tests/fixtures' / fixture).read_text().replace('PLAYING', 'STOPPED')
                    if suffix == '/zgt':
                        # Preserve the captured payload; route its addresses to
                        # local fake speakers so tests never contact real devices.
                        body = body.replace('192.168.178.132', '127.0.0.1').replace('192.168.178.145', '127.0.0.2').replace('192.168.178.140', '127.0.0.3')
                        body = body.replace('RINCON_949F3EFABA6601400', 'RINCON_00112233445501400')
                    endpoint = urlsplit(slot['callback'])
                    connection = http.client.HTTPConnection(endpoint.hostname, endpoint.port, timeout=2)
                    connection.request('NOTIFY', endpoint.path, body=body.encode(), headers={
                        'SID': slot['sid'], 'SEQ': '0', 'NT': 'upnp:event', 'NTS': 'upnp:propchange'})
                    response = connection.getresponse()
                    assert response.status == 200, response.status
                    response.read(); connection.close()
                except Exception as error: self.server.errors.append(error)
            # Send initial NOTIFY before the SUBSCRIBE response publishes its SID.
            threading.Thread(target=initial).start()
            time.sleep(.05)
        self.send_response(200)
        self.send_header('SID', slot['sid'])
        self.send_header('TIMEOUT', 'Second-2' if self.server.mode in ('lifecycle', 'ip-change') else 'Second-300')
        self.send_header('Content-Length', '0')
        self.end_headers()
    def do_UNSUBSCRIBE(self):
        self.server.unsubscriptions.append((self.headers['SID'], self.headers['Host'], self.path))
        self.send('')
    def notify(self, state, sid=None, expected=200):
        endpoint = urlsplit(self.server.callback)
        connection = http.client.HTTPConnection(endpoint.hostname, endpoint.port, timeout=2)
        body = (ROOT / 'tests/fixtures/sonos-lastchange.xml').read_text().replace('PLAYING', state)
        connection.request('NOTIFY', endpoint.path, body=body.encode(), headers={
            'SID': sid or self.server.sid, 'SEQ': '12', 'NT': 'upnp:event', 'NTS': 'upnp:propchange'})
        response = connection.getresponse()
        assert response.status == expected, response.status
        response.read(); connection.close()


def event_run(mode, command):
    server = ThreadingHTTPServer(('127.0.0.1', 0), EventSpeaker)
    server.mode, server.counts, server.requests, server.errors, server.golden = mode, {}, [], [], {}
    server.current_uri = ''
    server.sid, server.callback = '', ''
    server.services = {}
    server.subscriptions, server.unsubscriptions = [], []
    peer = ThreadingHTTPServer(('127.0.0.2', server.server_port), EventSpeaker)
    peer.root = server
    threads = [threading.Thread(target=s.serve_forever) for s in (server, peer)]
    for thread in threads: thread.start()
    try:
        result = subprocess.run([*command, mode, str(server.server_port)], cwd=ROOT,
                                check=True, capture_output=True, text=True, timeout=20, env={**os.environ, "SONOS_LMS_YENEY_POLL": "legacy"})
        print(result.stdout, end='')
        assert not server.errors, server.errors
        avt = '/MediaRenderer/AVTransport/Event'
        if mode == 'fallback':
            assert not server.services and not server.unsubscriptions
            assert 'reason=HTTP-503' in result.stdout
            for path in set(s[3] for s in server.subscriptions):
                times = [s[0] for s in server.subscriptions if s[3] == path]
                assert len(times) >= 3, times
                assert .8 <= times[1] - times[0] < 1.8
                assert 4.8 <= times[2] - times[1] < 5.8
        else:
            assert len(server.services) == (2 if mode == 'partial' else 3), server.services
            slot = server.services[avt]
            if mode == 'ip-change':
                assert all(s['fresh'] == 2 for s in server.services.values()), server.services
                assert len(server.unsubscriptions) == 6
                callbacks = [s[1].get('CALLBACK', '') for s in server.subscriptions]
                assert any(c.startswith('<http://127.0.0.2:') for c in callbacks)
            elif mode in ('events', 'partial'):
                assert server.counts['GetTransportInfo'] == 1, server.counts
                assert server.counts['GetMediaInfo'] == 1, server.counts
                assert server.counts['GetPositionInfo'] == 1, server.counts
                assert server.counts['GetZoneGroupState'] == 2, server.counts
                assert server.counts['GetVolume'] == 1 if mode == 'events' else server.counts['GetVolume'] >= 3
                assert result.stdout.count('yeney: first NOTIFY') == len(server.services)
                assert 'yeney: monitor events' in result.stdout if mode == 'events' else 'yeney: monitor polling RenderingControl reason=HTTP-503' in result.stdout
                assert len(server.unsubscriptions) == len(server.services)
            elif mode == 'lifecycle':
                assert slot['fresh'] == 3, slot
                assert slot['renewals'] >= 2
                avt_requests = [s for s in server.subscriptions if s[3] == avt]
                delta = avt_requests[1][0] - avt_requests[0][0]
                assert .8 <= delta < 1.7, delta
                retry = avt_requests[2][0] - avt_requests[1][0]
                assert .8 <= retry < 1.7, retry
                assert all(s['fresh'] == 1 for p,s in server.services.items() if p != avt)
                assert len(server.unsubscriptions) == 4, server.unsubscriptions
                assert any(s[1].startswith('127.0.0.2:') and s[2] == avt for s in server.unsubscriptions)
                assert all(s[1].startswith('127.0.0.1:') for s in server.unsubscriptions if s[2] != avt)
            else:
                assert all(s['fresh'] == 1 for s in server.services.values())
                assert len(server.unsubscriptions) == 3
        checks = 'polling fallback after subscription failure' if mode == 'fallback' else (
            'SUBSCRIBE headers/SID, half-time renewal, 412 recovery, coordinator handoff and UNSUBSCRIBE' if mode == 'lifecycle'
            else 'SID validation, callback delivery and shutdown UNSUBSCRIBE')
        print(f'PASS: GENA {mode}: {checks}')
        return result.stdout
    finally:
        for s in (server, peer): s.shutdown(); s.server_close()
        for thread in threads: thread.join()

with tempfile.TemporaryDirectory(prefix='sonos-gena-') as temp:
    executable = Path(temp) / 'gena-test'
    subprocess.run(['g++', '-O2', '-Wall', '-Wextra', '-I', str(ROOT),
                    str(ROOT / 'tests/gena_fixture.cpp'),
                    *[str(ROOT / ('upnp/' + name + '.cpp')) for name in
                      ('http_server', 'gena', 'own_speaker_control', 'xml', 'soap', 'http', 'discovery')],
                    '-lpthread', '-o', str(executable)], check=True)
    subprocess.run([str(executable)], cwd=ROOT, check=True)
    for mode in ('lifecycle', 'fallback', 'stale', 'events', 'partial', 'ip-change'):
        event_run(mode, [str(executable)])

    # Reuse the existing device-resume fixture and all production transport
    # functions. Only its device boundary delegates to real yeney control.
    fixture = (ROOT / 'tests/device_resume_fixture.cpp').read_text()
    fixture = fixture.replace('struct Transport {', 'static upnp::SpeakerControl* eventControl = nullptr;\nstruct Transport {')
    fixture = fixture.replace('Transport transportInfo() { return property; }',
        'Transport transportInfo() { if (eventControl) { auto t = eventControl->transportInfo(); return {t.state, t.status}; } return property; }')
    fixture = fixture.replace('++stopCalls;\n        return true;', '++stopCalls;\n        return eventControl ? eventControl->stop() : true;')
    fixture = fixture.replace('return player.property.state;', 'return player.transportInfo().state;')
    fixture = fixture.replace('static unsigned decisionLogs = 0;', 'static unsigned decisionLogs = 0, resumeLogs = 0;')
    fixture = fixture.replace('    fputs(message, stdout);',
        '    fputs(message, stdout);\n    if (std::string(message).find("Device-initiated resume: current stream") == 0) ++resumeLogs;')
    fixture = fixture.replace('int main() {', 'int legacyMain() {')
    fixture = fixture.rstrip()[:-1] + '    return 0;\n}\n'
    signatures = (
        'std::string SqueezeBoxURL(unsigned stream_id)',
        'static bool alreadyPlayingCurrentStream(unsigned stream)\n',
        'static bool PlaySqueezeBoxLocked(unsigned stream_id, bool resetPosition)\n',
        'extern "C" void new_squeezebox_stream_id(', 'static void dispatchDeferredStop(',
        'static void dispatchStreamStart(', 'static void dispatchTransportIntent(',
        'extern "C" void sonos_lms_transport(', 'static void ObserveDeviceTransport(',
        'void ResumeSqueezeBox(', 'void refreshStatus(')
    Path(temp, 'production_resume.inc').write_text('\n'.join(production_function(s) for s in signatures))
    # Match the definition rather than the forward declaration.
    callback_start = source.index('void onSonosEvent(void* handle)\n{')
    callback = source[callback_start:source.index('\n}', callback_start) + 2]
    fixture += '\nstatic std::atomic<bool> gEvent{false};\n' + callback
    fixture += '\n' + (ROOT / 'tests/gena_resume_main.inc').read_text()
    path = Path(temp, 'gena-resume.cpp')
    path.write_text('#include "upnp/own_speaker_control.h"\n#include "upnp/http.h"\n#include <iostream>\n' + fixture)
    executable = Path(temp, 'gena-resume')
    subprocess.run(['g++', '-O2', '-Wall', '-Wextra', '-I', str(ROOT), '-I', str(ROOT/'tests'), '-I', temp,
                    str(path), *[str(ROOT / ('upnp/' + name + '.cpp')) for name in
                      ('http_server', 'gena', 'own_speaker_control', 'xml', 'soap', 'http', 'discovery')],
                    '-lpthread', '-lcrypto', '-o', str(executable)], check=True)
    for mode in ('event-first', 'get-first'):
        output = event_run(mode, [str(executable)])
        assert output.count('Device-initiated resume: current stream') == 1

# Both modes keep transport polling running; only idle GetMediaInfo differs.
previous = os.environ.get('SONOS_LMS_YENEY_STOPPED_MEDIAINFO')
try:
    for mode, held, expected in [(None, False, 3), ('0', False, 3), ('0', True, 3), ('1', False, 5), ('1', True, 3)]:
        if mode is None: os.environ.pop('SONOS_LMS_YENEY_STOPPED_MEDIAINFO', None)
        else: os.environ['SONOS_LMS_YENEY_STOPPED_MEDIAINFO'] = mode
        if held: os.environ['TEST_HELD_REQUEST'] = '1'
        else: os.environ.pop('TEST_HELD_REQUEST', None)
        speaker = run('stopped-media-info', [str(ROOT / 'own-control-test'), '--stopped-media-info'])
        assert speaker.counts['GetMediaInfo'] == expected, speaker.counts
        assert speaker.counts['GetTransportInfo'] == 4, speaker.counts
        assert f'stopped_mediainfo={mode or 0}' in speaker.output
        print(f'PASS: STOPPED_MEDIAINFO={mode or "default"}, held={held}: STOPPED/PAUSED polling, active polling and explicit reads verified')
finally:
    os.environ.pop('TEST_HELD_REQUEST', None)
    if previous is None: os.environ.pop('SONOS_LMS_YENEY_STOPPED_MEDIAINFO', None)
    else: os.environ['SONOS_LMS_YENEY_STOPPED_MEDIAINFO'] = previous
