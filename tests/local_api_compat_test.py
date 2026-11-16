# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Device wire variations through the production source and binary diagnostic."""
import os
import subprocess
from queue_sonos_fake import Coordinator
from sonos_websocket_fake import LocalApi


def main():
    owner = Coordinator()
    owner.realtime = True
    owner.transport_state = 'PLAYING'
    owner.position = dict(Track='1', TrackURI='owned', RelTime='00:00:10')
    api = LocalApi(owner, port=0)
    port = api.server.server_port
    api.websocket_url = f'wss://127.0.0.1:{port}/websocket/api'
    binary = os.environ.get('YENEY_CHECK_BINARY', './yeney')
    fixture = os.environ.get('YENEY_COMPAT_FIXTURE')
    variants = ['chunked', 'length', 'close', 'extensions', 'id-chunks', 'mixed-case', 'oversize', 'truncated',
                'upgrade-case', 'fragmented', 'split-tcp', 'extra-fields', 'field-order',
                'numeric-string', 'optional-missing', 'interleaved', 'slow-first']
    selected = os.environ.get('YENEY_COMPAT_VARIANT')
    try:
        for variant in variants:
            if selected and variant != selected: continue
            api.variant = variant
            if os.environ.get('YENEY_COMPAT_BEFORE'): api.info_variant = variant if variants.index(variant) < 8 else 'length'
            api.polls = 0
            api.requests.clear()
            api.commands.clear()
            expected = {'oversize': 'local info too large', 'truncated': 'local info truncated chunk'}.get(variant)
            if fixture:
                process = subprocess.Popen([fixture], env=dict(os.environ, YENEY_WS_TEST_PORT=str(port)),
                                           stdout=subprocess.PIPE, text=True)
                try:
                    for line in process.stdout:
                        fields = line.rstrip('\n').split('\t')
                        if (expected and expected in line) or (not expected and float(fields[1]) >= 0):
                            if os.environ.get('YENEY_COMPAT_BEFORE'):
                                print(f'BEFORE {variant}: source=' + ('upnp reason=' + expected if expected else 'websocket'), flush=True)
                            break
                        if fields[3] and fields[3] != 'connecting':
                            if os.environ.get('YENEY_COMPAT_BEFORE'):
                                print(f'BEFORE {variant}: source=upnp reason={fields[3]}', flush=True)
                                break
                            raise AssertionError(line.strip())
                    else: raise AssertionError('no result')
                finally:
                    process.terminate(); process.wait(5); process.stdout.close()
                if os.environ.get('YENEY_COMPAT_BEFORE'): continue
                result = 'upnp reason=' + expected if expected else 'websocket'
            else:
                run = subprocess.run([binary, '--position-source-check', '--ip', f'127.0.0.1:{port}', '--seconds', '1'],
                                     capture_output=True, text=True, timeout=12)
                if expected:
                    assert run.returncode != 0 and 'local/info FAIL reason=' + expected in run.stdout, run.stdout
                    assert not api.commands and api.requests == ['/api/v1/players/local/info'], api.requests
                else:
                    assert run.returncode == 0 and 'selected source=websocket' in run.stdout, run.stdout + run.stderr
                    for step in ['local/info OK', 'WebSocket upgrade OK', 'playback subscription OK',
                                 'metadata subscription OK', 'rate=5Hz', 'rate=20Hz']:
                        assert step in run.stdout, run.stdout
                result = 'upnp reason=' + expected if expected else 'websocket'
                assert all(h['command'] in ('subscribe', 'getPlaybackStatus') for _, h in api.commands), api.commands
                assert all(path in ('/api/v1/players/local/info', '/websocket/api') for path in api.requests)
            print(f'PASS {variant}: source={result}', flush=True)
        if not fixture: print('PASS E4 diagnostic steps, exit status and read-only request log', flush=True)
    finally:
        api.close(); owner.close()


if __name__ == '__main__': main()
