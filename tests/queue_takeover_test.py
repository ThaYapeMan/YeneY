"""October 25 Study/MBR takeover regressions; loopback fakes only."""
import json
import http.client
from urllib.parse import urlsplit
from xml.sax.saxutils import escape, quoteattr
import time
from queue_binary_test import Binary, HTTP
from queue_group_test import WRITES

SPOTIFY='x-sonos-vli:RINCON_FIXTURE:2,spotify:opaque-secret-token'

def status(h):
    return json.loads((h.base/('stream-pacing-'+b'Study'.hex()+'.json')).read_text())['playback']

def writes(h):
    return [r for r in h.sonos.requests if r[0] in WRITES]

def replay(member=False):
    h=Binary(rates=(44100,),seconds=24,initial_foreign=SPOTIFY if not member else None)
    try:
        h.state(1,5);h.until(lambda:len(h.packets('STMs'))==1)
        if not member: assert len(h.sonos.actions('SaveQueue'))==1
        for cycle in range(3):
            if not member: time.sleep(7 if cycle==0 else .2)
            with h.sonos.lock:
                before=len(writes(h))
                h.sonos.reject_writes=True
                h.sonos.uri=SPOTIFY
                h.sonos.transport_state='PAUSED_PLAYBACK'
                h.sonos.position.update(TrackURI=SPOTIFY)
                if member:
                    h.sonos.grouped=True;h.sonos.coordinator_uuid='RINCON_OTHER'
            h.until(lambda:h.commands.count('stop')==cycle+1)
            h.until(lambda:not status(h)['owned'])
            time.sleep(.4)
            assert len(writes(h))==before, writes(h)[before:]
            assert not h.packets('STMn')
            text=(h.base/'bridge.log').read_text()
            assert text.count('external takeover: source=')==cycle+1,text
            assert 'opaque-secret-token' not in text,text
            assert 'ownership already held' not in text and 'relay fault=' not in text,text
            # A new explicit LMS play admits the foreign Spotify queue afresh.
            with h.sonos.lock:
                h.sonos.reject_writes=False
                h.sonos.coordinator_uuid='RINCON_TEST'
                h.sonos.items=[dict(uri=SPOTIFY)]
                h.sonos.update+=1
            saves=len(h.sonos.actions('SaveQueue'))
            source=HTTP(h.bodies[0]);h.sources.append(source)
            h.lms.strm('s',source,fmt='f',rate=44100)
            h.until(lambda:len(h.sonos.actions('Play'))==cycle+2)
            h.state(1,5)
            h.until(lambda:status(h)['owned'])
            assert len(h.sonos.actions('SaveQueue'))==saves+(1 if member and cycle==0 else 0)
            assert h.commands.count('stop')==cycle+1
        print('PASS '+('Study regrouped' if member else 'MBR Spotify reclaim after 7 s')+': 3 cycles, no takeover writes, one LMS stop per cycle, local release, fresh admission and verified foreign queue backup reuse',flush=True)
    finally:h.close()

def cleanup():
    for fault in ('500','712','701','stop-712','stop-500'):
        h=Binary(rates=(44100,))
        try:
            h.state(1,5);h.until(lambda:len(h.packets('STMs'))==1)
            if fault.startswith('stop-'): h.sonos.stop_fault=fault[5:]
            else: h.sonos.restore_fault=fault
            h.lms.strm('q')
            h.until(lambda:not status(h)['owned'])
            h.until(lambda:'cleanup ' in (h.base/'bridge.log').read_text())
            text=(h.base/'bridge.log').read_text()
            assert text.count('queue cleanup ')==1,text
            assert not h.commands.count('stop') and 'relay fault=' not in text,text
            h.sonos.restore_fault='';h.sonos.stop_fault=''
            source=HTTP(h.bodies[0]);h.sources.append(source)
            h.lms.strm('s',source,fmt='f',rate=44100)
            h.until(lambda:len(h.sonos.actions('Play'))==2)
            assert 'ownership already held' not in (h.base/'bridge.log').read_text()
            print('PASS cleanup fault '+fault+': local release, one cleanup log, no LMS stop, next admission plays',flush=True)
        finally:h.close()

def gena_before_append():
    h=Binary(rates=(44100,44100))
    try:
        h.until(lambda:len(h.sonos.actions('Play'))==1)
        h.until(lambda:bool(getattr(h.sonos,'callback','')))
        before=[]
        def takeover():
            before.append(len(writes(h)))
            h.sonos.uri=SPOTIFY;h.sonos.reject_writes=True
            change='<Event><InstanceID val="0"><AVTransportURI val='+quoteattr(SPOTIFY)+'/></InstanceID></Event>'
            body='<e:propertyset xmlns:e="urn:schemas-upnp-org:event-1-0"><e:property><LastChange>'+escape(change)+'</LastChange></e:property></e:propertyset>'
            url=urlsplit(h.sonos.callback);connection=http.client.HTTPConnection(url.netloc,timeout=3)
            try:
                connection.request('NOTIFY',url.path,body,headers={'NT':'upnp:event','NTS':'upnp:propchange','SID':'uuid:binary','SEQ':'10'})
                response=connection.getresponse();response.read();assert response.status==200
            finally:connection.close()
        h.sonos.browse_hook=takeover
        h.state(1,5)
        h.until(lambda:h.commands.count('stop')==1)
        assert before and len(writes(h))==before[0],h.sonos.requests
        assert len(h.sonos.actions('AddURIToQueue'))==1
        assert not status(h)['owned']
        text=(h.base/'bridge.log').read_text()
        assert 'external takeover: source=spotify-connect' in text,text
        assert 'relay fault=' not in text and 'opaque-secret-token' not in text,text
        print('PASS GENA between poll and successor append: no AddURIToQueue or cleanup writes, external stop once, source class retained',flush=True)
    finally:h.close()

def stale_join():
    h=Binary(rates=(44100,44100),seconds=24)
    try:
        h.state(1,5);h.until(lambda:len(h.packets('STMs'))==1)
        h.until(lambda:len(h.items())==2)
        joined=[]
        def join():
            joined.append(time.monotonic())
            h.state(2,0)
        h.sonos.position_hook=join
        h.until(lambda:bool(joined))
        h.until(lambda:len(h.packets('STMs'))==2,timeout=.5)
        assert time.monotonic()-joined[0]<=.5
        h.until(lambda:status(h)['current_owned_item']==h.items()[1]['uri'])
        time.sleep(1.2)
        assert len(h.packets('STMs'))==2
        text=(h.base/'bridge.log').read_text()
        assert 'relay-change state=' in text,text
        assert 'stale poll answer discarded after GENA seq=' in text,text
        assert 'field=CurrentTrack old="2" new="1"' not in text,text
        print('PASS Study join replay: GENA 1->2, stale poll 2->1 discarded, fresh poll item 2; one STMs for item 2 within one poll interval, no flip-flop',flush=True)
    finally:h.close()

def source_classes():
    for uri, source in (
        ('x-rincon-queue:FOREIGN#0','queue'),
        ('x-rincon:FOREIGN','other-group member'),
        (SPOTIFY,'spotify-connect'),
        ('x-sonos-spotify:opaque-secret-token','streaming service'),
        ('x-sonos-http:opaque-secret-token','streaming service'),
        ('x-rincon-stream:opaque-secret-token','line-in'),
        ('mystery:opaque-secret-token','unknown')):
        h=Binary(rates=(44100,))
        try:
            h.state(1,5);h.until(lambda:len(h.packets('STMs'))==1)
            before=len(writes(h))
            with h.sonos.lock:
                h.sonos.reject_writes=True;h.sonos.uri=uri
                h.sonos.position.update(TrackURI=uri)
            h.until(lambda:h.commands.count('stop')==1)
            text=(h.base/'bridge.log').read_text()
            assert 'external takeover: source='+source in text,text
            assert 'crossfade restore skipped: transport owned by '+source in text,text
            assert 'opaque-secret-token' not in text,text
            assert len(writes(h))==before
            print('PASS source-class logging: '+source+'; foreign identifiers redacted; no writes',flush=True)
        finally:h.close()

def admission():
    h=Binary(rates=(44100,),initial_foreign=SPOTIFY,grouped=True)
    try:
        h.state(1,5);h.until(lambda:len(h.packets('STMs'))==1)
        names=[r[0] for r in h.sonos.requests]
        assert names.index('SaveQueue') < names.index('SetAVTransportURI') < names.index('SetCrossfadeMode') < names.index('AddURIToQueue') < names.index('Seek') < names.index('Play'),names
        assert 'GetMediaInfo' in names[names.index('SetAVTransportURI')+1:names.index('SetCrossfadeMode')],names
        assert len(h.sonos.actions('SetAVTransportURI'))==1
        assert len(h.sonos.actions('SaveQueue'))==1
        assert status(h)['state']=='PLAYING' and not h.commands.count('stop')
        assert 'relay fault=' not in (h.base/'bridge.log').read_text()
        print('PASS Study live Spotify coordinator admission: backup, confirmed queue switch, crossfade, AddURIToQueue, Seek, Play, PLAYING and STMs; no LMS stop or room fault',flush=True)
    finally:h.close()

def setting_race():
    for race in (True, 'own'):
        h=Binary(rates=(44100,),initial_foreign='x-rincon-queue:RINCON_TEST#0',crossfade_race=race)
        try:
            h.state(1,5);h.until(lambda:len(h.packets('STMs'))==1)
            text=(h.base/'bridge.log').read_text()
            assert text.count('admission retry after confirmed queue switch:')==1,text
            assert not h.commands.count('stop') and 'relay fault=' not in text,text
            assert len(h.sonos.actions('SetCrossfadeMode'))==2
            print('PASS pre-switch setting race: SetCrossfadeMode 712 retried once after confirmed queue switch; STMs, no room fault or LMS stop',flush=True)
        finally:h.close()

def own_queue_admission():
    h=Binary(rates=(44100,),initial_foreign='x-rincon-queue:RINCON_TEST#0')
    try:
        h.state(1,5);h.until(lambda:len(h.packets('STMs'))==1)
        names=[r[0] for r in h.sonos.requests]
        assert names.index('SaveQueue')<names.index('SetCrossfadeMode')<names.index('AddURIToQueue')<names.index('SetAVTransportURI')<names.index('Seek')<names.index('Play'),names
        assert len(h.sonos.actions('SetAVTransportURI'))==1 and not h.commands.count('stop')
        print('PASS October 25 MBR Spotify own-queue admission: backup/settings/enqueue/select/seek/play order unchanged',flush=True)
    finally:h.close()

def post_switch_failure():
    h=Binary(rates=(44100,),initial_foreign=SPOTIFY,crossfade_fault='712')
    try:
        h.until(lambda:h.commands.count('stop')==1)
        time.sleep(.4)
        text=(h.base/'bridge.log').read_text()
        assert text.count('relay fault=')==1 and 'action=SetCrossfadeMode fault=712' in text,text
        assert h.commands.count('stop')==1 and not h.sonos.actions('Play')
        names=[r[0] for r in h.sonos.requests]
        assert names.index('SetAVTransportURI')<names.index('SetCrossfadeMode')
        print('PASS genuine post-switch SetCrossfadeMode fault=712: one room fault, one LMS stop, action and fault logged',flush=True)
    finally:h.close()

def podcast_then_local():
    h=Binary(rates=(44100,),initial_foreign=SPOTIFY,remote_sources=True,consume_stream=True,seconds=8)
    try:
        h.until(lambda:len(h.sonos.actions('Play'))==1)
        h.until(lambda:len(h.packets('STMs'))==1)
        assert h.sonos.uri.startswith('http://')
        assert not h.sonos.actions('SaveQueue') and not h.sonos.actions('SetCrossfadeMode')
        assert not h.commands.count('stop')
        print('PASS podcast takes live Spotify via existing HTTP SetAVTransportURI and Play; no queue acquisition or LMS stop',flush=True)
        h.lms.strm('q')
        h.until(lambda:len(h.sonos.actions('Stop'))>=1)
        h.remote_sources=False
        source=HTTP(h.bodies[0]);h.sources.append(source)
        h.lms.strm('s',source,fmt='f',rate=44100)
        h.until(lambda:len(h.sonos.actions('Play'))==2)
        h.state(1,1);h.until(lambda:len(h.packets('STMs'))==2)
        text=(h.base/'bridge.log').read_text()
        assert 'relay fault=' not in text and 'fault=712' not in text,text
        assert not h.commands.count('stop') and status(h)['owned']
        print('PASS local track after Spotify-to-podcast takeover: fresh queue admission, PLAYING and STMs, no 712 or LMS stop',flush=True)
    finally:h.close()

if __name__=='__main__':
    admission();own_queue_admission();setting_race();post_switch_failure();podcast_then_local()
    stale_join();replay(True);replay(False);cleanup();source_classes();gena_before_append()
