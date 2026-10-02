import os
import subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
for value, expected in [(None,1000),('0',0),('1000',1000),('2000',2000),('10000',10000),('',1000),('-1',1000),('10001',1000),('999999999999999999999999999',1000),(' 2000',1000),('true',1000)]:
    env=dict(os.environ); env.pop('YENEY_START_LEAD_MS',None)
    if value is not None: env['YENEY_START_LEAD_MS']=value
    completed=subprocess.run([str(root/'lead-test'),str(expected)],env=env,capture_output=True,text=True,check=True)
    assert ('invalid' in completed.stderr) == (value is not None and value not in ('0','1000','2000','10000'))
    print(f'PASS: YENEY_START_LEAD_MS={value!r} -> {expected}',flush=True)
