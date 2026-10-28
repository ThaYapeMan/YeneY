"""Qualify the owner-only finite-podcast transaction with a simulated clock."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import io
import struct
from types import SimpleNamespace
from unittest.mock import patch
from queue_device_suite_test import m

suite=object.__new__(m.Suite)
clock=[0.];transport=['PLAYING'];position=[0];calls=[];paused=[];seeked=[False]
suite.backend=SimpleNamespace(now=lambda:clock[0])
suite.remote_argument=None;suite.remote_pool=[];suite.podcast_argument='podcast://https://fixture.invalid/episode.mp3'
track=dict(id='',url=suite.podcast_argument,title='Episode')
suite.remote_choice=lambda menus:(track,'configured podcast')
suite.begin=lambda name:setattr(suite,'current',dict(name=name))
suite.problem=lambda reason:(_ for _ in ()).throw(AssertionError(reason))
def row():
    return dict(monotonic=clock[0],playback=dict(reason='remote-finite'),speaker=dict(uri='http://127.0.0.1/item.wav',duration=65,
                state=transport[0],rel=position[0]))
suite.load=lambda tracks:row()
def sample(tracks,**kwargs):
    clock[0]+=1
    if transport[0]=='PAUSED_PLAYBACK':paused.append(clock[0])
    elif seeked[0]:transport[0]='STOPPED'
    return row()
suite.sample=sample
suite.query=lambda field:str(position[0]) if field=='time' else 'stop' if transport[0]=='STOPPED' else 'play'
def call(service,action,**kwargs):
    calls.append((action,kwargs))
    if action=='Pause':transport[0]='PAUSED_PLAYBACK'
    elif action=='Play':transport[0]='PLAYING'
    elif action=='Seek':
        h,minute,sec=map(int,kwargs['Target'].split(':'));position[0]=h*3600+minute*60+sec;seeked[0]=True
suite.speaker=SimpleNamespace(call=call)
header=bytearray(44);header[:4]=b'RIFF';struct.pack_into('<I',header,24,44100);struct.pack_into('<I',header,40,65*44100*6)
class Response(io.BytesIO):
    headers={'Content-Range':f'bytes 0-43/{44+65*44100*6}'}
with patch('urllib.request.urlopen',return_value=Response(header)):
    suite.finite_scenario()
assert len(paused)>=120,paused
assert [a for a,_ in calls]==['Pause','Play','Seek'],calls
assert calls[-1][1]['Unit']=='REL_TIME' and calls[-1][1]['Target']=='00:00:32'
assert 'LMS time=32' in suite.current['note'] and 'clean end observed' in suite.current['note']
assert suite.remote_argument is None and suite.remote_pool==[]
print('PASS owner remote-finite scenario: fixed length matches TrackDuration, 120 s simulated pause, resume position, middle REL_TIME seek, LMS position recorded, real end observed; no device contact')
