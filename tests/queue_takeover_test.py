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
            assert len(h.sonos.actions('SaveQueue'))==saves+1
            assert h.commands.count('stop')==cycle+1
        print('PASS '+('Study regrouped' if member else 'MBR Spotify reclaim after 7 s')+': 3 cycles, no takeover writes, one LMS stop per cycle, local release, fresh admission and foreign queue backup',flush=True)
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

if __name__=='__main__':
    replay(True);replay(False);cleanup();source_classes();gena_before_append()
