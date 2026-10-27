"""Name precedence and immutable identity, including direct binary/tool fixtures."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import itertools
import json
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
from queue_binary_test import Binary, ROOT

with tempfile.TemporaryDirectory(prefix='player-name-') as directory:
    base=Path(directory);config=base/'config';test=base/'name-test'
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-I.', 'tests/player_name_test.cpp','-o',str(test)],cwd=ROOT,check=True)
    for configured,environment,option in itertools.product((None,'yes','no','invalid'),repeat=3):
        config.write_text('room.Other=yes\n'+('  YENEY_SHOW_SONOS_SUFFIX = '+configured+'\n' if configured is not None else ''))
        env=dict(os.environ);env.pop('YENEY_SHOW_SONOS_SUFFIX',None)
        if environment is not None:env['YENEY_SHOW_SONOS_SUFFIX']=environment
        selected=next((v for v in (option,environment,configured) if v is not None),'yes')
        source='command-line' if option is not None else 'environment' if environment is not None else 'config' if configured is not None else 'default'
        display='Study' if selected=='no' else 'Study (Sonos)'
        output=subprocess.check_output([str(test),str(config)]+([option] if option is not None else []),env=env,text=True)
        assert f'true=Study (Sonos) display={display} source={source}' in output,output
        assert output.count('invalid')==int(selected=='invalid'),output
    print('PASS player name: all 64 source/precedence/invalid combinations; one warning and default yes for invalid selected value')

for suffix in ('yes','no','invalid'):
    os.environ['YENEY_SHOW_SONOS_SUFFIX']=suffix
    h=Binary(rates=(),mode='stream')
    try:
        path=h.base/('stream-pacing-'+b'Study'.hex()+'.json')
        h.until(path.exists)
        state=json.loads(path.read_text())
        display='Study' if suffix=='no' else 'Study (Sonos)'
        assert state['true_name']=='Study (Sonos)' and state['display_name']==display and state['mac']==h.mac,state
        text=(h.base/'bridge.log').read_text()
        assert text.count('yeney: player name true=Study (Sonos) display='+display+' source=environment')==1,text
        assert text.count('invalid YENEY_SHOW_SONOS_SUFFIX')==int(suffix=='invalid'),text
        identity=runpy.run_path(str(ROOT/'scripts/yeney_room_state.py'))['player_identity']
        assert identity('Study',h.base)==(h.mac,'Study (Sonos)')
        # Execute the exact shell identity resolver without starting its owner-only runner.
        script=(ROOT/'scripts/device-test.sh').read_text()
        resolver=script[script.index('resolve_player() {'):script.index('\n# --------------------------------------------------------------- markers')]
        result=subprocess.check_output(['bash','-c',resolver+'\nresolve_player'],text=True,env=dict(os.environ,ROOM='Study',YENEY_STATE_DIRECTORY=str(h.base)))
        assert result.strip()==h.mac,result
        class CLI:
            def call(self,*args):
                return ['playerindex:0','playerid:'+h.mac,'name:'+display,'connected:1',
                        'playerindex:1','playerid:12:34:56:78:90:ab','name:Study (Sonos)','connected:1']
        for filename in ('yeney-stream-pacing-suite','yeney-queue-device-suite'):
            Suite=runpy.run_path(str(ROOT/'scripts'/filename))['Suite']
            suite=Suite.__new__(Suite);suite.room='Study';suite.mac=None;suite.state_root=h.base;suite.cli=CLI()
            assert suite.player()==h.mac and suite.true_name=='Study (Sonos)'
        installer=runpy.run_path(str(ROOT/'scripts/install_devices.py'))
        class Socket:
            def __enter__(self):return self
            def __exit__(self,*args):pass
            def settimeout(self,*args):pass
            def sendall(self,*args):pass
            def recv(self,*args):
                from urllib.parse import quote
                return (' '.join(quote(v,safe=':') for v in CLI().call())+'\n').encode()
        result,skipped=installer['check_lms']('127.0.0.1',['Study'],connect=lambda *a,**k:Socket(),identity=lambda room:identity(room,h.base))
        assert not skipped and result=={'Study':'LMS player "Study (Sonos)" connected'},result
        print('PASS installer, queue device suite, pacing suite: MAC selection suffix='+suffix+'; same-name impostor ignored')
        print('PASS direct binary suffix='+suffix+': immutable true name/MAC in logs/status; shared tool identity and device-test.sh resolver')
    finally:h.close()
os.environ.pop('YENEY_SHOW_SONOS_SUFFIX',None)

with tempfile.TemporaryDirectory(prefix='name-direct-config-') as directory:
    config=Path(directory)/'config';os.environ['YENEY_CONFIG']=str(config)
    for configured,environment,option,display,source in (
            (None,None,None,'Study (Sonos)','default'),
            ('no',None,None,'Study','config'),
            ('yes','no',None,'Study','environment'),
            ('no','no','yes','Study (Sonos)','command-line'),
            ('yes','yes','no','Study','command-line'),
            ('no','no','invalid','Study (Sonos)','command-line')):
        config.write_text('' if configured is None else 'YENEY_SHOW_SONOS_SUFFIX='+configured+'\n')
        os.environ.pop('YENEY_SHOW_SONOS_SUFFIX',None)
        if environment is not None:os.environ['YENEY_SHOW_SONOS_SUFFIX']=environment
        h=Binary(rates=(),mode='stream',player_options=() if option is None else ('--show-sonos-suffix='+option,))
        try:
            path=h.base/('stream-pacing-'+b'Study'.hex()+'.json');h.until(path.exists)
            state=json.loads(path.read_text());text=(h.base/'bridge.log').read_text()
            assert state['true_name']=='Study (Sonos)' and state['display_name']==display,state
            assert 'display='+display+' source='+source in text,text
        finally:h.close()
    os.environ.pop('YENEY_CONFIG',None);os.environ.pop('YENEY_SHOW_SONOS_SUFFIX',None)
    print('PASS direct binary without systemd: default, global config, environment, both command-line overrides and invalid override')
