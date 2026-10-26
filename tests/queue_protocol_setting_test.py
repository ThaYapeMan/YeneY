"""Queue-only remote admission setting; no running service or network."""
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import os
from pathlib import Path
import subprocess
import tempfile
tempfile.tempdir = (os.environ.get('TMPDIR') or '/var/tmp')
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='yeney-next-delay-') as directory:
    source=Path(directory)/'main.cpp'; binary=source.with_suffix('')
    source.write_text('#include "queue_protocol.h"\nint main(){return bridge::queueNextDelayMs()/1000;}\n')
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-I',str(ROOT),str(source),'-o',str(binary)],check=True)
    for value,expected in ((None,20),('1',1),('120',120),('0',20),('-1',20),('abc',20),('999999999999999999999999',20)):
        env=dict(os.environ);env.pop('YENEY_QUEUE_NEXT_DELAY_S',None)
        if value is not None:env['YENEY_QUEUE_NEXT_DELAY_S']=value
        result=subprocess.run([str(binary)],env=env,text=True,capture_output=True)
        assert result.returncode==expected,(value,result)
        assert ('Invalid' in result.stderr)==(value is not None and value not in ('1','120')),result
print('PASS queue next delay: default 20 s, 1–120 s bounds, invalid values warn/default; startup log')
