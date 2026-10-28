"""Real bridge overflow/late-overflow qualification on loopback only."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import http.client
import time
from urllib.parse import urlsplit
from queue_binary_test import Binary

def fetch(uri):
    url=urlsplit(uri);conn=http.client.HTTPConnection(url.netloc,timeout=20)
    try:
        conn.request('GET',url.path+'?'+url.query);response=conn.getresponse()
        try:return response.read()
        except http.client.IncompleteRead as error:return error.partial
    finally:conn.close()

for late in (False,True):
    host=Binary(rates=(44100,44100),seconds=55,remote_sources=True,remote_duration=20,
                partial_source=late,queue_title_prefix='')
    try:
        host.until(lambda:len(host.items())>=1)
        host.state(1,20 if late else 0)
        host.until(lambda:any(e.get('metadata',{}).get('title')=='Title & 1' for e in host.items()),timeout=25)
        entries=host.items()
        following=next(i for i,e in enumerate(entries) if e.get('metadata',{}).get('title')=='Title & 1')
        assert following>=2
        assert [e['metadata']['title'] for e in entries[:following]]==['Title & 0']*following
        bodies=[fetch(e['uri']) for e in entries[:following]]
        actual=b''.join(b[44:] for b in bodies)
        expected=(b'\0\xe8\x03\0\x18\xfc')*(44100*55)
        assert actual==expected,(len(actual),len(expected))
        text=(host.base/'bridge.log').read_text()
        assert 'path=relay reason=remote-finite' in text
        if late:assert 'WARNING continuation inserted' in text,text[-10000:]
        else:assert 'prefetch_margin_s=15.' in text,text[-10000:]
        for i in range(1,following+1):
            host.state(i,0)
        assert len(host.packets('STMs'))==1,host.packets('STMs')
        assert not [c for c in host.commands if not c.startswith('status ') and not c.endswith('?')]
        print('PASS remote-finite real binary '+('late overflow: warning and retained audio' if late else 'early overflow: inserted >10 s before end')+
              f'; {following} ordered segments before successor; sample-exact 55 s; one LMS STMs, no extra LMS mutation',flush=True)
    finally:host.close()

# A completed episode may be paused while its continuation is selected. The
# exhausted LMS producer's q+s resume echo must retain the same Sonos asset.
host=Binary(rates=(44100,),seconds=55,remote_sources=True,remote_duration=20,
            resume_restart=True,queue_title_prefix='')
try:
    host.until(lambda:len(host.items())>=2)
    host.state(2,2)
    time.sleep(3.2)
    additions=len(host.sonos.actions('AddURIToQueue'))
    host.state(2,2,state='PAUSED_PLAYBACK')
    host.until(lambda:'pause 1' in host.commands)
    host.state(2,2,state='PLAYING')
    host.until(lambda:'resume echo adopted:' in (host.base/'bridge.log').read_text())
    host.until(lambda:len(host.packets('STMs'))>=2)
    assert len(host.sonos.actions('AddURIToQueue'))==additions
    mutations=[c for c in host.commands if not c.startswith('status ') and not c.endswith('?')]
    assert mutations==['pause 1','pause 0'],mutations
    assert host.items()[1]['metadata']['title']=='Title & 0'
    print('PASS continuation cached resume: parent LMS identity, pause/resume only, same immutable queue assets, no re-enqueue or audio restart',flush=True)
finally:host.close()
