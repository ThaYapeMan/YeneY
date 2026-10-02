from pathlib import Path
import os
import subprocess
import tempfile
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
