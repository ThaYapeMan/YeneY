"""Real bridge overflow/late-overflow qualification on loopback only."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import http.client
import socket
import json
import time
from urllib.parse import urlsplit
from queue_binary_test import Binary

def fetch(uri,stall=False):
    url=urlsplit(uri);conn=http.client.HTTPConnection(url.netloc,timeout=20)
    try:
        if stall:
            conn.connect();conn.sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,256*1024)
        conn.request('GET',url.path+'?'+url.query);response=conn.getresponse()
        first=response.read(16384) if stall else b''
        if stall:
            time.sleep(2)
            response.fp.raw._sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4*1024*1024)
        try:return first+response.read()
        except http.client.IncompleteRead as error:return first+error.partial
    finally:conn.close()

for late in (False,True):
    host=Binary(rates=(44100,44100),seconds=55,remote_sources=True,remote_duration=20,
                partial_source=late,queue_title_prefix='')
    try:
        host.until(lambda:len(host.items())>=1)
        host.state(1,20 if late else 0)
        host.until(lambda:'queue item-end occurrence=1' in (host.base/'bridge.log').read_text(),timeout=25)
        entries=host.items()
        following=len(entries)
        assert following>=2
        assert [e['metadata']['title'] for e in entries[:following]]==['Title & 0']*following
        bodies=[fetch(e['uri'],stall=not late and index==0) for index,e in enumerate(entries[:following])]
        actual=b''.join(b[44:] for b in bodies)
        expected=(b'\0\xe8\x03\0\x18\xfc')*(44100*55)
        assert actual==expected,(len(actual),len(expected))
        text=(host.base/'bridge.log').read_text()
        assert 'path=relay reason=remote-finite' in text
        if not late:assert 'HTTP backpressure item=' in text,'receiver stall did not exercise backpressure'
        if late:assert 'WARNING continuation inserted' in text,text[-10000:]
        else:assert 'prefetch_margin_s=15.' in text,text[-10000:]
        for i in range(1,following+1):
            host.state(i,0)
            host.until(lambda:json.loads((host.base/'stream-pacing-5374756479.json').read_text())['playback']['current_owned_item']==entries[i-1]['uri'])
        assert not host.packets('STMd'),'estimated episode reported decoded before audible end'
        assert len(host.packets('STMs'))==1,host.packets('STMs')
        tail_frames=(len(bodies[-1])-44)//6
        host.state(following,tail_frames//44100,state='STOPPED')
        host.until(lambda:any(e['metadata']['title']=='Title & 1' for e in host.items()),timeout=15)
        assert not [c for c in host.commands if not c.startswith('status ') and not c.endswith('?')]
        print('PASS remote-finite real binary '+('late overflow: warning and retained audio' if late else 'early overflow: inserted >10 s before end')+
              f'; {following} ordered segments before successor; sample-exact 55 s; one LMS STMs, no extra LMS mutation',flush=True)
    finally:host.close()

# A completed episode may be paused while its continuation is selected. The
# exhausted LMS producer's q+s resume echo must retain the same Sonos asset.
host=Binary(rates=(44100,),seconds=55,remote_sources=True,remote_duration=20,
            resume_restart=True,queue_title_prefix='')
try:
    host.pause_flush=True
    host.until(lambda:len(host.items())>=2)
    host.state(2,2)
    time.sleep(3.2)
    additions=len(host.sonos.actions('AddURIToQueue'))
    host.state(2,2,state='PAUSED_PLAYBACK')
    host.until(lambda:'pause flush retained:' in (host.base/'bridge.log').read_text())
    assert len(host.items())>=2
    host.state(2,2,state='PLAYING')
    host.until(lambda:'resume echo adopted:' in (host.base/'bridge.log').read_text())
    host.until(lambda:len(host.packets('STMs'))>=2)
    assert len(host.sonos.actions('AddURIToQueue'))==additions
    mutations=[c for c in host.commands if not c.startswith('status ') and not c.endswith('?')]
    assert mutations==['pause 1','pause 0'],mutations
    assert host.items()[1]['metadata']['title']=='Title & 0'
    print('PASS continuation cached resume: parent LMS identity, pause/resume only, same immutable queue assets, no re-enqueue or audio restart',flush=True)
finally:host.close()

# A transition mid-item is not completion; a declared-boundary stop with a
# retained overflow is a fault, even when the producer has already finished.
host=Binary(rates=(44100,),seconds=55,remote_sources=True,remote_duration=20,queue_title_prefix='')
try:
    host.until(lambda:len(host.items())>=2)
    host.state(1,4)
    time.sleep(3.2)
    ends=len(host.packets('STMu'))
    host.state(1,4,state='TRANSITIONING')
    time.sleep(1)
    assert len(host.packets('STMu'))==ends
    host.state(1,25,state='STOPPED')
    host.until(lambda:'end decision=fault-unplayed-audio' in (host.base/'bridge.log').read_text())
    assert len(host.packets('STMu'))==ends
    print('PASS finite remote stop classification: mid-item TRANSITIONING never ends; declared boundary with unplayed continuation is a fault, no STMu',flush=True)
finally:host.close()
