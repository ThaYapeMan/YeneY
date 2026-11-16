#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Read-only local API wire capture, with stable identifier redaction."""
import argparse
import base64
import hashlib
import http.client
import importlib.util
import io
import json
import os
import re
from pathlib import Path
import socket
import struct
import select
import ipaddress
import urllib.parse
import ssl
import tarfile
import time

spec = importlib.util.spec_from_file_location('timing_probe', Path(__file__).with_name('sonos-timing-probe.py'))
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class Buffer:
    def __init__(self, data): self.data = data
    def makefile(self, *_): return io.BytesIO(self.data)


def read_chunk(connection, size, deadline):
    connection.setblocking(False)
    while time.monotonic() < deadline:
        remaining = min(.1, max(0, deadline-time.monotonic()))
        if not connection.pending() and not select.select([connection], [], [], remaining)[0]: continue
        try: return connection.recv(size)
        except (ssl.SSLWantReadError, ssl.SSLWantWriteError, socket.timeout, BlockingIOError): continue
    return None


class Messages:
    """Incremental nonblocking TLS/frame reader; readiness is only a hint."""
    def __init__(self, connection):
        self.connection = connection
        self.buffer = bytearray()
        self.fragments = bytearray()
        self.outgoing = bytearray()
        self.started = time.monotonic()
        self.controls = []
        connection.setblocking(False)

    def receive(self, timeout):
        end = time.monotonic() + timeout
        control_seen = False
        while True:
            if self.outgoing:
                try:
                    sent = self.connection.send(self.outgoing)
                    del self.outgoing[:sent]
                except (ssl.SSLWantReadError, ssl.SSLWantWriteError, BlockingIOError): pass
            if len(self.buffer) >= 2:
                first, length = self.buffer[:2]
                size, offset = length & 127, 2
                needed = 2 if size == 126 else 8 if size == 127 else 0
                if len(self.buffer) >= offset + needed:
                    if needed:
                        size = int.from_bytes(self.buffer[offset:offset+needed], 'big')
                        offset += needed
                    if size > 1024*1024: raise RuntimeError('frame exceeds 1 MiB')
                    if length & 128: raise RuntimeError('masked server frame')
                    if len(self.buffer) >= offset + size:
                        body = self.buffer[offset:offset+size]
                        del self.buffer[:offset+size]
                        opcode = first & 15
                        if opcode in (8, 9, 10):
                            record = dict(elapsed=time.monotonic()-self.started,
                                          control={8: 'close', 9: 'ping', 10: 'pong'}[opcode],
                                          direction='received', payload_bytes=len(body))
                            if opcode == 8:
                                record.update(code=int.from_bytes(body[:2], 'big') if len(body) >= 2 else 1005,
                                              reason=bytes(body[2:]).decode('utf-8', errors='replace'))
                            self.controls.append(record)
                            control_seen = True
                        if opcode == 8: raise EOFError(f'websocket close code={record["code"]} reason={record["reason"]}')
                        if opcode == 9:
                            if len(body) > 125: raise RuntimeError('oversize ping')
                            mask = os.urandom(4)
                            self.outgoing.extend(bytes([0x8a, 0x80 | len(body)]) + mask + bytes(c ^ mask[i%4] for i,c in enumerate(body)))
                            self.controls.append(dict(elapsed=time.monotonic()-self.started, control='pong',
                                                      direction='queued', payload_bytes=len(body)))
                            continue
                        if opcode == 10: continue
                        if opcode not in (0, 1): raise RuntimeError('unsupported opcode')
                        self.fragments.extend(body)
                        if len(self.fragments) > 1024*1024: raise RuntimeError('message exceeds 1 MiB')
                        if first & 128:
                            value = json.loads(self.fragments)
                            self.fragments.clear()
                            return value
                        continue
            remaining = end - time.monotonic()
            if remaining <= 0: return None
            if control_seen and not self.buffer and not self.connection.pending(): return None
            if not self.connection.pending() and not select.select([self.connection], [], [], remaining)[0]: return None
            try:
                data = self.connection.recv(16384)
            except (ssl.SSLWantReadError, ssl.SSLWantWriteError, socket.timeout, BlockingIOError):
                continue
            if not data: raise EOFError('connection dropped')
            self.buffer.extend(data)


def capture(host, port, seconds, destination=None):
    context = ssl._create_unverified_context()
    files, identities = {}, {}
    events = []
    ending = 'completed'
    raw = bytearray()
    info_body = b''
    response = None
    reader = None
    connections = dict(scope='this-capture', held=0, peak_tls=0, opened=0, peak_websocket=0,
                       external_connections='not-observed')

    class CountedSocket(ssl.SSLSocket):
        counted = False

        def close(self):
            if self.counted:
                connections['held'] -= 1
                self.counted = False
            return super().close()

    context.sslsocket_class = CountedSocket

    def remember(value, context=None, scalar_ids=False):
        if isinstance(value, dict):
            for key, child in value.items():
                name = key.lower()
                kind = next((k for k in ('household', 'group', 'player', 'serial', 'device', 'location', 'queue') if k in name), None)
                if name == 'coordinatorid': kind = 'player'
                identity = kind if kind and ('id' in name or 'serial' in name) else context if name == 'id' else None
                if identity and isinstance(child, str) and child:
                    identities.setdefault(child, f'REDACTED_{identity.upper()}_{len(identities)+1}')
                remember(child, kind or context, bool(identity))
        elif isinstance(value, list):
            for child in value: remember(child, context, scalar_ids)
        elif scalar_ids and context and isinstance(value, str) and value:
            identities.setdefault(value, f'REDACTED_{context.upper()}_{len(identities)+1}')

    def connect():
        connection = context.wrap_socket(socket.create_connection((host, port), timeout=3), server_hostname=host)
        connection.counted = True
        connections['held'] += 1
        connections['opened'] += 1
        connections['peak_tls'] = max(connections['peak_tls'], connections['held'])
        return connection

    try:
        with connect() as connection:
            connection.sendall((f'GET /api/v1/players/local/info HTTP/1.1\r\nHost: {host}:{port}\r\n'
                                f'X-Sonos-Api-Key: {probe.API_KEY}\r\nConnection: close\r\n\r\n').encode())
            raw = bytearray()
            deadline = time.monotonic() + seconds
            while True:
                data = read_chunk(connection, 16384, deadline)
                if data is None:
                    ending = "duration elapsed during local/info"
                    break
                if not data: break
                raw.extend(data)
                if len(raw) > 1024*1024: raise RuntimeError('local/info capture exceeds 1 MiB')
        files['local-info.http'] = bytes(raw)
        response = http.client.HTTPResponse(Buffer(bytes(raw)))
        response.begin()
        if response.status != 200: raise RuntimeError(f'local/info HTTP {response.status}')
        info_body = response.read(65537)
        info = json.loads(info_body)
        remember(info)
        client = probe.SonosSocket.__new__(probe.SonosSocket)
        client.socket = connect()
        connections['peak_websocket'] = 1
        files['tls.txt'] = (f'protocol={client.socket.version()} cipher={client.socket.cipher()[0]}\n').encode()
        try:
            key = base64.b64encode(os.urandom(16)).decode()
            client.socket.sendall((f'GET /websocket/api HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n'
                                   f'Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: {key}\r\n'
                                   f'Sec-WebSocket-Protocol: {probe.PROTOCOL}\r\nX-Sonos-Api-Key: {probe.API_KEY}\r\n\r\n').encode())
            headers = bytearray()
            deadline = time.monotonic() + seconds
            while not headers.endswith(b'\r\n\r\n'):
                data = read_chunk(client.socket, 1, deadline)
                if data is None: raise EOFError('duration elapsed during upgrade')
                if not data: raise EOFError('connection dropped during upgrade')
                headers.extend(data)
                files['websocket-upgrade.http'] = bytes(headers)
                if len(headers) > 16384: raise RuntimeError('upgrade headers exceed 16 KiB')
            files['websocket-upgrade.http'] = bytes(headers)
            upgrade = http.client.HTTPResponse(Buffer(bytes(headers)))
            upgrade.begin()
            accept = base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
            if upgrade.status != 101 or upgrade.getheader('Sec-WebSocket-Accept') != accept:
                raise RuntimeError('invalid upgrade')
            client.socket.setblocking(True)
            for serial, namespace, command in ((1, 'playback:1', 'subscribe'), (2, 'playbackMetadata:1', 'subscribe'),
                                               (3, 'playback:1', 'getPlaybackStatus')):
                header = dict(namespace=namespace, command=command, householdId=info['householdId'],
                              groupId=info['groupId'], cmdId=str(serial))
                client.send(json.dumps([header, {}]).encode())
            end = time.monotonic() + seconds
            answers = set()
            reader = Messages(client.socket)
            total = 0
            while time.monotonic() < end:
                value = reader.receive(min(.5, max(.01, end-time.monotonic())))
                if value is None: continue
                remember(value)
                encoded = json.dumps(dict(elapsed=seconds-(end-time.monotonic()), message=value), ensure_ascii=False).encode()
                total += len(encoded)
                if total > 16*1024*1024: raise RuntimeError('event capture exceeds 16 MiB')
                events.append(encoded)
                if isinstance(value, list) and value and str(value[0].get('cmdId')) in ('1', '2', '3'):
                    answers.add(str(value[0]['cmdId']))
            if answers != {'1', '2', '3'}: ending = 'duration elapsed; missing subscription or status answers'
            files['messages.jsonl'] = b'\n'.join(events) + b'\n'
        finally:
            client.close()
        files['local-info.json'] = info_body
    except BaseException as error:
        ending = f'{type(error).__name__}: {error}'
    finally:
        files['messages.jsonl'] = b'\n'.join(events) + b'\n'
        files['local-info.http'] = bytes(raw)
        if reader and (reader.buffer or reader.fragments):
            files['partial-websocket.bin'] = bytes(reader.fragments + reader.buffer)
        if reader:
            files['websocket-controls.jsonl'] = ('\n'.join(json.dumps(value) for value in reader.controls)+'\n').encode()
            files['connection-ending.json'] = json.dumps(dict(elapsed=time.monotonic()-reader.started,
                                                             ending=ending)).encode()
        files['ending.txt'] = (ending + '\n').encode()
        files['connections.json'] = json.dumps(connections).encode()
    files['README.txt'] = (b'Read-only capture: local/info, upgrade, two subscriptions, one getPlaybackStatus, then events.\n'
                           b'Identifiers replaced consistently before writing. Header spelling/order, body text, chunk extensions and trailers retained.\n'
                           b'Chunk sizes and Content-Length are recalculated after redaction; extra chunks may hold longer placeholders.\n')

    def redact(data):
        for match in re.finditer(rb'"((?:device|location|queue|household|group|player)Id|serialNumber)"\s*:\s*"([^"\r\n]+)"', data, re.I):
            value = match[2].decode('utf-8', errors='replace')
            kind = re.sub('Id$|Number$', '', match[1].decode(), flags=re.I).upper()
            identities.setdefault(value, f'REDACTED_{kind}_{len(identities)+1}')
        key_pattern = r'((?:device|location|queue|household|group|player)Id|serialNumber)'
        decoded = urllib.parse.unquote(data.decode('utf-8', errors='replace'))
        for pattern in (r'(?im)^(?:X-Sonos-)?'+key_pattern+r':[ \t]*([^\r\n]+)',
                        r'(?i)'+key_pattern+r'=([^&\s"<>]+)'):
            for match in re.finditer(pattern, decoded):
                kind = re.sub('Id$|Number$', '', match[1], flags=re.I).upper()
                value = match[2].strip('" ')
                if value: identities.setdefault(value, f'REDACTED_{kind}_{len(identities)+1}')
        for original, replacement in sorted(identities.items(), key=lambda pair: -len(pair[0])):
            data = data.replace(json.dumps(original, ensure_ascii=True)[1:-1].encode(), replacement.encode())
            data = data.replace(original.encode('utf-8', errors='replace'), replacement.encode())
        # Discover addresses and owned object identifiers in every wire artifact.
        text = urllib.parse.unquote(data.decode('utf-8', errors='replace'))
        pattern = r'yeney:[A-Za-z0-9_-]+|(?:[0-9]{1,3}\.){3}[0-9]{1,3}|(?:[0-9A-Fa-f]{0,4}:){2,}[0-9A-Fa-f]{0,4}'
        for match in re.finditer(pattern, text):
            value = match.group()
            if not value.startswith('yeney:'):
                try: ipaddress.ip_address(value)
                except ValueError: continue
            identities.setdefault(value, f'REDACTED_ADDRESS_{len(identities)+1}')
        for original, replacement in sorted(identities.items(), key=lambda pair: -len(pair[0])):
            encoded = original.encode('utf-8', errors='replace')
            pattern = b''.join(b'(?:' + re.escape(bytes([byte])) + b'|(?i:%' + f'{byte:02x}'.encode() + b'))' for byte in encoded)
            data = re.sub(pattern, replacement.encode(), data)
        return data

    def redacted_info():
        headers, wire = bytes(raw).split(b'\r\n\r\n', 1)
        body = redact(info_body)
        if response.chunked:
            # Reassemble before replacing: an identifier may cross chunk boundaries.
            chunks, at, cursor = [], 0, 0
            while True:
                end = wire.index(b'\r\n', at)
                line = wire[at:end]
                size_text, separator, extension = line.partition(b';')
                size = int(size_text, 16)
                at = end + 2
                if not size:
                    if cursor < len(body):
                        tail = body[cursor:]
                        chunks.append(f'{len(tail):x}\r\n'.encode() + tail + b'\r\n')
                    chunks.append(redact(line + b'\r\n' + wire[at:]))
                    break
                payload = body[cursor:cursor+size]
                cursor += len(payload)
                if payload:
                    chunks.append(f'{len(payload):x}'.encode() + separator + redact(extension) + b'\r\n' + payload + b'\r\n')
                at += size + 2
            body = b''.join(chunks)
        else:
            headers = re.sub(rb'(?im)^(content-length:[ \t]*)[0-9]+',
                             lambda match: match[1] + str(len(body)).encode(), headers)
        return redact(headers) + b'\r\n\r\n' + body
    destination = Path(destination or f'/tmp/sonos-local-api-capture-{time.strftime("%Y%m%d-%H%M%S")}.tar.gz')
    # Write only redacted data; originals never reach disk.
    with tarfile.open(destination, 'w:gz') as archive:
        for name, data in files.items():
            data = redacted_info() if name == 'local-info.http' and response and info_body else redact(data)
            entry = tarfile.TarInfo(name); entry.size = len(data); entry.mode = 0o600
            archive.addfile(entry, io.BytesIO(data))
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ip', required=True)
    parser.add_argument('--port', type=int, default=1443)
    parser.add_argument('--seconds', type=float, default=60)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600: parser.error('--seconds must be 1..3600')
    result = capture(args.ip, args.port, args.seconds)
    with tarfile.open(result) as archive:
        print('connections ' + archive.extractfile('connections.json').read().decode())
    print(result)


if __name__ == '__main__': main()
