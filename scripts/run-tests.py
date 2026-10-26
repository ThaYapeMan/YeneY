#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Run checks in a disk workspace, retaining only five transcript directories."""
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

cache = Path(os.environ.get('YENEY_TEST_CACHE', Path.home()/'.cache/yeney-test')).expanduser().resolve()
logs = cache/'runs'
logs.mkdir(parents=True, exist_ok=True)
run = logs/(time.strftime('%Y%m%d-%H%M%S')+'-'+str(os.getpid()))
run.mkdir()
parent = Path((os.environ.get('TMPDIR') or '/var/tmp'))
parent.mkdir(parents=True, exist_ok=True)
code = 1
child = None

def interrupted(number, frame):
    raise KeyboardInterrupt

for sig in (signal.SIGINT,signal.SIGTERM,signal.SIGHUP):
    signal.signal(sig,interrupted)
with tempfile.TemporaryDirectory(prefix='yeney-tests-',dir=parent) as work:
    try:
        env = dict(os.environ, TMPDIR=work, YENEY_TEST_CACHE=str(cache))
        with (run/'console.log').open('w') as log:
            child = subprocess.Popen(sys.argv[1:],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=True)
            for line in iter(child.stdout.readline,b''):
                sys.stdout.buffer.write(line);sys.stdout.buffer.flush()
                log.write(line.decode('utf-8','replace'));log.flush()
            code = child.wait()
    except KeyboardInterrupt:
        code = 130
    finally:
        for sig in (signal.SIGINT,signal.SIGTERM,signal.SIGHUP): signal.signal(sig,signal.SIG_IGN)
        if child and child.poll() is None:
            os.killpg(child.pid,signal.SIGTERM)
            try: child.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid,signal.SIGKILL);child.wait()
        for path in Path(work).rglob('*'):
            if path.is_file() and not path.is_symlink() and path.suffix in ('.log','.txt','.json','.jsonl','.md','.tsv','.csv','.xml'):
                target=run/'evidence'/path.relative_to(work)
                target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
        (run/'result.txt').write_text('exit status '+str(code)+'\n')
        for old in sorted(logs.iterdir(), key=lambda p:p.stat().st_mtime,reverse=True)[5:]:
            if old.is_dir(): shutil.rmtree(old)
print('Test logs: '+str(run),flush=True)
raise SystemExit(code)
