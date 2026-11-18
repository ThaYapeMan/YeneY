#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Read-only Sonos/LMS clock comparison. No transport or playlist writes."""
import argparse
import base64
import concurrent.futures
import hashlib
import http.client
import json
import math
import os
from pathlib import Path
import socket
import ssl
import statistics
import struct
import threading
import time
import urllib.parse
import xml.etree.ElementTree as ET

API_KEY = '123e4567-e89b-12d3-a456-426655440000'
PROTOCOL = 'v1.api.smartspeaker.audio'


def percentile(values, fraction):
    return sorted(values)[min(len(values) - 1, math.ceil(len(values) * fraction) - 1)] if values else float('nan')


def slope(points):
    if len(points) < 2:
        return float('nan')
    x0 = statistics.mean(x for x, _ in points)
    y0 = statistics.mean(y for _, y in points)
    denominator = sum((x - x0) ** 2 for x, _ in points)
    return sum((x - x0) * (y - y0) for x, y in points) / denominator if denominator else float('nan')


def summary(points):
    values = [y for _, y in points]
    if not values:
        return 'samples=0'
    share = lambda pred: 100 * sum(pred(v) for v in values) / len(values)
    jumps = sum(abs(b[1] - a[1]) > 150 for a, b in zip(points, points[1:]))
    return (f'samples={len(values)} median_ms={statistics.median(values):.3f} '
            f'below_500_pct={share(lambda v: v < 500):.3f} '
            f'900_1100_pct={share(lambda v: 900 <= v <= 1100):.3f} '
            f'above_1100_pct={share(lambda v: v > 1100):.3f} '
            f'within_30_pct={share(lambda v: abs(v) <= 30):.3f} '
            f'jumps_over_150_ms={jumps} creep_ms_per_s={slope(points):.6f}')


class SonosSocket:
    def __init__(self, host, port=1443):
        context = ssl._create_unverified_context()
        info = http.client.HTTPSConnection(host, port, context=context, timeout=3)
        try:
            info.request('GET', '/api/v1/players/local/info', headers={'X-Sonos-Api-Key': API_KEY})
            response = info.getresponse()
            if response.status != 200:
                raise RuntimeError(f'local info HTTP {response.status}')
            identity = json.loads(response.read(65537))
        finally:
            info.close()
        self.group = identity['groupId']
        self.household = identity['householdId']
        self.socket = context.wrap_socket(socket.create_connection((host, port), timeout=3), server_hostname=host)
        self.serial = 0
        key = base64.b64encode(os.urandom(16)).decode()
        self.socket.sendall((f'GET /websocket/api HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n'
                             f'Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: {key}\r\n'
                             f'Sec-WebSocket-Protocol: {PROTOCOL}\r\nX-Sonos-Api-Key: {API_KEY}\r\n\r\n').encode())
        headers = bytearray()
        while not headers.endswith(b'\r\n\r\n'):
            headers.extend(self.read(1))
            if len(headers) > 16384:
                raise RuntimeError('oversized HTTP header')
        expected = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
        if b' 101 ' not in headers or expected not in headers or PROTOCOL.encode() not in headers:
            self.close()
            raise RuntimeError('invalid WebSocket handshake')

    def close(self):
        self.socket.close()

    def read(self, size):
        out = bytearray()
        while len(out) < size:
            data = self.socket.recv(size - len(out))
            if not data:
                raise EOFError('WebSocket closed')
            out.extend(data)
        return bytes(out)

    def send(self, data, opcode=1):
        mask = os.urandom(4)
        size = len(data)
        header = bytes([0x80 | opcode, 0x80 | size]) if size < 126 else bytes([0x80 | opcode, 0xfe]) + struct.pack('!H', size)
        self.socket.sendall(header + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(data)))

    def receive(self):
        out = bytearray()
        while True:
            first, second = self.read(2)
            size = second & 127
            if second & 128:
                raise RuntimeError('masked server frame')
            if size == 126:
                size = struct.unpack('!H', self.read(2))[0]
            elif size == 127:
                size = struct.unpack('!Q', self.read(8))[0]
            if size + len(out) > 65536:
                raise RuntimeError('oversized WebSocket message')
            body = self.read(size)
            opcode = first & 15
            if opcode == 8:
                raise EOFError('WebSocket close')
            if opcode == 9:
                self.send(body, 10)
                continue
            if opcode == 10:
                continue
            if opcode not in (0, 1):
                raise RuntimeError('unexpected WebSocket opcode')
            out.extend(body)
            if first & 128:
                return json.loads(out)

    def status(self):
        self.serial += 1
        header = {'namespace': 'playback:1', 'command': 'getPlaybackStatus',
                  'householdId': self.household, 'groupId': self.group, 'cmdId': str(self.serial)}
        sent = time.monotonic()
        self.send(json.dumps([header, {}]).encode())
        while True:
            h, body = self.receive()
            if h.get('type') == 'error' or 'errorCode' in body:
                raise RuntimeError('Sonos command error')
            if str(h.get('cmdId')) == str(self.serial):
                received = time.monotonic()
                position = body.get('positionMillis')
                return {'kind': 'websocket', 'sent': sent, 'received': received, 'midpoint': (sent + received) / 2,
                        'position_ms': None if position is None else float(position),
                        'item': str(body.get('itemId', '')), 'state': body.get('playbackState', 'UNKNOWN')}


def upnp_position(host, port):
    connection = http.client.HTTPConnection(host, port, timeout=3)
    body = ('<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body>'
            '<u:GetPositionInfo xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
            '<InstanceID>0</InstanceID></u:GetPositionInfo></s:Body></s:Envelope>')
    sent = time.monotonic()
    try:
        connection.request('POST', '/MediaRenderer/AVTransport/Control', body=body,
                           headers={'Content-Type': 'text/xml; charset="utf-8"',
                                    'SOAPAction': '"urn:schemas-upnp-org:service:AVTransport:1#GetPositionInfo"'})
        response = connection.getresponse()
        text = response.read(262145)
        received = time.monotonic()
        if response.status != 200:
            raise RuntimeError(f'GetPositionInfo HTTP {response.status}')
    finally:
        connection.close()
    fields = {node.tag.split('}')[-1]: node.text or '' for node in ET.fromstring(text).iter()}
    position = None
    reltime = fields.get('RelTime')
    if reltime and reltime != 'NOT_IMPLEMENTED':
        h, m, s = map(float, reltime.split(':'))
        position = (h * 3600 + m * 60 + s) * 1000
    return {'kind': 'upnp', 'sent': sent, 'received': received, 'midpoint': (sent + received) / 2,
            'position_ms': position, 'item': fields.get('TrackURI', ''), 'state': ''}


class LmsClock:
    def __init__(self, host, port, mac):
        self.socket = socket.create_connection((host, port), timeout=3)
        self.file = self.socket.makefile('rb')
        self.mac = mac

    def close(self):
        self.file.close()
        self.socket.close()

    def sample(self):
        sent = time.monotonic()
        self.socket.sendall((self.mac + ' time ?\n').encode())
        line = self.file.readline(65536)
        received = time.monotonic()
        parts = urllib.parse.unquote(line.decode()).split()
        if len(parts) != 3 or parts[1] != 'time':
            raise RuntimeError('unexpected LMS time response')
        return {'kind': 'lms', 'sent': sent, 'received': received, 'midpoint': (sent + received) / 2,
                'position_ms': float(parts[2]) * 1000, 'item': '', 'state': ''}


def project(sample, at):
    return sample['position_ms'] + ((at - sample['midpoint']) * 1000 if sample['state'].removeprefix('PLAYBACK_STATE_') == 'PLAYING' else 0)


def reltime_summary(rows):
    ws = [r for r in rows if r['kind'] == 'websocket' and r['position_ms'] is not None
          and r['received'] - r['sent'] <= .2]
    upnp = [r for r in rows if r['kind'] == 'upnp' and r['position_ms'] is not None]
    if not ws or not upnp:
        print('RelTime samples insufficient')
        return
    offsets, edges = [], {'steady': [], 'boundary': [], 'seek': []}
    recent_event = None
    for a, b in zip(ws, ws[1:]):
        if a['item'] != b['item']:
            recent_event = ('boundary', b['midpoint'])
        elif abs(project(a, b['midpoint']) - b['position_ms']) > 150:
            recent_event = ('seek', b['midpoint'])
        if recent_event:
            b['event'] = recent_event
    for i, row in enumerate(upnp):
        reference = min(ws, key=lambda r: abs(r['midpoint'] - row['midpoint']))
        if abs(reference['midpoint'] - row['midpoint']) > .5:
            continue
        position = project(reference, row['midpoint'])
        offsets.append(position - row['position_ms'])
        if not i:
            continue
        previous = upnp[i - 1]
        if row['item'] != previous['item'] or row['position_ms'] - previous['position_ms'] != 1000:
            continue
        # A second-change lies in this observation bracket. Estimate its midpoint;
        # both observations remain in the TSV to preserve the uncertainty.
        event = reference.get('event')
        category = event[0] if event and 0 <= row['midpoint'] - event[1] < 2 else 'steady'
        edge_time = (previous['midpoint'] + row['midpoint']) / 2
        edges[category].append(abs(project(reference, edge_time) - row['position_ms']))
    if offsets:
        truncated = sum(0 <= v < 1000 for v in offsets)
        rounded = sum(-500 <= v < 500 for v in offsets)
        print(f'RelTime classification={"truncated" if truncated > rounded else "rounded" if rounded > truncated else "inconclusive"} '
              f'truncation_share_pct={100 * truncated / len(offsets):.3f} rounding_share_pct={100 * rounded / len(offsets):.3f}')
    for category, values in edges.items():
        print(f'RelTime second-change {category}: n={len(values)} median_ms={statistics.median(values) if values else float("nan"):.3f} '
              f'p95_ms={percentile(values, .95):.3f} max_ms={max(values) if values else float("nan"):.3f}')


def position_gaps(rows, finished):
    for kind in ('websocket', 'upnp'):
        gaps, count, start = [], 0, None
        for row in rows:
            if row['kind'] != kind:
                continue
            if row['position_ms'] is None:
                count += 1
                if start is None:
                    start = row['midpoint']
            elif start is not None:
                gaps.append((start, row['midpoint'], False))
                start = None
        if start is not None:
            gaps.append((start, finished, True))
        print(f'{kind} no_position_samples={count} gaps={len(gaps)}')
        for start, end, open_gap in gaps:
            print(f'{kind} no position start_mono={start:.9f} duration_s={end-start:.6f} '
                  f'ended={"run" if open_gap else "position"}')



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', choices=('websocket', 'both'), default='websocket')
    parser.add_argument('--speaker', required=True, help='numeric speaker IP, one room')
    parser.add_argument('--port', type=int, default=1443)
    parser.add_argument('--upnp-port', type=int, default=1400)
    parser.add_argument('--lms-host')
    parser.add_argument('--lms-port', type=int, default=9090)
    parser.add_argument('--player-mac')
    parser.add_argument('--duration', type=float, default=120)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.source == 'both' and (not args.lms_host or not args.player_mac):
        parser.error('--source both requires --lms-host and --player-mac')
    output = args.output or Path.cwd() / ('sonos-timing-' + time.strftime('%Y%m%d-%H%M%S') + '.tsv')
    ws = lms = thread = None
    exit_code = 0
    rows, pairs, errors = [], [], []
    stop = threading.Event()

    def reltime_worker():
        due = started
        while not stop.is_set() and time.monotonic() - started < args.duration:
            try:
                rows.append(upnp_position(args.speaker, args.upnp_port))
            except (OSError, RuntimeError, ValueError, ET.ParseError) as error:
                errors.append(str(error))
            due += .1
            stop.wait(max(0, due - time.monotonic()))

    try:
        ws = SonosSocket(args.speaker, args.port)
        if args.source == 'both':
            lms = LmsClock(args.lms_host, args.lms_port, args.player_mac)
        started = time.monotonic()
        thread = threading.Thread(target=reltime_worker)
        thread.start()
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            due = started
            while time.monotonic() - started < args.duration:
                future = pool.submit(ws.status)
                other = pool.submit(lms.sample) if lms else None
                sample = future.result()
                rows.append(sample)
                if other:
                    cli = other.result()
                    rows.append(cli)
                    if (sample['position_ms'] is not None and sample['received'] - sample['sent'] <= .2
                            and cli['received'] - cli['sent'] <= .2):
                        pairs.append((cli['midpoint'], project(sample, cli['midpoint']) - cli['position_ms']))
                due += .25 if lms else .2
                stop.wait(max(0, due - time.monotonic()))
    except KeyboardInterrupt:
        exit_code = 130
    except (OSError, EOFError, RuntimeError, ValueError, KeyError) as error:
        errors.append(str(error))
        exit_code = 1
    finally:
        stop.set()
        if thread:
            thread.join()
        finished = time.monotonic()
        if ws:
            ws.close()
        if lms:
            lms.close()
        rows.sort(key=lambda row: row['midpoint'])
        states = [row for row in rows if row['kind'] == 'websocket']
        for row in rows:
            if row['kind'] == 'upnp' and row['position_ms'] is None:
                nearest = min(states, key=lambda sample: abs(sample['midpoint'] - row['midpoint']), default=None)
                row['state'] = nearest['state'] if nearest else 'UNKNOWN'
        with output.open('w') as file:
            file.write('kind\tsent_mono\treceived_mono\tmidpoint_mono\trtt_ms\tposition_ms\titem\tstate\n')
            for row in rows:
                position = 'no position' if row['position_ms'] is None else f"{row['position_ms']:.3f}"
                file.write('\t'.join([row['kind'], f"{row['sent']:.9f}", f"{row['received']:.9f}",
                                      f"{row['midpoint']:.9f}", f"{(row['received']-row['sent'])*1000:.3f}",
                                      position, row['item'].replace('\t', ' ').replace('\n', ' '), row['state']]) + '\n')
        print('positionMillis - LMS time ?: ' + summary(pairs))
        reltime_summary(rows)
        position_gaps(rows, finished)
        print(f'errors={len(errors)} raw_tsv={output.resolve()}')
        if errors:
            print('first_error=' + errors[0])
    return exit_code


if __name__ == '__main__':
    raise SystemExit(main())
