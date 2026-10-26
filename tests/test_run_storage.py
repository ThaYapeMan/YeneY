# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Workspace ownership, interruption and bounded transcript retention."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]


class StorageTests(unittest.TestCase):
    def test_cleanup_failure_interruption_and_retention(self):
        with tempfile.TemporaryDirectory(dir=(os.environ.get('TMPDIR') or '/var/tmp')) as directory:
            base=Path(directory);tmp=base/'tmp';tmp.mkdir()
            env=dict(os.environ,TMPDIR=str(tmp),YENEY_TEST_CACHE=str(base/'cache'))
            command=[sys.executable,str(ROOT/'scripts/run-tests.py'),sys.executable,'-c']
            for i in range(6):
                code="import os,pathlib; p=pathlib.Path(os.environ['TMPDIR']); (p/'audio.wav').write_bytes(b'large audio'); (p/'diagnostic.log').write_text('keep'); raise SystemExit(3)"
                result=subprocess.run(command+[code],env=env,capture_output=True,text=True)
                self.assertEqual(result.returncode,3,result.stdout+result.stderr)
                self.assertFalse(list(tmp.iterdir()))
            runs=list((base/'cache/runs').iterdir())
            self.assertEqual(len(runs),5)
            self.assertTrue(all((p/'evidence/diagnostic.log').read_text()=='keep' for p in runs))
            self.assertFalse(list((base/'cache').rglob('*.wav')))
            ready=base/'ready'
            code="import os,pathlib,time; pathlib.Path("+repr(str(ready))+").write_text(os.environ['TMPDIR']); time.sleep(60)"
            process=subprocess.Popen(command+[code],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
            try:
                deadline=time.monotonic()+10
                while not ready.exists() and time.monotonic()<deadline:time.sleep(.02)
                self.assertTrue(ready.exists())
                process.send_signal(signal.SIGTERM)
                output=process.communicate(timeout=15)[0]
                self.assertEqual(process.returncode,130,output)
                self.assertFalse(list(tmp.iterdir()))
                self.assertEqual(len(list((base/'cache/runs').iterdir())),5)
            finally:
                if process.poll() is None:process.kill();process.wait()


if __name__=='__main__':unittest.main()
