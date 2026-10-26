# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Standalone real-LMS runs also clean up setup failures and keep five logs."""
import atexit
import os
from pathlib import Path
import shutil
import signal
import tempfile
import time


def workspace(name):
    root=Path(tempfile.mkdtemp(prefix='yeney-'+name+'-',dir=(os.environ.get('TMPDIR') or '/var/tmp')))
    def finish():
        cache=Path(os.environ.get('YENEY_TEST_CACHE',Path.home()/'.cache/yeney-test'))/'runs'
        cache.mkdir(parents=True,exist_ok=True)
        dest=cache/(name+'-'+time.strftime('%Y%m%d-%H%M%S')+'-'+str(os.getpid()))
        dest.mkdir(exist_ok=True)
        try:
            for path in root.rglob('*'):
                if path.is_file() and path.suffix in ('.log','.json','.txt','.md','.tsv'):
                    target=dest/path.relative_to(root);target.parent.mkdir(parents=True,exist_ok=True)
                    shutil.copyfile(path,target)
        finally:
            shutil.rmtree(root)
        for old in sorted(cache.iterdir(),key=lambda p:p.stat().st_mtime,reverse=True)[5:]:
            if old.is_dir():shutil.rmtree(old)
    atexit.register(finish)
    def interrupted(number,frame):raise SystemExit(128+number)
    for sig in (signal.SIGTERM,signal.SIGHUP):signal.signal(sig,interrupted)
    return root
