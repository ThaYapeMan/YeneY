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
    def __init__(self, owner, port=1443):
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
                if api.disabled:
                    self.send_error(503)
                    return
                if self.path == '/api/v1/players/local/info':
                    api.info_reads += 1
                    body = json.dumps({'householdId': api.household, 'groupId': api.group}).encode()
                    self.send_response(200)
                    self.send_header('Content-Length', str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                assert self.path == '/websocket/api'
                self.close_connection = True
                assert self.headers['Sec-WebSocket-Protocol'] == 'v1.api.smartspeaker.audio'
                assert self.headers['X-Sonos-Api-Key'] == '123e4567-e89b-12d3-a456-426655440000'
                api.handshakes.append(dict(self.headers))
                accept = base64.b64encode(hashlib.sha1((self.headers['Sec-WebSocket-Key'] +
                                                       '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
                self.send_response(101)
                self.send_header('Upgrade', 'websocket')
                self.send_header('Connection', 'Upgrade')
                self.send_header('Sec-WebSocket-Accept', accept)
                self.send_header('Sec-WebSocket-Protocol', 'v1.api.smartspeaker.audio')
                self.end_headers()
                api.connections.add(self.connection)
                lock = threading.Lock()
                done = threading.Event()
                subscribed = set()

                def send(header, body):
                    value = json.dumps([header, body]).encode()
                    frame = bytes([0x81, len(value)]) if len(value) < 126 else b'\x81\x7e' + struct.pack('!H', len(value))
                    with lock:
                        self.connection.sendall(frame + value)

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
                            subscribed.add(header['namespace'])
                            send(header, {})
                            continue
                        assert header['command'] == 'getPlaybackStatus'
                        api.polls += 1
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

        self.server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
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
