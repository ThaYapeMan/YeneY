# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import importlib.util
import json
import http.client
from pathlib import Path
import tarfile
import tempfile
import threading
import time
from queue_sonos_fake import Coordinator
from sonos_websocket_fake import LocalApi

spec = importlib.util.spec_from_file_location('capture', Path(__file__).resolve().parents[1]/'scripts/sonos-local-api-capture.py')
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)
owner = Coordinator()
api = LocalApi(owner, port=0)
api.websocket_url = f'wss://127.0.0.1:{api.server.server_port}/websocket/api'
api.info_variant = 'id-chunks'
api.info_extra = {'groups': [{'id': api.group, 'playerIds': ['RINCON_ARRAY_ONLY']}], 'serialNumbers': ['ARRAY-SERIAL-002']}
try:
    for mode in ('id-chunks', 'length', 'close'):
        api.info_variant = mode
        api.commands.clear()
        owner.position = dict(Track='1', TrackURI='first', RelTime='00:00:00')
        owner.transport_state = 'PLAYING'
        with tempfile.TemporaryDirectory() as work:
            def change():
                time.sleep(.3)
                owner.position = dict(Track='2', TrackURI='second', RelTime='00:00:01')
                owner.transport_state = 'PAUSED_PLAYBACK'
            worker = threading.Thread(target=change)
            worker.start()
            result = capture.capture('127.0.0.1', api.server.server_port, 1, Path(work)/'capture.tar.gz')
            worker.join()
            with tarfile.open(result) as archive:
                contents = {name: archive.extractfile(name).read() for name in archive.getnames()}
            wire = contents['local-info.http']
            response = http.client.HTTPResponse(capture.Buffer(wire))
            response.begin()
            decoded = response.read()
            identity = json.loads(decoded)
            assert json.loads(contents['local-info.json']) == identity
            assert identity['groupId'].startswith('REDACTED_GROUP_')
            assert identity['groups'][0]['id'] == identity['groupId']
            assert b'RINCON_ARRAY_ONLY' not in decoded and b'CAPTURE-SERIAL-001' not in decoded
            if mode == 'id-chunks': assert b';part=yes' in wire and b'X-Extra: retained' in wire
            assert b'Connection: close' in wire
            if mode == 'id-chunks': assert b'Transfer-encoding: chunked' in wire
            assert b'101 Switching Protocols' in contents['websocket-upgrade.http']
            combined = b''.join(contents.values())
            assert api.household.encode() not in combined and api.group.encode() not in combined
            assert b'RINCON_ARRAY_ONLY' not in combined and b'ARRAY-SERIAL-002' not in combined
            assert b'RINCON_CAPTURE_TEST' not in combined and b'CAPTURE-SERIAL-001' not in combined
            assert b'REDACTED_PLAYER_' in combined and b'REDACTED_SERIAL_' in combined
            assert b'REDACTED_HOUSEHOLD_' in combined and b'REDACTED_GROUP_' in combined
            rows = [json.loads(row)['message'] for row in contents['messages.jsonl'].splitlines()]
            assert {row[0]['command'] for row in rows} >= {'subscribe', 'getPlaybackStatus'}
            assert any(row[0]['command'] == 'metadataStatus' for row in rows)
            assert len(api.commands) == 3 and all(h['command'] in ('subscribe', 'getPlaybackStatus') for _, h in api.commands)
            print('PASS R4 ' + mode + ': split-chunk identifiers, valid redacted HTTP framing, extensions/trailers; redacted raw HTTP/upgrade, both subscriptions, status and read-only request log')

finally:
    api.close(); owner.close()
