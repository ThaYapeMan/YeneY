# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Loopback-only Sonos local API, with adjustable symmetric network delay."""
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time


class LocalApi:
    def __init__(self, owner, port=1443, host='127.0.0.1'):
        self.websocket_url = f'wss://{host}:{port}/websocket/api'
        self.owner = owner
        owner.local_api_clock = self
        self.group = 'loopback-group-1'
        self.household = 'loopback-household'
        self.rtt_ms = 10
        self.spike_ms = 50
        self.spike_every = 5
        self.polls = 0
        self.outlier_poll = 0
        self.info_reads = 0
        self.handshakes = []
        self.commands = []
        self.connections = set()
        self.disabled = False
        self.variant = 'chunked'
        self.shape = 'capture'
        self.events_enabled = True
        self.answer_profile = os.environ.get('YENEY_FAKE_ANSWER_PROFILE')
        self.events_enabled = os.environ.get('YENEY_FAKE_IGNORE_EVENTS') != '1'
        self.state_override = None
        self.item_override = None
        self.senders = {}
        self.emit_status = lambda body: self.broadcast('playbackStatus', body)
        self.emit_error = lambda body: self.broadcast('playbackError', body)
        self.emit_metadata = lambda body: self.broadcast('metadataStatus', body)
        self.command_error = None
        self.quality = dict(bitDepth=24, sampleRate=44100, lossless=True, immersive=False)
        self.silent = False
        self.partial_frame = False
        self.answer_gate = None
        self.status_captured = threading.Event()
        self.buffering_seconds = .3
        self.buffering_until = 0
        self.last_clock_stamp = None
        self.info_variant = None
        self.info_extra = {}
        self.info_headers = {}
        self.requests = []
        self.control_senders = {}
        self.controls = []
        self.ping_interval = 60
        self.subscription_lifetime = 3 * 24 * 60 * 60
        self.ping_before_reply = False
        self.drop_status_answers = False
        self.reply_delay_once = 0
        self.answer_pings = True
        self.tls_sessions = []
        self.reply_times = []
        self.directory = tempfile.TemporaryDirectory(prefix='sonos-ws-')
        root = Path(self.directory.name)
        key, cert = root / 'key.pem', root / 'cert.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(key),
                        '-out', str(cert), '-days', '1', '-subj', '/CN=loopback'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        api = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *_):
                pass

            def do_GET(self):
                api.requests.append(self.path)
                if api.disabled:
                    self.send_error(503)
                    return
                if self.path == '/api/v1/players/local/info':
                    api.info_reads += 1
                    body = json.dumps({'householdId': api.household, 'groupId': api.group, 'websocketUrl': api.websocket_url,
                                       'playerId': 'RINCON_CAPTURE_TEST', 'serialNumber': 'CAPTURE-SERIAL-001', **api.info_extra}).encode()
                    if api.variant == 'oversize': body = b'x' * 65537
                    mode = api.info_variant or api.variant
                    headers = ('HTTP/1.1 200 OK\r\nContent-type: application/json\r\n'
                               'X-Sonos-Type: discoveryInfo\r\nServer: Linux UPnP/1.0 Sonos/98.0-82170 (ZPS23)\r\n'
                               'Connection: close\r\n')
                    headers += ''.join(f'{name}: {value}\r\n' for name, value in api.info_headers.items())
                    if mode == 'length':
                        headers += f'Content-Length: {len(body)}\r\n'
                        wire = body
                    elif mode == 'close': wire = body
                    else:
                        headers += 'Transfer-encoding: chunked\r\n'
                        extension = ';device=port' if mode == 'extensions' else ''
                        wire = f'{len(body):x}{extension}\r\n'.encode() + body + b'\r\n0\r\n'
                        if mode == 'extensions': wire += b'X-Extra: ignored\r\n'
                        wire += b'\r\n'
                        if mode == 'truncated': wire = b'100\r\nx'
                        if mode == 'id-chunks':
                            wire = b''.join(f'{len(body[at:at+7]):x};part=yes\r\n'.encode() + body[at:at+7] + b'\r\n'
                                            for at in range(0, len(body), 7)) + b'0\r\nX-Extra: retained\r\n\r\n'
                    if mode == 'mixed-case': headers = headers.swapcase()
                    wire = headers.encode() + b'\r\n' + wire
                    if mode == 'split-tcp':
                        for at in range(0, len(wire), 3): self.connection.sendall(wire[at:at+3])
                    else: self.connection.sendall(wire)
                    self.close_connection = True
                    return
                assert self.path == '/websocket/api'
                self.close_connection = True
                assert self.headers['Sec-WebSocket-Protocol'] == 'v1.api.smartspeaker.audio'
                assert self.headers['X-Sonos-Api-Key'] == '123e4567-e89b-12d3-a456-426655440000'
                api.handshakes.append(dict(self.headers))
                accept = base64.b64encode(hashlib.sha1((self.headers['Sec-WebSocket-Key'] +
                                                       '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
                headers = ('HTTP/1.1 101 Switching Protocols\r\nSec-WebSocket-Protocol: v1.api.smartspeaker.audio\r\n'
                           f'Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n'
                           'Server: Linux UPnP/1.0 Sonos/86.11-82170 (ZPS12)\r\n\r\n')
                if api.variant == 'upgrade-case':
                    headers = ('http/1.1 101 switching protocols\r\nuPgRaDe: WebSocket\r\ncOnNeCtIoN: keep-alive, uPgRaDe\r\n'
                               f'sEc-WeBsOcKeT-aCcEpT: {accept}\r\nsEc-WeBsOcKeT-pRoToCoL: V1.API.SMARTSPEAKER.AUDIO\r\n\r\n')
                if api.variant == 'split-tcp':
                    for at in range(0, len(headers), 3): self.connection.sendall(headers[at:at+3].encode())
                else: self.connection.sendall(headers.encode())
                api.connections.add(self.connection)
                api.tls_sessions.append((self.connection.version(), self.connection.cipher()[0]))
                lock = threading.Lock()
                done = threading.Event()
                subscribed = set()
                expiry = {}
                request_times = {}
                connected = time.monotonic()
                next_ping = connected + api.ping_interval

                def control(opcode, body):
                    assert len(body) <= 125
                    with lock:
                        self.connection.sendall(bytes([0x80 | opcode, len(body)]) + body)
                    api.controls.append(dict(when=time.monotonic(), direction='sent', opcode=opcode))

                api.control_senders[self.connection] = control

                def send(header, body):
                    if api.shape == 'capture':
                        header = dict(header)
                        name = header.pop('command', header.get('type', 'none'))
                        header.update(householdId=api.household, locationId='loopback-location', groupId=api.group)
                        if 'cmdId' in header:
                            header.update(response=name, success=not bool(body.get('errorCode')), type='globalError' if body.get('errorCode') else 'playbackStatus' if name == 'getPlaybackStatus' else 'none')
                        else:
                            header.update(name=name, type=name)
                    if api.variant == 'extra-fields': body = dict(future={'nested': [1, True, None]}, **body)
                    if api.variant == 'field-order':
                        header = dict(reversed(list(header.items())))
                        body = dict(reversed(list(body.items())))
                    if api.variant == 'numeric-string' and 'positionMillis' in body: body['positionMillis'] = str(body['positionMillis'])
                    if api.variant == 'optional-missing': body.pop('itemId', None)
                    value = json.dumps([header, body]).encode()
                    def frame(data, opcode):
                        return (bytes([opcode, len(data)]) if len(data) < 126 else bytes([opcode, 126]) + struct.pack('!H', len(data))) + data
                    wire = frame(value, 0x81)
                    if api.variant == 'fragmented':
                        cut = len(value)//2
                        wire = frame(value[:cut], 1) + frame(b'ping', 0x89) + frame(value[cut:], 0x80)
                    if api.silent: return
                    if api.partial_frame: wire = wire[:3]
                    with lock:
                        if api.variant == 'split-tcp':
                            for at in range(0, len(wire), 3): self.connection.sendall(wire[at:at+3])
                        else: self.connection.sendall(wire)
                    if header.get('response', header.get('command')) == 'getPlaybackStatus':
                        api.reply_times.append((id(self.connection), time.monotonic(),
                                                (time.monotonic()-request_times[header['cmdId']])*1000))

                last_change = [None, 0, 0]
                last_previous = ['', 0]
                def state():
                    with owner.lock:
                        return (owner.position['TrackURI'], owner.transport_state, owner.position.stamp)

                def status():
                    with owner.lock:
                        current = state()
                        position = owner.position.milliseconds()
                        if last_change[0] != current:
                            last_change[:] = [current, time.monotonic(), position]
                            if api.last_clock_stamp != current and api.shape == 'capture' and owner.transport_state == 'PLAYING':
                                api.buffering_until = time.monotonic()+api.buffering_seconds
                            api.last_clock_stamp = current
                        transport = owner.transport_state
                        buffering = transport == 'PLAYING' and time.monotonic() < api.buffering_until
                        mapped = 'BUFFERING' if buffering else 'PAUSED' if transport == 'PAUSED_PLAYBACK' else 'IDLE' if transport == 'STOPPED' else transport
                        if api.shape == 'tidy':
                            return dict(positionMillis=round(position), itemId=current[0], playbackState='PAUSED' if transport == 'PAUSED_PLAYBACK' else transport)
                        body = dict(_objectType='playbackStatus', playbackState=api.state_override or 'PLAYBACK_STATE_'+mapped,
                                    isDucking=False, queueVersion='7', queueId='loopback-queue',
                                    itemId=api.item_override or str(owner.position.get('Track', '1')),
                                    positionMillis=round(last_change[2] if buffering else position), previousItemId=last_previous[0], previousPositionMillis=last_previous[1],
                                    playModes=dict(_objectType='playMode', repeat=False, repeatOne=False, shuffle=False, crossfade=False),
                                    availablePlaybackActions=dict(_objectType='playbackAction', **{key: True for key in
                                        ('canPlay','canSkip','canSkipBack','canSkipToPrevious','canSeek','canPause','canStop','canRepeat','canRepeatOne','canCrossfade','canShuffle')}),
                                    sourceIsLanSwappable=True)
                        return body

                api.senders[self.connection] = (send, status)


                def events():
                    nonlocal next_ping
                    previous = status()
                    previous_stamp = state()
                    while not done.wait(.01):
                        now = time.monotonic()
                        try:
                            if api.ping_interval and now >= next_ping:
                                control(9, b'fixture-heartbeat')
                                next_ping = now + api.ping_interval
                            expired = [namespace for namespace, end in expiry.copy().items() if now >= end]
                            for namespace in expired:
                                expiry.pop(namespace, None); subscribed.discard(namespace)
                                send({'namespace': namespace, 'command': 'subscriptionExpired'}, {})
                        except OSError:
                            return
                        current = status()
                        stamp = state()
                        if current != previous or stamp != previous_stamp:
                            try:
                                if api.events_enabled and 'playback:1' in subscribed:
                                    # Position changes during steady playback are poll-only.
                                    changed = any(current.get(k) != previous.get(k) for k in ('playbackState', 'itemId')) or stamp != previous_stamp
                                    if changed:
                                        seek = stamp[:2] == previous_stamp[:2] and stamp[2] != previous_stamp[2]
                                        item_change = current.get('itemId') != previous.get('itemId')
                                        last_previous[:] = [previous.get('itemId', ''), previous.get('positionMillis', 0) if seek or item_change else current['positionMillis']]
                                        current.update(previousItemId=last_previous[0], previousPositionMillis=last_previous[1])
                                        send({'namespace': 'playback:1', 'command': 'playbackStatus'}, current)
                                if api.events_enabled and stamp[0] != previous_stamp[0] and 'playbackMetadata:1' in subscribed:
                                    metadata = {} if api.shape == 'tidy' else dict(container=dict(id=dict(objectId='yeney:loopback-1')),
                                        currentItem=dict(track=dict(name='Fixture', durationMillis=180000,
                                            quality=api.quality)), nextItem=dict(track=dict(name='Next fixture', durationMillis=180000, quality=api.quality)))
                                    send({'namespace': 'playbackMetadata:1', 'command': 'metadataStatus'}, metadata)
                            except OSError:
                                return
                            previous, previous_stamp = current, stamp
                worker = threading.Thread(target=events, daemon=True)
                worker.start()
                try:
                    while not api.disabled:
                        header = self.rfile.read(2)
                        if len(header) != 2:
                            return
                        opcode, length = header
                        size = length & 127
                        if size == 126:
                            size = struct.unpack('!H', self.rfile.read(2))[0]
                        assert length & 128 and size <= 65536
                        mask = self.rfile.read(4)
                        value = self.rfile.read(size)
                        payload = bytes(c ^ mask[i % 4] for i, c in enumerate(value))
                        if opcode & 15 == 8: break
                        if opcode & 15 in (9, 10):
                            api.controls.append(dict(when=time.monotonic(), direction='received', opcode=opcode & 15))
                            if opcode & 15 == 9 and api.answer_pings: control(10, payload)
                            continue
                        assert opcode == 0x81
                        header, body = json.loads(payload)
                        request_times[header['cmdId']] = time.monotonic()
                        api.commands.append((time.monotonic(), header))
                        assert header['householdId'] == api.household
                        if header['groupId'] != api.group:
                            send(header, dict(errorCode='ERROR_INVALID_OBJECT_ID', reason='group moved'))
                            continue
                        if api.command_error and header['command'] == 'subscribe':
                            send(header, dict(errorCode=api.command_error, reason='fixture command failure'))
                            continue
                        if header['command'] == 'subscribe':
                            if api.variant == 'slow-first' and not subscribed: time.sleep(.6)
                            subscribed.add(header['namespace'])
                            expiry[header['namespace']] = time.monotonic() + api.subscription_lifetime
                            send(header, {})
                            continue
                        assert header['command'] == 'getPlaybackStatus'
                        api.polls += 1
                        if api.ping_before_reply: control(9, b'command-in-flight')
                        if api.drop_status_answers: continue
                        if api.reply_delay_once:
                            delay_once = api.reply_delay_once; api.reply_delay_once = 0
                            time.sleep(delay_once)
                        if api.variant == 'interleaved': send({'command': 'metadataStatus'}, {})
                        if api.variant == 'slow-first' and api.polls == 1: time.sleep(.6)
                        if api.answer_profile:
                            interval = time.monotonic() - getattr(api, '_last_poll', time.monotonic())
                            api._last_poll = time.monotonic()
                            if api.answer_profile == 'cable': api.rtt_ms, api.spike_ms, api.spike_every = 10, 12, 5
                            else:
                                api.rtt_ms, api.spike_ms = (77, 227) if interval > .12 else (11, 50)
                                api.spike_every = 6
                        delay = (api.spike_ms if api.spike_every and api.polls % api.spike_every == 0 else api.rtt_ms) / 2000
                        outlier = api.polls == api.outlier_poll
                        if outlier: delay = .125
                        time.sleep(delay)
                        with owner.lock:
                            body = status()
                            if outlier: body['positionMillis'] += 400
                        time.sleep(delay)
                        gate = api.answer_gate
                        if gate is not None:
                            api.status_captured.set()
                            gate.wait(3)
                        send(header, body)
                except (OSError, ValueError):
                    pass
                finally:
                    done.set()
                    worker.join()
                    api.connections.discard(self.connection)
                    api.senders.pop(self.connection, None)
                    api.control_senders.pop(self.connection, None)

        self.server = ThreadingHTTPServer((host, port), Handler)
        self.server.daemon_threads = True
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(cert), str(key))
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.worker = threading.Thread(target=self.server.serve_forever)
        self.worker.start()

    def broadcast(self, name, body):
        for send, status in list(self.senders.values()):
            try:
                send({'namespace': 'playbackMetadata:1' if name == 'metadataStatus' else 'playback:1', 'command': name}, dict(body))
                if name == 'playbackError':
                    current = status()
                    current.update(previousItemId=current.get('itemId', ''), previousPositionMillis=current['positionMillis'])
                    send({'namespace': 'playback:1', 'command': 'playbackStatus'}, current)
            except OSError: pass

    def disconnect(self):
        for connection in list(self.connections):
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def control(self, opcode, body=b''):
        for send in list(self.control_senders.values()):
            try: send(opcode, body)
            except OSError: pass

    def regroup(self, group):
        self.group = group
        self.broadcast('groupCoordinatorChanged', dict(groupId=group))

    def close(self):
        self.disabled = True
        self.disconnect()
        self.server.shutdown()
        self.server.server_close()
        self.worker.join()
        self.directory.cleanup()
