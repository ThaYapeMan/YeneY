"""Queue controller + real finite HTTP store against a loopback Sonos coordinator."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import copy
import os
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import queue
import shlex
import struct
import subprocess
import tempfile
tempfile.tempdir = (os.environ.get('TMPDIR') or '/var/tmp')
import threading
import time
import unittest
from unittest.mock import Mock
from types import SimpleNamespace
from urllib.parse import urlsplit
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape, quoteattr

ROOT = Path(__file__).resolve().parents[1]
SOAP = 'http://schemas.xmlsoap.org/soap/envelope/'
SERVICES = {name: ('urn:schemas-sonos-com:service:Queue:1' if name == 'Queue' else
                  'urn:schemas-upnp-org:service:'+name+':1')
            for name in ('AVTransport', 'Queue', 'ContentDirectory', 'ZoneGroupTopology','RenderingControl')}
PATHS = {name: '/advertised/'+name for name in SERVICES}


def didl(entries, saved=False):
    body = ''
    for item in entries:
        if saved:
            body += ('<container id='+quoteattr(item['id'])+'><dc:title>'+escape(item['title'])+
                     '</dc:title><dc:date>'+item['date']+'</dc:date></container>')
        else:
            body += '<item><res>'+escape(item['uri'])+'</res></item>'
    return ('<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" '
            'xmlns:dc="http://purl.org/dc/elements/1.1/">'+body+'</DIDL-Lite>')


class Coordinator(ThreadingHTTPServer):
    daemon_threads = True
    def __init__(self, address=('127.0.0.1', 0), handler=None):
        super().__init__(address, handler or Handler)
        self.closing = False
        self.lock = threading.RLock()
        self.original = [{'uri': 'http://foreign.invalid/original.flac'}]
        self.items = copy.deepcopy(self.original)
        self.saved = [dict(id='SQ:1', title='Unrelated favorite', date='2026-10-17T12:00:00Z')]
        self.saved += [dict(id='SQ:'+str(i+2), title=f'Before YeneY 2026-10-18 12:{i:02}',
                            date=f'2026-10-18T12:{i:02}:00Z') for i in range(7)]
        self.update, self.saved_update, self.next_id = 20, 1, 9
        self.crossfade, self.playing, self.uri = True, False, ''
        self.volume=23
        self.position = dict(Track='0', TrackURI='', RelTime='00:00:00')
        self.transport_state = 'STOPPED'
        self.play_mode = 'NORMAL'
        self.requests, self.errors, self.snapshots = [], [], {}
        self.fail_save = self.hide_save = self.race_add = self.ignore_crossfade = self.race_page = False
        self.last_save, self.verified_save = None, False
        self.busy, self.max_busy, self.grouped = 0, 0, False
        self.worker = threading.Thread(target=self.serve_forever)
        self.worker.start()

    def close(self):
        if getattr(self,'device_audio',None):self.device_audio.close()
        self.closing = True
        self.shutdown(); self.server_close(); self.worker.join()
        assert not self.errors, self.errors

    def add_foreign(self, uri='http://foreign.invalid/added-during-play.flac'):
        with self.lock:
            self.items.append({'uri': uri}); self.update += 1

    def actions(self, name):
        with self.lock:
            return [r for r in self.requests if r[0] == name]


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *_): pass

    def send(self, body, status=200):
        body = body.encode()
        self.send_response(status)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        try:
            self.end_headers(); self.wfile.write(body)
        except ConnectionError:
            if not self.server.closing:
                raise # live client failures still fail the fixture

    def do_GET(self):
        try:
            assert self.path == '/description.xml'
            services = ''.join('<service><serviceType>'+urn+'</serviceType><controlURL>'+PATHS[name]+
                               '</controlURL></service>' for name, urn in SERVICES.items())
            self.send('<root><device><UDN>uuid:RINCON_TEST</UDN><serviceList>'+services+
                      '</serviceList></device></root>')
        except Exception as error:
            self.server.errors.append(repr(error)); self.send('', 500)

    def do_POST(self):
        s = self.server
        with s.lock:
            s.busy += 1; s.max_busy = max(s.max_busy, s.busy)
        try:
            time.sleep(.001)  # reveal overlapping controller RPCs without slowing the suite
            node = ET.fromstring(self.rfile.read(int(self.headers['Content-Length']))).find('{'+SOAP+'}Body')[0]
            urn, action = node.tag[1:].split('}')
            service = next(k for k, v in SERVICES.items() if v == urn)
            args = {child.tag: child.text or '' for child in node}
            assert self.path == PATHS[service]
            assert self.headers['SOAPAction'] == '"'+urn+'#'+action+'"'
            with s.lock:
                s.requests.append((action, args, service, time.monotonic()))
                result, fault = self.action(service, action, args)
                s.busy -= 1
            if fault:
                self.send(f'<s:Envelope xmlns:s="{SOAP}"><s:Body><s:Fault><detail><UPnPError>'
                          f'<errorCode>{fault}</errorCode><errorDescription>fixture rejection</errorDescription>'
                          '</UPnPError></detail></s:Fault></s:Body></s:Envelope>', 500)
            else:
                values = ''.join('<'+k+'>'+escape(str(v))+'</'+k+'>' for k, v in result.items())
                self.send(f'<s:Envelope xmlns:s="{SOAP}"><s:Body><u:{action}Response xmlns:u="{urn}">'
                          f'{values}</u:{action}Response></s:Body></s:Envelope>')
        except (ET.ParseError, UnicodeError):
            with s.lock:
                s.busy = max(0, s.busy-1)
            self.send(f'<s:Envelope xmlns:s="{SOAP}"><s:Body><s:Fault><detail><UPnPError>'
                      '<errorCode>402</errorCode><errorDescription>Invalid Args</errorDescription>'
                      '</UPnPError></detail></s:Fault></s:Body></s:Envelope>', 500)
        except Exception as error:
            with s.lock:
                s.errors.append(repr(error)); s.busy = max(0, s.busy-1)
            self.send('', 500)

    def action(self, service, action, a):
        s = self.server
        writes = {'SetAVTransportURI','AddURIToQueue','Play','Pause','Stop','Seek',
                  'RemoveTrackRangeFromQueue','SetCrossfadeMode','SaveQueue','DestroyObject'}
        if getattr(s, 'reject_writes', False) and action in writes: return {}, '712'
        if action == 'SetCrossfadeMode' and a['CrossfadeMode'] == '1' and getattr(s, 'restore_fault', ''):
            return {}, s.restore_fault
        if action == 'Stop' and getattr(s, 'stop_fault', ''): return {}, s.stop_fault
        if action == 'GetZoneGroupState':
            other = ('<ZoneGroupMember UUID="RINCON_OTHER" ZoneName="Other" Location="http://127.0.0.1:'+str(s.server_port)+'/description.xml"/>') if s.grouped else ''
            if s.grouped and int(s.grouped)>1:
                other += '<ZoneGroupMember UUID="RINCON_SECOND" ZoneName="Second" Location="http://127.0.0.1:'+str(s.server_port)+'/description.xml"/>'
            topology = ('<ZoneGroupState><ZoneGroups><ZoneGroup Coordinator="'+getattr(s,'coordinator_uuid','RINCON_TEST')+'" ID="test">'
                        '<ZoneGroupMember UUID="RINCON_TEST" ZoneName="Study" Location="http://127.0.0.1:'+str(s.server_port)+
                        '/description.xml"/>'+other+'</ZoneGroup></ZoneGroups></ZoneGroupState>')
            return {'ZoneGroupState': topology}, None
        if action == 'Browse':
            if service == 'Queue':
                assert a['QueueID'] == '0'
                if s.race_page and int(a['StartingIndex']):
                    s.race_page = False; s.update += 1
                entries, revision, saved = s.items, s.update, False
            else:
                assert service == 'ContentDirectory' and a['ObjectID'] == 'SQ:'
                assert a['BrowseFlag'] == 'BrowseDirectChildren'
                entries = [p for p in s.saved if not (s.hide_save and p['id'] == s.last_save)]
                revision, saved = s.saved_update, True
                if any(p['id'] == s.last_save for p in entries): s.verified_save = True
            start, count = int(a['StartingIndex']), int(a['RequestedCount'])
            page = entries[start:start+count]
            return dict(Result=didl(page, saved), NumberReturned=len(page), TotalMatches=len(entries), UpdateID=revision), None
        assert service == 'AVTransport' or action == 'DestroyObject'
        if action == 'SaveQueue':
            if s.fail_save: return {}, '701'
            assert a['Title'].startswith('Before YeneY ') and not a['ObjectID']
            item = dict(id='SQ:'+str(s.next_id), title=a['Title'], date='2026-10-19T12:34:00Z')
            s.next_id += 1; s.saved.append(item); s.saved_update += 1
            s.last_save, s.verified_save = item['id'], False
            s.snapshots[item['id']] = copy.deepcopy(s.items)
            return {'AssignedObjectID': item['id']}, None
        if action == 'DestroyObject':
            item = next(p for p in s.saved if p['id'] == a['ObjectID'])
            assert item['title'].startswith('Before YeneY ') and s.verified_save
            s.saved.remove(item); s.saved_update += 1
            return {}, None
        if action == 'GetMediaInfo': return {'CurrentURI': s.uri, 'NrTracks': str(len(s.items))}, None
        if action == 'GetTransportSettings': return {'PlayMode': s.play_mode}, None
        if action == 'GetPositionInfo': return dict(s.position), None
        if action == 'GetTransportInfo': return {'CurrentTransportState': s.transport_state}, None
        if action == 'GetCrossfadeMode': return {'CrossfadeMode': int(s.crossfade)}, None
        if action == 'SetCrossfadeMode':
            if getattr(s, 'crossfade_race', False):
                race=s.crossfade_race
                s.crossfade_race=False
                if race != 'own': s.uri='x-sonos-vli:spotify-raced-admission'
                return {}, '712'
            if s.uri.startswith('x-sonos-vli:'): return {}, '712'
            if getattr(s, 'crossfade_fault', None): return {}, s.crossfade_fault
            assert s.verified_save
            if not s.ignore_crossfade: s.crossfade = a['CrossfadeMode'] == '1'
            return {}, None
        if action == 'RemoveTrackRangeFromQueue':
            if int(a['UpdateID']) != s.update: return {}, '803'
            assert s.verified_save
            first, count = int(a['StartingIndex'])-1, int(a['NumberOfTracks'])
            del s.items[first:first+count]; s.update += 1
            return {'NewUpdateID': s.update}, None
        if action == 'AddMultipleURIsToQueue': return {}, '402'
        if action == 'AddURIToQueue':
            assert set(a) == {'InstanceID', 'EnqueuedURI', 'EnqueuedURIMetaData',
                              'DesiredFirstTrackNumberEnqueued', 'EnqueueAsNext'}
            if getattr(s, 'fail_add', False): return {}, '402'
            if s.race_add:
                s.race_add = False; s.add_foreign()
            assert not s.crossfade and a['InstanceID'] == '0'
            assert a['EnqueueAsNext'] == '0'
            before=int(a['DesiredFirstTrackNumberEnqueued'])
            assert 0 <= before <= len(s.items)+1
            uri = a['EnqueuedURI']; u = urlsplit(uri)
            assert u.hostname == '127.0.0.1' and u.path == '/music/queue.wav'
            metadata = ET.fromstring(a['EnqueuedURIMetaData'])
            assert metadata.tag == '{urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/}DIDL-Lite'
            assert len(metadata) == 1 and metadata[0].tag.rsplit('}', 1)[-1] == 'item'
            item = metadata[0]
            # All queue submissions, including remote-finite continuations, need
            # these elements even when disabled/unknown (Sonos discards otherwise).
            expected = ['{http://purl.org/dc/elements/1.1/}title',
                        '{http://purl.org/dc/elements/1.1/}creator',
                        '{urn:schemas-upnp-org:metadata-1-0/upnp/}artist',
                        '{urn:schemas-upnp-org:metadata-1-0/upnp/}album']
            assert [node.tag for node in item][:4] == expected, a['EnqueuedURIMetaData']
            assert len([n for n in item if n.tag.rsplit('}', 1)[-1] == 'res']) == 1
            fields = {n.tag.rsplit('}', 1)[-1]: n.text for n in item}
            resource = next(n for n in item if n.tag.rsplit('}', 1)[-1] == 'res')
            assert fields['class'] == 'object.item.audioItem.musicTrack'
            assert fields['title'].startswith(getattr(s, 'expected_title_prefix', 'Title & '))
            assert fields.get('album') in getattr(s, 'expected_albums', ['Album & fixture'])
            assert resource.text == uri and resource.attrib['protocolInfo'] == 'http-get:*:audio/wav:*'
            assert resource.attrib['bitsPerSample'] == '24' and resource.attrib['nrAudioChannels'] == '2'
            insertion=before-1 if before else len(s.items)
            s.items.insert(insertion,dict(uri=uri, metadata=fields, resource=resource.attrib)); s.update += 1
            return dict(NumTracksAdded=1, FirstTrackNumberEnqueued=insertion+1, NewQueueLength=len(s.items), NewUpdateID=s.update), None
        if action == 'SetAVTransportURI':
            if a.get('CurrentURIMetaData'):
                ET.fromstring(a['CurrentURIMetaData']) # strict XML 1.0, including nested DIDL
            assert a['CurrentURI'] == 'x-rincon-queue:RINCON_TEST#0'
            s.uri = a['CurrentURI']; return {}, None
        if action == 'Seek':
            if a['Unit'] == 'TRACK_NR':
                track = int(a['Target'])
                assert 1 <= track <= len(s.items)
                s.position = dict(Track=str(track), TrackURI=s.items[track-1]['uri'], RelTime='00:00:00')
            else:
                assert a['Unit'] == 'REL_TIME'
                s.position['RelTime'] = ':'.join(f'{int(v):02}' for v in a['Target'].split(':'))
            return {}, None
        if action in ('Pause', 'Stop'):
            s.transport_state = 'PAUSED_PLAYBACK' if action == 'Pause' else 'STOPPED'
            return {}, None
        if action == 'Play':
            assert s.uri == 'x-rincon-queue:RINCON_TEST#0' and not s.crossfade
            s.playing = True
            if getattr(s, 'follow_controls', False):
                s.transport_state = 'PLAYING'
            return {}, None
        raise AssertionError((service, action, a))


def request(uri, method='GET', byte_range=None):
    u = urlsplit(uri); assert u.hostname == '127.0.0.1'
    connection = http.client.HTTPConnection(u.hostname, u.port, timeout=3)
    connection.request(method, u.path+'?'+u.query, headers={} if byte_range is None else {'Range': byte_range})
    response = connection.getresponse()
    return connection, response

def serialized_audio(method):
    def call(self,*args,**kwargs):
        with self.audio_lock:return method(self,*args,**kwargs)
    return call

class FiniteDeviceAudio:
    """Opt-in device model: immutable WAV length, prefetch, disconnect and Range.

    The control-only fixtures retain their explicit position controls. Audio
    acceptance attaches this model and calls tick from GetPositionInfo.
    """
    def __init__(self, server, speed=1, progressive=False):
        self.audio_lock=threading.RLock();self.active=False
        self.progressive=progressive;self.real_eof=False
        self.server=server;self.speed=speed;self.cache={};self.requests=[]
        self.audio=bytearray();self.current=None;self.frame=0;self.stamp=time.monotonic()
        self.paused=False;self.connection=None;self.prefetch=[]
        self.closed=threading.Event()
        def play_audio():
            try:
                while not self.closed.wait(.02) and not server.closing:
                    self.tick()
            except Exception as error:
                server.errors.append('audio reader: '+repr(error))
                print('DEVICE AUDIO ERROR '+repr(error),flush=True)
        self.reader=threading.Thread(target=play_audio,daemon=True);self.reader.start()
    def start(self):
        with self.audio_lock:self.stamp=time.monotonic();self.active=True
    def close(self):
        self.closed.set();self.reader.join(5)
        assert not self.reader.is_alive(),'device audio reader did not stop'
        if self.connection:self.response.close();self.connection.close();self.connection=None
    def fetch(self, item, offset=0, resume=False):
        if resume:
            c,r=request(item['uri']);r.read(16384);r.close();c.close()
            self.requests.append(('GET',item['uri'],0,'probe-closed'))
        c,r=request(item['uri'],byte_range=f'bytes={offset}-' if offset else None)
        try:
            try:body=r.read()
            except http.client.IncompleteRead as error:body=error.partial
            self.requests.append(('GET',item['uri'],offset,r.status))
            return body
        finally:c.close()
    def open_current(self,offset):
        if self.connection:self.response.close();self.connection.close()
        self.connection,self.response=request(self.current['uri'],byte_range=f'bytes={offset}-' if offset else None)
        assert self.response.status==(206 if offset else 200)
        self.requests.append(('GET',self.current['uri'],offset,self.response.status))
        if not offset:assert self.response.read(44)==self.cache[self.current['uri']][:44]
    @serialized_audio
    def pause(self):
        self.tick();self.paused=True
        if self.connection:self.response.close();self.connection.close();self.connection=None
        self.server.transport_state='PAUSED_PLAYBACK'
    @serialized_audio
    def resume(self):
        if self.current:
            c,r=request(self.current['uri']);r.read(16384);r.close();c.close()
            self.requests.append(('GET',self.current['uri'],0,'probe-closed'))
            self.open_current(44+self.frame*6)
        self.paused=False;self.stamp=time.monotonic();self.server.transport_state='PLAYING'
    @serialized_audio
    def transitioning(self):
        self.tick();self.server.transport_state='TRANSITIONING'
        self.transition_until=time.monotonic()+.5
    def rebuffer(self):self.transitioning()
    @serialized_audio
    def seek(self,seconds):
        self.tick()
        body=self.cache[self.current['uri']];rate=int.from_bytes(body[24:28],'little')
        self.frame=min((len(body)-44)//6,int(seconds*rate))
        self.open_current(44+self.frame*6)
        self.transitioning()
    @serialized_audio
    def tick(self):
        if not self.active:return
        s=self.server;now=time.monotonic()
        if s.transport_state=='TRANSITIONING' and now>=getattr(self,'transition_until',float('inf')):
            s.transport_state='PLAYING';self.stamp=now
        if s.transport_state!='PLAYING' or self.paused:
            self.stamp=now;return
        uri=s.position['TrackURI']
        item=next((i for i in s.items if i['uri']==uri),None)
        if not item or 'resource' not in item:return
        if self.current is not item:
            self.current=item;self.frame=0
            if self.progressive:
                if self.connection:self.response.close();self.connection.close()
                self.connection,self.response=request(uri)
                self.requests.append(('GET',uri,0,self.response.status))
                self.cache[uri]=self.response.read(44)
                assert len(self.cache[uri])==44
                self.real_eof=False
            else:
                if uri not in self.cache:self.cache[uri]=self.fetch(item)
                self.open_current(0)
            self.stamp=time.monotonic();return
        body=self.cache[uri];rate=int.from_bytes(body[24:28],'little')
        declared=int.from_bytes(body[40:44],'little')//6
        real=declared if self.progressive else (len(body)-44)//6
        limit=min(declared,real)
        target=min(limit,self.frame+int((now-self.stamp)*rate*self.speed))
        index=s.items.index(item)
        def consume(to):
            count=(to-self.frame)*6
            received=self.response.read(count) if count else b''
            if self.progressive:
                assert len(received)%6==0
                self.real_eof=len(received)<count
            else:assert received==body[44+self.frame*6:44+to*6],'device Range changed PCM'
            self.audio.extend(received);self.frame+=len(received)//6
        threshold=max(0,declared-int(6.5*rate))
        if index+1<len(s.items) and s.items[index+1]['uri'] not in self.cache and target>=threshold:
            consume(max(self.frame,threshold))
            following=s.items[index+1]
            self.prefetch.append((uri,(declared-self.frame)/rate))
            self.cache[following['uri']]=self.fetch(following)
        consume(target);self.stamp=now
        elapsed=self.frame//rate
        s.position['RelTime']=f'{elapsed//3600:02}:{elapsed//60%60:02}:{elapsed%60:02}'
        if self.frame==limit or self.real_eof:
            if index+1<len(s.items):
                following=s.items[index+1]
                s.position=dict(Track=str(index+2),TrackURI=following['uri'],RelTime='00:00:00')
                self.current=None
            else:
                s.transport_state='STOPPED'
                if self.connection:self.response.close();self.connection.close();self.connection=None
