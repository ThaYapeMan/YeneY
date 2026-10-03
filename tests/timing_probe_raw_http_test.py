"""Raw records at the real isolated HTTP boundary; no Sonos or deployment."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import html
import os
import shlex
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


class Speaker(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_SUBSCRIBE(self):
        self.send_response(200)
        self.send_header('SID', 'uuid:raw')
        self.send_header('TIMEOUT', 'Second-3600')
        self.send_header('Content-Length', '0')
        self.end_headers()

    do_UNSUBSCRIBE = do_SUBSCRIBE

    def do_POST(self):
        self.rfile.read(int(self.headers['Content-Length']))
        action = self.headers['SOAPACTION'].split('#')[-1].strip('"')
        with self.server.lock:
            count = self.server.count.get(action, 0) + 1
            self.server.count[action] = count
        fields = {}
        if action == 'GetZoneGroupState':
            fields = {'ZoneGroupState': '<ZoneGroups><ZoneGroup Coordinator="study">'
                      f'<ZoneGroupMember UUID="study" ZoneName="Study" Location="http://127.0.0.1:{self.server.server_port}/"/>'
                      '</ZoneGroup></ZoneGroups>'}
        elif action == 'GetTransportInfo':
            fields = {'CurrentTransportState': 'PLAYING' if count == 1 else 'PAUSED_PLAYBACK',
                      'CurrentTransportStatus': 'OK'}
        elif action == 'GetPositionInfo':
            second = 123 if count == 1 else int(time.monotonic() - self.server.started)
            text = f'0:{second // 60:02}:{second % 60:02}'
            fields = {'RelTime': text, 'TrackDuration': '0:00:00'}
            self.server.reltimes.append(text)
        elif action == 'GetVolume':
            fields = {'CurrentVolume': '25'}
        service = ('ZoneGroupTopology' if action == 'GetZoneGroupState' else
                   'RenderingControl' if action == 'GetVolume' else 'AVTransport')
        body = (f'<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body>'
                f'<u:{action}Response xmlns:u="urn:schemas-upnp-org:service:{service}:1">' +
                ''.join(f'<{k}>{html.escape(v)}</{k}>' for k, v in fields.items()) +
                f'</u:{action}Response></s:Body></s:Envelope>').encode()
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


server = ThreadingHTTPServer(('127.0.0.1', 0), Speaker)
server.count = {}
server.reltimes = []
server.lock = threading.Lock()
server.started = time.monotonic()
thread = threading.Thread(target=server.serve_forever)
thread.start()
try:
    env = os.environ.copy()
    env.update(YENEY_TIMING_PROBE='1', YENEY_TIMING_RAW='1', YENEY_TIMING_STALE_S='60')
    result = subprocess.run([str(ROOT / 'timing-probe-http-test'), str(server.server_port), 'active'],
                            env=env, text=True, capture_output=True, check=True, timeout=18)
    records = [dict(part.split('=', 1) for part in shlex.split(line)[2:])
               for line in result.stdout.splitlines() if line.startswith('yeney: timing-raw ')]
    edges = [line for line in result.stdout.splitlines() if line.startswith('yeney: timing-edge ')]
    assert len(records) == len(server.reltimes) - 1
    assert [r['reltime'] for r in records] == server.reltimes[1:]
    assert [int(r['seq']) for r in records] == list(range(1, len(records) + 1))
    assert records[0]['kind'] == 'seed'
    assert edges
    for r in records:
        assert float(r['recv_mono']) >= float(r['send_mono']) > 0
        assert abs((float(r['recv_mono']) - float(r['send_mono'])) * 1000 - float(r['rtt_ms'])) < .0011
        assert r['outcome'] in ('accepted', 'rejected-rtt', 'rejected-stale-epoch')
    print(f'PASS: real HTTP RAW: {len(records)} requests each logged once with exact RelTime, sequence, timestamps; {len(edges)} edges; leased cache unchanged')
finally:
    server.shutdown()
    server.server_close()
    thread.join()
