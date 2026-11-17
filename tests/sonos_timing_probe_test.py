# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import importlib.util
from pathlib import Path
import tempfile
import subprocess
import sys
import threading
import socketserver
import time
import re
from queue_sonos_fake import Coordinator
from sonos_websocket_fake import LocalApi

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('probe', root/'scripts/sonos-timing-probe.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
for state in ('PLAYING', 'PLAYBACK_STATE_PLAYING'):
    assert probe.project(dict(position_ms=1000, midpoint=1, state=state), 1.2) == 1200
for state in ('PLAYBACK_STATE_BUFFERING', 'TRANSITIONING', 'PLAYBACK_STATE_PAUSED', 'PLAYBACK_STATE_IDLE', 'UNKNOWN'):
    assert probe.project(dict(position_ms=1000, midpoint=1, state=state), 1.2) == 1000
assert 'median_ms=991.000' in probe.summary([(i*.25, 991) for i in range(480)])
assert 'creep_ms_per_s=0.000000' in probe.summary([(i*.25, 991) for i in range(480)])
owner = Coordinator()
owner.realtime = True
owner.transport_state = 'PLAYING'
owner.position = dict(Track='1', TrackURI='owned-one', RelTime='00:00:10')
api = LocalApi(owner, port=0)
class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        for line in self.rfile:
            assert line == b'00:11:22:33:44:55 time ?\n'
            value = (owner.position.milliseconds() - 991) / 1000
            self.wfile.write(f'00:11:22:33:44:55 time {value:.6f}\n'.encode())
server = socketserver.ThreadingTCPServer(('127.0.0.1', 0), Handler)
worker = threading.Thread(target=server.serve_forever)
worker.start()
try:
    with tempfile.TemporaryDirectory(prefix='sonos-probe-') as directory:
        output = Path(directory)/'raw.tsv'
        result = subprocess.run([sys.executable, str(root/'scripts/sonos-timing-probe.py'), '--source', 'both',
            '--speaker', '127.0.0.1', '--port', str(api.server.server_port), '--upnp-port', str(owner.server_port),
            '--lms-host', '127.0.0.1', '--lms-port', str(server.server_address[1]), '--player-mac', '00:11:22:33:44:55',
            '--duration', '3', '--output', str(output)], capture_output=True, text=True, check=True)
        print(result.stdout, end='')
        rows = output.read_text().splitlines()
        assert sum(r.startswith('websocket\t') for r in rows) == 12
        assert sum(r.startswith('lms\t') for r in rows) == 12
        assert sum(r.startswith('upnp\t') for r in rows) >= 28
        assert 'classification=truncated' in result.stdout
        assert all(field in result.stdout for field in ('below_500_pct=', '900_1100_pct=', 'above_1100_pct=',
            'within_30_pct=', 'jumps_over_150_ms=', 'creep_ms_per_s=', 'second-change boundary:', 'second-change seek:'))
        print('PASS R5 read-only both-source probe, 4 Hz paired midpoint clocks, 10 Hz RelTime TSV and all summary fields')
        def transition(track, position):
            api.position_variant = 'missing-position'
            owner.position_variant = 'not-implemented'
            with owner.lock:
                owner.position = dict(Track=str(track), TrackURI=f'owned-{track}', RelTime=position)
            time.sleep(.45)
            api.position_variant = ''
            owner.position_variant = ''

        def change_playback():
            time.sleep(.9)
            transition(2, '00:00:02')
            time.sleep(2.2)
            transition(2, '00:00:25')

        changes = threading.Thread(target=change_playback)
        changes.start()
        try:
            transitional = subprocess.run([sys.executable, str(root/'scripts/sonos-timing-probe.py'), '--source', 'both',
                '--speaker', '127.0.0.1', '--port', str(api.server.server_port), '--upnp-port', str(owner.server_port),
                '--lms-host', '127.0.0.1', '--lms-port', str(server.server_address[1]), '--player-mac', '00:11:22:33:44:55',
                '--duration', '7', '--output', str(output)], capture_output=True, text=True, check=True)
        finally:
            changes.join()
        print(transitional.stdout, end='')
        assert 'errors=0' in transitional.stdout
        for source in ('websocket', 'upnp'):
            assert re.search(source + r' no_position_samples=[1-9]\d* gaps=2', transitional.stdout), transitional.stdout
            assert any(line.startswith(source+'\t') and '\tno position\t' in line for line in output.read_text().splitlines())
        for phase in ('boundary', 'seek'):
            assert re.search(r'second-change ' + phase + r': n=[1-9]', transitional.stdout), transitional.stdout
        api.disabled = True
        early = subprocess.run([sys.executable, str(root/'scripts/sonos-timing-probe.py'),
            '--speaker', '127.0.0.1', '--port', str(api.server.server_port), '--output', str(output)],
            capture_output=True, text=True)
        assert early.returncode == 1 and 'errors=1' in early.stdout, early
        assert output.read_text().splitlines()[0].startswith('kind\tsent_mono')
        print('PASS probe records both invalid-coordinate answers, measures skip and scrub, and writes early-exit output')

finally:
    server.shutdown()
    server.server_close()
    worker.join()
    api.close()
    owner.close()
