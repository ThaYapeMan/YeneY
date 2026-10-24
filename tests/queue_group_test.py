"""Coordinator/member field replays through the real binary, loopback only."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import time
import http.client
from urllib.parse import urlsplit
from xml.sax.saxutils import escape
from queue_binary_test import Binary

WRITES={'SetAVTransportURI','AddURIToQueue','Play','Pause','Stop','Seek',
        'RemoveTrackRangeFromQueue','SetCrossfadeMode','SaveQueue','DestroyObject'}

def coordinator():
    h=Binary(codec='l',grouped=2)
    try:
        for index in range(1,5):
            h.state(index)
            h.until(lambda:len(h.packets('STMs'))==index)
            assert h.model.playing==index
            if index==2:h.sonos.grouped=False
            if index==3:h.sonos.grouped=2
        assert len(h.sonos.actions('Play'))==1
        assert len(h.sonos.actions('AddURIToQueue'))==4
        assert not h.packets('STMn') and 'stop' not in h.commands
        text=(h.base/'bridge.log').read_text()
        assert 'group-rate-undecided' not in text
        assert text.count('core: queue command strm s')==4,text
        print('PASS 13:50 grouped coordinator ALAC replay: four starts/items, three joins, matching LMS index, membership changes do not interrupt, no playlist walk',flush=True)
    finally:h.close()

def member():
    for mode in ('queue','stream'):
        h=Binary(rates=(44100,),member=True,mode=mode)
        try:
            h.until(lambda:h.commands.count('stop')==1)
            time.sleep(.5)
            assert h.commands.count('stop')==1 and not h.packets('STMn'),h.commands
            assert not [r for r in h.sonos.requests if r[0] in WRITES],h.sonos.requests
            text=(h.base/'bridge.log').read_text()
            assert text.count('yeney: room is group member room=Study coordinator=Other; not playing')==1,text
            print('PASS group member '+mode+': no transport writes, one raw-MAC LMS stop, one member log, no STMn',flush=True)
        finally:h.close()

def moves_away():
    h=Binary(rates=(44100,),grouped=True)
    try:
        h.state(1,5);h.until(lambda:len(h.packets('STMs'))==1)
        before=len([r for r in h.sonos.requests if r[0] in WRITES])
        with h.sonos.lock:
            h.sonos.coordinator_uuid='RINCON_OTHER'
            h.sonos.uri='x-rincon:RINCON_OTHER'
            h.sonos.position['TrackURI']=''
        h.until(lambda:h.commands.count('stop')==1,6)
        time.sleep(.5)
        assert len([r for r in h.sonos.requests if r[0] in WRITES])==before
        assert not h.packets('STMn')
        assert 'LMS mutation reason=speaker-source-change command=stop' in (h.base/'bridge.log').read_text()
        print('PASS coordinator becomes member/source changes: existing external-stop path, one LMS stop, no write to new source',flush=True)
    finally:h.close()

def topology_event():
    h=Binary(rates=(44100,),grouped=True)
    try:
        h.state(1,5);h.until(lambda:len(h.packets('STMs'))==1)
        h.until(lambda:bool(getattr(h.sonos,'topology_callback','')))
        before=len([r for r in h.sonos.requests if r[0] in WRITES])
        h.sonos.coordinator_uuid='RINCON_OTHER'
        topology='<ZoneGroupState><ZoneGroups><ZoneGroup Coordinator="RINCON_OTHER"><ZoneGroupMember UUID="RINCON_TEST" ZoneName="Study" Location="http://127.0.0.1:1400/description.xml"/><ZoneGroupMember UUID="RINCON_OTHER" ZoneName="Other" Location="http://127.0.0.1:1400/description.xml"/></ZoneGroup></ZoneGroups></ZoneGroupState>'
        body='<e:propertyset xmlns:e="urn:schemas-upnp-org:event-1-0"><e:property><ZoneGroupState>'+escape(topology)+'</ZoneGroupState></e:property></e:propertyset>'
        url=urlsplit(h.sonos.topology_callback);c=http.client.HTTPConnection(url.netloc,timeout=3)
        c.request('NOTIFY',url.path,body,headers={'NT':'upnp:event','NTS':'upnp:propchange','SID':'uuid:binary','SEQ':'1'})
        response=c.getresponse();response.read();assert response.status==200;c.close()
        h.until(lambda:h.commands.count('stop')==1,3)
        time.sleep(.3)
        assert h.commands.count('stop')==1 and not h.packets('STMn')
        assert len([r for r in h.sonos.requests if r[0] in WRITES])==before
        assert 'LMS mutation reason=speaker-source-change command=stop' in (h.base/'bridge.log').read_text()
        print('PASS topology GENA role-only change: existing external stop once, no transport writes, no new periodic polling',flush=True)
    finally:h.close()

def remote_role_change():
    h=Binary(rates=(44100,),consume_stream=True,remote_sources=True,seconds=8)
    try:
        h.until(lambda:len(h.sonos.actions('Play'))==1)
        h.until(lambda:'path=stream reason=remote-source' in (h.base/'bridge.log').read_text())
        h.until(lambda:bool(getattr(h.sonos,'topology_callback','')))
        before=len([r for r in h.sonos.requests if r[0] in WRITES])
        h.sonos.coordinator_uuid='RINCON_OTHER';h.sonos.grouped=True
        h.sonos.uri='x-rincon:RINCON_OTHER'
        topology='<ZoneGroupState><ZoneGroups><ZoneGroup Coordinator="RINCON_OTHER"><ZoneGroupMember UUID="RINCON_TEST" ZoneName="Study" Location="http://127.0.0.1:1400/description.xml"/><ZoneGroupMember UUID="RINCON_OTHER" ZoneName="Other" Location="http://127.0.0.1:1400/description.xml"/></ZoneGroup></ZoneGroups></ZoneGroupState>'
        body='<e:propertyset xmlns:e="urn:schemas-upnp-org:event-1-0"><e:property><ZoneGroupState>'+escape(topology)+'</ZoneGroupState></e:property></e:propertyset>'
        url=urlsplit(h.sonos.topology_callback);c=http.client.HTTPConnection(url.netloc,timeout=3)
        c.request('NOTIFY',url.path,body,headers={'NT':'upnp:event','NTS':'upnp:propchange','SID':'uuid:binary','SEQ':'1'})
        response=c.getresponse();response.read();assert response.status==200;c.close()
        h.until(lambda:h.commands.count('stop')==1,3)
        time.sleep(.5)
        assert h.commands.count('stop')==1 and not h.packets('STMn'),h.commands
        assert len([r for r in h.sonos.requests if r[0] in WRITES])==before
        print('PASS queue remote stream loses coordinator: one serialized LMS stop across stream/relay observers, no transport writes or STMn',flush=True)
    finally:h.close()

def fault_limit():
    h=Binary(rates=(64000,)*4)
    try:
        h.until(lambda:h.commands.count('stop')==1)
        time.sleep(.5)
        assert len(h.packets('STMn'))<=2 and h.model.streaming<=2,(h.lms.packets,h.model.streaming)
        assert not h.sonos.actions('AddURIToQueue')
        assert 'track-fault-limit' in (h.base/'bridge.log').read_text()
        print('PASS consecutive track faults: second fault within 10 s stops once, at most two STMn, no playlist walk',flush=True)
    finally:h.close()

def stream_status():
    h=Binary(rates=(44100,44100),mode='stream',seconds=2,consume_stream=True)
    try:
        h.until(lambda:len(h.packets('STMs'))>=1)
        h.until(lambda:'| Title  Artist <one> - Title & 0' in (h.base/'bridge.log').read_text())
        h.until(lambda:len(h.sources)>=2)
        with h.sonos.lock:
            h.sonos.clock_base=time.monotonic()-2
            h.sonos.clocked=True
        h.until(lambda:len(h.packets('STMs'))>=2)
        h.until(lambda:'| Title  Artist <one> - Title & 1' in (h.base/'bridge.log').read_text())
        text=(h.base/'bridge.log').read_text()
        assert '0:00:24' in text,text
        assert len(h.sonos.actions('SetAVTransportURI'))==1
        assert h.sonos.stream_metadata['title']=='Artist <one> - Title & 0'
        print('PASS continuous stream status: confirmed LMS track change refreshes title/duration, no DIDL/URI/transport change',flush=True)
    finally:h.close()

if __name__=='__main__':
    coordinator();member();moves_away();topology_event();remote_role_change();fault_limit();stream_status()
