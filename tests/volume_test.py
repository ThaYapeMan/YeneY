"""Volume acceptance with real binary, loopback LMS CLI and Sonos SOAP/GENA."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import os
import json
import subprocess
import time
import tempfile
from pathlib import Path
import http.client
from urllib.parse import urlsplit
from xml.sax.saxutils import escape
from queue_binary_test import Binary, ROOT

def notify(h,group=False):
    key='group_volume_callback' if group else 'volume_callback'
    h.until(lambda:bool(getattr(h.sonos,key,'')))
    u=urlsplit(getattr(h.sonos,key));c=http.client.HTTPConnection(u.netloc,timeout=3)
    value=h.sonos.group_volume if group else h.sonos.volume
    inner=f'<Event><InstanceID val="0"><Volume channel="Master" val="{value}"/></InstanceID></Event>'
    body='<e:propertyset xmlns:e="urn:schemas-upnp-org:event-1-0"><e:property>'+('<GroupVolume>'+str(value)+'</GroupVolume>' if group else '<LastChange>'+escape(inner)+'</LastChange>')+'</e:property></e:propertyset>'
    c.request('NOTIFY',u.path,body,headers={'NT':'upnp:event','NTS':'upnp:propchange','SID':'uuid:binary','SEQ':'1'})
    r=c.getresponse();r.read();assert r.status==200;c.close()

def writes(h):return getattr(h.sonos,'volume_writes',[])
def run(mode,group=False,maximum=100,member=False,playback="queue"):
    os.environ['YENEY_LMS_ICON']='';os.environ['YENEY_LMS_VOLUME']=mode;os.environ['YENEY_VOLUME_MAX']=str(maximum)
    h=Binary(rates=() if member else (44100,),seconds=2,grouped=group,member=member,mode=playback)
    try:
        h.until(lambda:bool(list(h.base.glob('stream-pacing-*.json'))))
        status=json.loads(next(h.base.glob('stream-pacing-*.json')).read_text())['playback']
        assert status['lms_volume']==mode and status['lms_volume_source']=='environment' and status['volume_max']==maximum,status
        if mode=='off':
            time.sleep(.6)
            assert not writes(h) and not any(c.startswith('mixer ') for c in h.commands)
            print('PASS off mode direct binary: no RenderingControl writes and no LMS mixer commands')
            return
        action='SetGroupVolume' if group else 'SetVolume'
        field='group_volume' if group else 'volume'
        muted='group_muted' if group else 'muted'
        h.until(lambda:any(w[1]==action for w in writes(h)))
        for value in (0,25,50,100):
            h.lms_volume=value
            h.until(lambda:getattr(h.sonos,field)==min(value,maximum))
        h.lms_mute=1
        h.until(lambda:getattr(h.sonos,muted,0)==1)
        n=len(writes(h));setattr(h.sonos,field,41);notify(h,group)
        h.until(lambda:getattr(h,'lms_volume',None)==41 and getattr(h,'lms_mute',None)==1)
        time.sleep(.5);assert len(writes(h))==n
        assert h.commands.count('mixer volume 41')==1 and h.commands.count('mixer muting 1')==1
        if maximum<100:assert (h.base/'bridge.log').read_text().count('LMS value capped maximum=')==1
        n=len(writes(h));time.sleep(.4);assert len(writes(h))==n
        for value in (11,22,33):h.lms_volume=value;time.sleep(.05)
        h.until(lambda:getattr(h.sonos,field)==33)
        assert [w[2] for w in writes(h)[n:] if w[1]==action]==[33]
        assert all(b[0]-a[0]>=.19 for a,b in zip(writes(h),writes(h)[1:]))
        assert not group or all('Group' in w[1] for w in writes(h))
        assert not group or sum(w[1]=='SnapshotGroupVolume' for w in writes(h))==sum(w[1]=='SetGroupVolume' for w in writes(h))
        n=len(writes(h));setattr(h.sonos,field,42);setattr(h.sonos,muted,0);notify(h,group)
        h.until(lambda:getattr(h,'lms_volume',None)==42 and getattr(h,'lms_mute',None)==0)
        time.sleep(.5)
        assert h.commands.count('mixer volume 42')==1 and h.commands.count('mixer muting 0')==1
        assert len(writes(h))==n
        reads=h.commands.count('mixer volume ?');h.lms_volume=57
        h.until(lambda:h.commands.count('mixer volume ?')>reads)
        setattr(h.sonos,field,43);notify(h,group)
        h.until(lambda:h.lms_volume==43)
        time.sleep(.4);assert len(writes(h))==n
        print(f'PASS speaker mode real binary group={group} member={member} playback={playback} maximum={maximum}: exact 0/25/50/100, mute, debounce, cap, unchanged suppression, GENA app changes reach LMS once, app supersedes pending LMS change, no echo')
    finally:h.close()

def settings():
    import runpy
    installer=runpy.run_path(str(ROOT/'scripts/install_devices.py'))
    text='room.Study=yes\nroom.Study.lms_volume=speaker\nroom.Study.volume_max=61\n'
    warnings=[]
    assert installer['parse_rooms'](text,warnings.append)=={'Study':True} and not warnings
    merged,_=installer['merge_config'](text,['Study'],['Study'])
    assert merged==text
    print('PASS installer preserves per-room volume settings without treating them as room enable keys')
    with tempfile.TemporaryDirectory(prefix='volume-settings-') as directory:
        path=Path(directory)/'config';path.write_text('room.Other.lms_volume=off\nroom.Study.lms_volume=speaker\nroom.Study.volume_max=61\n')
        env=dict(os.environ);env.pop('YENEY_LMS_VOLUME',None);env.pop('YENEY_VOLUME_MAX',None)
        def check(expected,option=None):
            args=[str(ROOT/'volume-test'),str(path)]+([option] if option is not None else [])
            text=subprocess.check_output(args,env=env,text=True)
            assert expected in text,text
            return text
        check('LMS volume=speaker source=config maximum=61')
        env['YENEY_LMS_VOLUME']='off';check('LMS volume=off source=environment')
        check('LMS volume=speaker source=command-line','speaker')
        text=check('LMS volume=off source=command-line','invalid')
        assert text.count('invalid YENEY_LMS_VOLUME')==1
        env['YENEY_LMS_VOLUME']='';check('LMS volume=off source=environment')
        env['YENEY_VOLUME_MAX']='101';assert 'invalid YENEY_VOLUME_MAX' in check('maximum=100')
        env.pop('YENEY_LMS_VOLUME');env.pop('YENEY_VOLUME_MAX');path.unlink()
        check('LMS volume=off source=default maximum=100')
        print('PASS volume settings: command-line > environment > per-room global config > default; invalid values warn once; maximum validation; no systemd')

if __name__=='__main__':
    settings()
    run('off');run('speaker');run('speaker',True);run('speaker',maximum=60);run('speaker',member=True);run('speaker',playback='stream')
