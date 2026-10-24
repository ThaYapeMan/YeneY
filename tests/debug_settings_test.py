"""Debug list and deprecated alias parity; no sockets or playback."""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
source=r'''
#include "debug_settings.h"
#include <cstdio>
int main() {
 bridge::warnDebugSettings(); bridge::warnDebugSettings();
 printf("%d%d%d%d\n",bridge::debugOption("events","YENEY_DEBUG_EVENTS"),
 bridge::debugOption("stream","YENEY_DEBUG_STREAM"),
 bridge::debugOption("legacy-poll","YENEY_POLL","legacy"),
 bridge::debugOption("stopped-mediainfo","YENEY_STOPPED_MEDIAINFO"));
}
'''
with tempfile.TemporaryDirectory(prefix='yeney-debug-settings-') as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(source)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-I.',str(p/'test.cpp'),'-o',str(p/'test')],cwd=ROOT,check=True)
 keys=('YENEY_DEBUG','YENEY_DEBUG_EVENTS','YENEY_DEBUG_STREAM','YENEY_POLL','YENEY_STOPPED_MEDIAINFO')
 env={k:v for k,v in os.environ.items() if k not in keys}
 for settings,expected,warnings in [({},'0000',0),({'YENEY_DEBUG':'events,stream,legacy-poll,stopped-mediainfo'},'1111',0),
 ({'YENEY_DEBUG_EVENTS':'1','YENEY_DEBUG_STREAM':'1','YENEY_POLL':'legacy','YENEY_STOPPED_MEDIAINFO':'1'},'1111',4),
 ({'YENEY_DEBUG':'stream','YENEY_DEBUG_EVENTS':'true','YENEY_POLL':'events'},'0100',2),
 ({'YENEY_DEBUG':'unknown'},'0000',1)]:
  result=subprocess.run([str(p/'test')],env=env|settings,text=True,capture_output=True,check=True)
  assert result.stdout.strip()==expected,(settings,result)
  assert len(result.stderr.splitlines())==warnings,(settings,result)
print('PASS debug settings: canonical list, legacy parity, exact enabled values, one startup warning per alias; no default log changes')
