from pathlib import Path
import os
import subprocess
import tempfile
tempfile.tempdir = (os.environ.get('TMPDIR') or '/var/tmp')
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='yeney-obsolete-player-') as tmp:
    source=Path(tmp)/'test.cpp'; exe=Path(tmp)/'test'
    source.write_text('#include "obsolete_player.h"\nint main(){warnObsoletePlayer();warnObsoletePlayer();}\n')
    subprocess.run(['g++','-I',str(root),str(source),'-o',str(exe)],check=True)
    for value in (None,'core','squeeze'+'lite','invalid',''):
        env=dict(os.environ);env.pop('YENEY_PLAYER',None)
        if value is not None:env['YENEY_PLAYER']=value
        result=subprocess.run([str(exe)],env=env,capture_output=True,text=True,check=True)
        expected='' if value is None or value=='core' else 'squeeze'+'lite has been removed; using yeney-core\n'
        assert result.stdout==expected,result
    print('PASS: obsolete player values never reject startup; unset/core silent, every other value warns exactly once')

# Active compatibility diagnostics: old settings warn but never configure playback.
root = Path(__file__).resolve().parents[1]
names = {
    'SONOS_LMS_UPNP': 'YENEY_UPNP',
    'SONOS_LMS_PAUSE': 'YENEY_PAUSE',
    'SONOS_SQUEEZEBOX_PAUSE': 'YENEY_PAUSE',
    'SONOS_LMS_TITLE_FORMAT': 'YENEY_TITLE_FORMAT',
    'SONOS_LMS_STREAM_CONTENT': 'YENEY_STREAM_CONTENT',
    'SONOS_LMS_AUDIO': 'YENEY_AUDIO',
    'SONOS_LMS_YENEY_POLL': 'YENEY_POLL',
    'SONOS_LMS_YENEY_STOPPED_MEDIAINFO': 'YENEY_STOPPED_MEDIAINFO',
    'SONOS_LMS_EVENT_PORT': None,
    'SONOS_LMS_UNKNOWN': None,
    'SONOS_SQUEEZEBOX_UNKNOWN': None,
}
env = {k: v for k, v in os.environ.items() if not k.startswith(('SONOS_LMS_', 'SONOS_SQUEEZEBOX_', 'YENEY_'))}
env.update({name: 'pause' for name in names})
env['SONOS_LMS_UNKNOWN'] = ''  # Set but empty still warns.
env['UNRELATED'] = 'pause'
expected = sorted('yeney: ' + name + (' is no longer read; rename it to ' + new if new else ' is obsolete')
                  for name, new in names.items())
# The real startup path warns before parsing command options, without needing a device.
result = subprocess.run([str(root / 'yeney'), '--file'], env=env, capture_output=True, text=True)
assert result.returncode != 0
assert sorted(result.stderr.splitlines()[:-1]) == expected, result.stderr
assert result.stderr.splitlines()[-1] == '--file is no longer supported'
with tempfile.TemporaryDirectory(prefix='yeney-old-settings-') as tmp:
    source, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
    source.write_text('''
#include "pause_mode.h"
#include "upnp/title_format.h"
#include <cassert>
int main(int argc, char**) {
    assert((pauseMode() == PauseMode::Pause) == (argc > 1));
    assert(upnp::titleFormat() == upnp::TitleFormat::ArtistTitle);
}
''')
    subprocess.run(['g++', '-I', str(root), str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], env=env, check=True)
    env['YENEY_PAUSE'] = 'pause'
    subprocess.run([str(exe), 'new'], env=env, check=True)
print('PASS: old-variable warning table covers successors, obsolete/unknown/empty values; old pause fallback is ignored and new settings win')
