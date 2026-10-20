# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Read-only Sonos local WebSocket measurement, RFC 6455, standard library."""
import base64
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

KEY = '123e4567-e89b-12d3-a456-426655440000'
PROTOCOL = 'v1.api.smartspeaker.audio'


def percentile(values, p):
    values=sorted(values)
    return values[min(len(values)-1,math.ceil(len(values)*p)-1)] if values else None


class WebSocket:
    def __init__(self, host, port=1443):
        context=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname=False
        context.verify_mode=ssl.CERT_NONE  # Local Sonos presents its own certificate.
        connection=http.client.HTTPSConnection(host,port,timeout=2,context=context)
        try:
            connection.request('GET','/api/v1/players/local/info',headers={'X-Sonos-Api-Key':KEY})
            response=connection.getresponse()
            if response.status!=200:
                raise ValueError('local info HTTP '+str(response.status))
            info=json.loads(response.read(1024*1024))
        finally:
            connection.close()
        self.household=info['householdId']; self.group=info['groupId']
        raw=socket.create_connection((host,port),timeout=2)
        self.socket=context.wrap_socket(raw,server_hostname=host)
        self.socket.settimeout(2)
        nonce=base64.b64encode(os.urandom(16)).decode()
        request=(f'GET /websocket/api HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                 f'Sec-WebSocket-Key: {nonce}\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: {PROTOCOL}\r\n'
                 f'X-Sonos-Api-Key: {KEY}\r\n\r\n')
        try:
            self.socket.sendall(request.encode())
            header=bytearray()
            while not header.endswith(b'\r\n\r\n'):
                if len(header)>16384:
                    raise ValueError('WebSocket handshake size')
                header.extend(self.read(1))
            lines=header.decode().split('\r\n')
            fields=dict((k.lower(),v.strip()) for k,v in (l.split(':',1) for l in lines[1:] if ':' in l))
            accept=base64.b64encode(hashlib.sha1((nonce+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
            if (' 101 ' not in lines[0] or fields.get('sec-websocket-accept')!=accept or
                    fields.get('sec-websocket-protocol')!=PROTOCOL):
                raise ValueError('WebSocket handshake rejected')
        except BaseException:
            self.socket.close(); raise
        self.serial=0

    def read(self, count):
        data=bytearray()
        while len(data)<count:
            part=self.socket.recv(count-len(data))
            if not part:
                raise EOFError('WebSocket closed')
            data.extend(part)
        return bytes(data)

    def status(self):
        self.serial+=1
        request_id=str(self.serial)
        # This is the only application message this module can send.
        payload=json.dumps([dict(namespace='playback:1',name='getPlaybackStatus',
                                  householdId=self.household,groupId=self.group,requestId=request_id),{}]).encode()
        mask=os.urandom(4)
        prefix=bytes([0x81,0x80|len(payload)]) if len(payload)<126 else bytes([0x81,0xfe])+struct.pack('!H',len(payload))
        sent=time.monotonic()
        self.socket.sendall(prefix+mask+bytes(v^mask[i%4] for i,v in enumerate(payload)))
        fragment=bytearray()
        while True:
            if time.monotonic()-sent>2:raise TimeoutError('getPlaybackStatus deadline')
            a,b=self.read(2); size=b&127
            if b&128:
                raise ValueError('masked server frame')
            if size==126:size=struct.unpack('!H',self.read(2))[0]
            elif size==127:size=struct.unpack('!Q',self.read(8))[0]
            if size>1024*1024 or len(fragment)+size>1024*1024:
                raise ValueError('WebSocket frame limit')
            body=self.read(size)
            opcode=a&15
            if opcode==8:raise EOFError('WebSocket close')
            if opcode in (9,10):
                # No pong/control commands are sent: keep this probe read-only.
                continue
            if opcode not in (0,1):raise ValueError('unexpected WebSocket opcode')
            fragment.extend(body)
            if not a&128:continue
            headers,body=json.loads(fragment);fragment.clear()
            if headers.get('requestId')!=request_id:
                if time.monotonic()-sent>2:raise ValueError('response correlation deadline')
                continue
            received=time.monotonic()
            if headers.get('type')=='error' or headers.get('success') is False or 'errorCode' in body or 'error' in body:
                raise ValueError('getPlaybackStatus error: '+json.dumps([headers,body]))
            return dict(send=sent,receive=received,midpoint=(sent+received)/2,
                        playbackState=body.get('playbackState'),itemId=body.get('itemId'),
                        positionMillis=body.get('positionMillis'),previousPositionMillis=body.get('previousPositionMillis'))

    def close(self):
        self.socket.close()


def analyze(rows, joins):
    """Reject impossible/out-of-order values; never promote an error to precision."""
    result={}
    for mode in ('queue','stream'):
        valid=[]; observed=[]; reasons=[]; last=None; segment=0
        for row in (r for r in rows if r.get('mode')==mode):
            if row.get('error'):
                reasons.append(row['error']);last=None;segment+=1;continue
            v=row.get('positionMillis'); previous=row.get('previousPositionMillis')
            if (not isinstance(v,(int,float)) or isinstance(v,bool) or not math.isfinite(v) or
                    v<0 or v>7*24*3600*1000):
                reasons.append('missing or implausible position/item');last=None;segment+=1;continue
            observed.append(row)
            if row.get('itemId') in (None,''):
                reasons.append('position present but itemId unavailable');last=None;segment+=1;continue
            # previousPositionMillis is recorded, never substituted for an invalid current position.
            if previous is not None and (not isinstance(previous,(int,float)) or not math.isfinite(previous) or abs(v-previous)>7*24*3600*1000):
                reasons.append('implausible previousPositionMillis');last=None;segment+=1;continue
            if last and (last['itemId']!=row['itemId'] or last.get('scenario')!=row.get('scenario') or last.get('playbackState')!=row.get('playbackState') or
                         abs(v-last['positionMillis']-(row['midpoint']-last['midpoint'])*1000)>2000):
                segment+=1
            item=dict(row,segment=segment)
            valid.append(item);last=row
        whole=sum(v['positionMillis']%1000==0 for v in observed)/len(observed) if observed else None
        buckets={}
        for v in observed:
            buckets.setdefault(int(v['midpoint']),set()).add((str(v.get('itemId')),v['positionMillis']))
        fits={}; residuals=[]
        for v in valid:
            if v.get('playbackState') not in ('PLAYBACK_STATE_PLAYING','PLAYING'):continue
            fits.setdefault((str(v['itemId']),v['segment']),[]).append(v)
        fit_rows=[]
        for (item,seg), points in fits.items():
            if len(points)<5 or len({v['positionMillis'] for v in points})<3:continue
            origin=points[0]['midpoint'];xs=[v['midpoint']-origin for v in points];ys=[v['positionMillis'] for v in points]
            xm=statistics.mean(xs);ym=statistics.mean(ys);den=sum((x-xm)**2 for x in xs)
            if not den:continue
            slope=sum((x-xm)*(y-ym) for x,y in zip(xs,ys))/den
            errors=[abs(y-(ym+slope*(x-xm))) for x,y in zip(xs,ys)]
            residuals+=errors
            fit_rows.append(dict(itemId=item,segment=seg,samples=len(points),slope_ms_per_s=slope,
                                 residual_median_ms=statistics.median(errors),residual_p95_ms=percentile(errors,.95),residual_max_ms=max(errors)))
        starts=[]
        for join in joins:
            if join.get('mode','queue')!=mode:continue
            near=[r for r in valid if abs(r['midpoint']-join['confirmed_monotonic'])<=.4 and r.get('scenario')==join['scenario']]
            if near:
                r=min(near,key=lambda r:abs(r['midpoint']-join['confirmed_monotonic']))
                starts.append(dict(scenario=join['scenario'],itemId=r['itemId'],
                    difference_ms=r['positionMillis']-(join['rel_time']*1000+(r['midpoint']-join['confirmed_monotonic'])*1000)))
        if not residuals:reasons.append('insufficient moving samples for a position fit')
        verdict=('unavailable' if not valid or not residuals else
                 'sub-second' if percentile(residuals,.95)<30 and whole<.1 else 'whole seconds')
        result[mode]=dict(position_present=bool(observed),samples=len(observed),whole_second_share=whole,
                          distinct_values_per_second={str(k):len(v) for k,v in buckets.items()},
                          distinct_per_second_median=statistics.median(map(len,buckets.values())) if buckets else None,
                          round_trip_median_ms=statistics.median([(v['receive']-v['send'])*1000 for v in observed]) if observed else None,
                          round_trip_p95_ms=percentile([(v['receive']-v['send'])*1000 for v in observed],.95),
                          residual_median_ms=statistics.median(residuals) if residuals else None,
                          residual_p95_ms=percentile(residuals,.95),residual_max_ms=max(residuals) if residuals else None,
                          fits=fit_rows,item_starts=starts,reasons=sorted(set(reasons)),verdict=verdict)
    return result


class Sampler:
    def __init__(self, host, work, context, port=1443):
        self.host,self.work,self.context,self.port=host,Path(work),context,port
        self.stop=threading.Event();self.rows=[]
        self.worker=threading.Thread(target=self.run,name='queue-device-ws',daemon=True)

    def start(self):self.worker.start()

    def run(self):
        with (self.work/'websocket.jsonl').open('a') as f:
            while not self.stop.is_set():
                client=None
                try:
                    client=WebSocket(self.host,self.port)
                    due=time.monotonic()
                    while not self.stop.is_set():
                        row=client.status();row.update(self.context());self.rows.append(row)
                        f.write(json.dumps(row)+'\n');f.flush()
                        due+=.2
                        self.stop.wait(max(0,due-time.monotonic()))
                        if time.monotonic()-due>1:due=time.monotonic()
                except Exception as error:
                    row=dict(self.context(),error=str(error),monotonic=time.monotonic())
                    self.rows.append(row);f.write(json.dumps(row)+'\n');f.flush()
                finally:
                    if client:client.close()
                self.stop.wait(5)  # Bounded retry also permits a later stream reference.

    def close(self):
        self.stop.set();self.worker.join(8)
        if self.worker.is_alive():
            raise RuntimeError('WebSocket sampler did not stop')
