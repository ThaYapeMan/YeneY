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
        self.info_variant = None
        self.info_extra = {}
        self.requests = []
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
                headers = ('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                           f'Sec-WebSocket-Accept: {accept}\r\nSec-WebSocket-Protocol: v1.api.smartspeaker.audio\r\n'
                           'X-Unknown: harmless\r\n\r\n')
                if api.variant == 'upgrade-case':
                    headers = ('http/1.1 101 switching protocols\r\nuPgRaDe: WebSocket\r\ncOnNeCtIoN: keep-alive, uPgRaDe\r\n'
                               f'sEc-WeBsOcKeT-aCcEpT: {accept}\r\nsEc-WeBsOcKeT-pRoToCoL: V1.API.SMARTSPEAKER.AUDIO\r\n\r\n')
                if api.variant == 'split-tcp':
                    for at in range(0, len(headers), 3): self.connection.sendall(headers[at:at+3].encode())
                else: self.connection.sendall(headers.encode())
                api.connections.add(self.connection)
                lock = threading.Lock()
                done = threading.Event()
                subscribed = set()

                def send(header, body):
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
                    with lock:
                        if api.variant == 'split-tcp':
                            for at in range(0, len(wire), 3): self.connection.sendall(wire[at:at+3])
                        else: self.connection.sendall(wire)

                def state():
                    with owner.lock:
                        return (owner.position['TrackURI'], owner.transport_state)

                def events():
                    previous = state()
                    while not done.wait(.01):
                        current = state()
                        if current != previous:
                            try:
                                if 'playback:1' in subscribed:
                                    send({'namespace': 'playback:1', 'command': 'playbackStatus'}, {})
                                if 'playbackMetadata:1' in subscribed:
                                    send({'namespace': 'playbackMetadata:1', 'command': 'metadataStatus'}, {})
                            except OSError:
                                return
                            previous = current
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
                        if opcode & 15 == 10:
                            continue
                        assert opcode == 0x81
                        header, body = json.loads(bytes(c ^ mask[i % 4] for i, c in enumerate(value)))
                        api.commands.append((time.monotonic(), header))
                        assert header['householdId'] == api.household
                        assert header['groupId'] == api.group
                        if header['command'] == 'subscribe':
                            if api.variant == 'slow-first' and not subscribed: time.sleep(.6)
                            subscribed.add(header['namespace'])
                            send(header, {})
                            continue
                        assert header['command'] == 'getPlaybackStatus'
                        api.polls += 1
                        if api.variant == 'interleaved': send({'command': 'metadataStatus'}, {})
                        if api.variant == 'slow-first' and api.polls == 1: time.sleep(.6)
                        delay = (api.spike_ms if api.spike_every and api.polls % api.spike_every == 0 else api.rtt_ms) / 2000
                        outlier = api.polls == api.outlier_poll
                        if outlier: delay = .125
                        time.sleep(delay)
                        with owner.lock:
                            position = owner.position.milliseconds()
                            item, transport = state()
                            body = {'positionMillis': round(position) + (400 if outlier else 0), 'itemId': item,
                                    'playbackState': 'PAUSED' if transport == 'PAUSED_PLAYBACK' else transport}
                        time.sleep(delay)
                        send(header, body)
                except (OSError, ValueError):
                    pass
                finally:
                    done.set()
                    worker.join()
                    api.connections.discard(self.connection)

        self.server = ThreadingHTTPServer((host, port), Handler)
        self.server.daemon_threads = True
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(cert), str(key))
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.worker = threading.Thread(target=self.server.serve_forever)
        self.worker.start()

    def disconnect(self):
        for connection in list(self.connections):
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def close(self):
        self.disabled = True
        self.disconnect()
        self.server.shutdown()
        self.server.server_close()
        self.worker.join()
        self.directory.cleanup()
