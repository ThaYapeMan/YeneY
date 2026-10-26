# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Failure and explicit opt-out paths must be observable without real devices."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


class ConsumerPolicy(unittest.TestCase):
    def consumer(self, checkout, off=False):
        env=dict(os.environ,LAMPASTREAM_CHECKOUT=str(checkout))
        env.pop('LAMPASTREAM_CONSUMER',None)
        if off:env['LAMPASTREAM_CONSUMER']='off'
        return subprocess.run([sys.executable,str(ROOT/'third_party/yeney-core/tests/shm_test.py'),
                               'ShmTests.test_03_real_lampastream_consumer'],env=env,capture_output=True,text=True)

    def test_required_and_explicit_off(self):
        with tempfile.TemporaryDirectory(dir=(os.environ.get('TMPDIR') or '/var/tmp')) as directory:
            root=Path(directory)
            missing=self.consumer(root/'missing')
            self.assertNotEqual(missing.returncode,0)
            self.assertIn('LampaStream checkout missing',missing.stdout+missing.stderr)
            failed_import=self.consumer(root)
            self.assertNotEqual(failed_import.returncode,0)
            self.assertIn('LampaStream consumer import failed',failed_import.stdout+failed_import.stderr)
            off=self.consumer(root/'missing',off=True)
            self.assertEqual(off.returncode,0)
            self.assertEqual(off.stdout.count('CROSS-REPO CONSUMER NOT RUN (LAMPASTREAM_CONSUMER=off)'),1)
        print('PASS consumer policy: missing checkout and failed import fail; explicit off appears once in final summary')

    def test_default_and_override(self):
        env=dict(os.environ);env.pop('LAMPASTREAM_CHECKOUT',None)
        cmd=['make','-s','--eval','consumer-path: ; @echo $(LAMPASTREAM_CHECKOUT)','consumer-path']
        default=subprocess.check_output(cmd,cwd=ROOT,env=env,text=True).strip()
        self.assertEqual(default,str(ROOT.parent/'LampaStream'))
        explicit=subprocess.check_output(cmd,cwd=ROOT,env=dict(env,LAMPASTREAM_CHECKOUT='/explicit/checkout'),text=True).strip()
        self.assertEqual(explicit,'/explicit/checkout')
        print('PASS consumer checkout: bridge default resolves to ../LampaStream; explicit LAMPASTREAM_CHECKOUT wins')


if __name__=='__main__':unittest.main()
