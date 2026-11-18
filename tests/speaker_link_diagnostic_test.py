# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import subprocess

from queue_sonos_fake import Coordinator
from sonos_websocket_fake import LocalApi


owner = Coordinator()
owner.realtime = True
owner.transport_state = 'PLAYING'
owner.position = dict(Track='1', TrackURI='owned-one', RelTime='00:00:10')
api = LocalApi(owner, port=0)
try:
    endpoint = f'127.0.0.1:{api.server.server_port}'
    api.websocket_url = f'wss://{endpoint}/websocket/api'
    result = subprocess.run(['./yeney', '--position-source-check', '--link-rates',
                             '--ip', endpoint, '--seconds', '8'],
                            capture_output=True, text=True, timeout=20)
    print(result.stdout, end='')
    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    assert len(lines) == 8
    for line, (hz, interleave) in zip(lines, ((hz, interleave) for hz in (5, 10, 20, 40)
                                             for interleave in ('no', 'yes'))):
        assert line.startswith(f'link check rate_hz={hz} interleaved_pings={interleave} ')
        fields = dict(part.split('=') for part in line.split()[2:])
        for kind in ('position', 'ping'):
            assert int(fields[f'{kind}_answers']) > 0
            assert 0 <= float(fields[f'{kind}_median_ms']) <= float(fields[f'{kind}_p95_ms']) <= float(fields[f'{kind}_max_ms'])
    assert {header['command'] for _, header in api.commands} == {'subscribe', 'getPlaybackStatus'}
finally:
    api.close()
    owner.close()
