# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import importlib.util
from pathlib import Path
import tempfile
import subprocess
import sys
import threading
import socketserver
from queue_sonos_fake import Coordinator
from sonos_websocket_fake import LocalApi

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('probe', root/'scripts/sonos-timing-probe.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
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
finally:
    server.shutdown()
    server.server_close()
    worker.join()
    api.close()
    owner.close()
