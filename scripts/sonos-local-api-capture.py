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
import ssl
import tarfile
import time

spec = importlib.util.spec_from_file_location('timing_probe', Path(__file__).with_name('sonos-timing-probe.py'))
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class Buffer:
    def __init__(self, data): self.data = data
    def makefile(self, *_): return io.BytesIO(self.data)


def capture(host, port, seconds, destination=None):
    context = ssl._create_unverified_context()
    files, identities = {}, {}

    def remember(value, context=None, scalar_ids=False):
        if isinstance(value, dict):
            for key, child in value.items():
                name = key.lower()
                kind = next((k for k in ('household', 'group', 'player', 'serial') if k in name), None)
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
        return context.wrap_socket(socket.create_connection((host, port), timeout=3), server_hostname=host)

    with connect() as connection:
        connection.sendall((f'GET /api/v1/players/local/info HTTP/1.1\r\nHost: {host}:{port}\r\n'
                            f'X-Sonos-Api-Key: {probe.API_KEY}\r\nConnection: close\r\n\r\n').encode())
        raw = bytearray()
        while True:
            data = connection.recv(16384)
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
    try:
        key = base64.b64encode(os.urandom(16)).decode()
        client.socket.sendall((f'GET /websocket/api HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n'
                               f'Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: {key}\r\n'
                               f'Sec-WebSocket-Protocol: {probe.PROTOCOL}\r\nX-Sonos-Api-Key: {probe.API_KEY}\r\n\r\n').encode())
        headers = bytearray()
        while not headers.endswith(b'\r\n\r\n'):
            headers.extend(client.read(1))
            if len(headers) > 16384: raise RuntimeError('upgrade headers exceed 16 KiB')
        files['websocket-upgrade.http'] = bytes(headers)
        upgrade = http.client.HTTPResponse(Buffer(bytes(headers)))
        upgrade.begin()
        accept = base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        if upgrade.status != 101 or upgrade.getheader('Sec-WebSocket-Accept') != accept:
            raise RuntimeError('invalid upgrade')
        for serial, namespace, command in ((1, 'playback:1', 'subscribe'), (2, 'playbackMetadata:1', 'subscribe'),
                                           (3, 'playback:1', 'getPlaybackStatus')):
            header = dict(namespace=namespace, command=command, householdId=info['householdId'],
                          groupId=info['groupId'], cmdId=str(serial))
            client.send(json.dumps([header, {}]).encode())
        end = time.monotonic() + seconds
        events, answers = [], set()
        total = 0
        while time.monotonic() < end:
            client.socket.settimeout(min(.5, max(.01, end-time.monotonic())))
            # Readiness polling keeps a timeout from discarding a partial frame.
            import select
            if not client.socket.pending() and not select.select([client.socket], [], [], .1)[0]: continue
            client.socket.settimeout(3)
            value = client.receive()
            remember(value)
            encoded = json.dumps(dict(elapsed=seconds-(end-time.monotonic()), message=value), ensure_ascii=False).encode()
            total += len(encoded)
            if total > 16*1024*1024: raise RuntimeError('event capture exceeds 16 MiB')
            events.append(encoded)
            if isinstance(value, list) and value and str(value[0].get('cmdId')) in ('1', '2', '3'):
                answers.add(str(value[0]['cmdId']))
        if answers != {'1', '2', '3'}: raise RuntimeError('missing subscription or status answers')
        files['messages.jsonl'] = b'\n'.join(events) + b'\n'
    finally:
        client.close()
    files['local-info.json'] = info_body
    files['README.txt'] = (b'Read-only capture: local/info, upgrade, two subscriptions, one getPlaybackStatus, then events.\n'
                           b'Identifiers replaced consistently before writing. Header spelling/order, body text, chunk extensions and trailers retained.\n'
                           b'Chunk sizes and Content-Length are recalculated after redaction; extra chunks may hold longer placeholders.\n')

    def redact(data):
        for original, replacement in sorted(identities.items(), key=lambda pair: -len(pair[0])):
            data = data.replace(json.dumps(original, ensure_ascii=True)[1:-1].encode(), replacement.encode())
            data = data.replace(original.encode(), replacement.encode())
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
            data = redacted_info() if name == 'local-info.http' else redact(data)
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
    print(capture(args.ip, args.port, args.seconds))


if __name__ == '__main__': main()
