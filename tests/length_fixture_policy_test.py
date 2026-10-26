"""Private length fixture selection and failure policy, without reading owner media."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Policy(unittest.TestCase):
    def test_selection(self):
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp)
            env = dict(os.environ, HOME=tmp)
            env.pop('YENEY_LENGTH_FIXTURE_DIR', None)
            def selected(extra=None):
                return subprocess.check_output(['make', '-s', '--eval',
                    'fixture-path: ; @echo "$(YENEY_LENGTH_FIXTURE_DIR)"', 'fixture-path'],
                    cwd=ROOT, env=env if extra is None else dict(env, **extra), text=True).strip()
            second = home/'private-fixtures'; second.mkdir()
            self.assertEqual(selected(), str(second))
            first = home/'src/private-fixtures'; first.mkdir(parents=True)
            self.assertEqual(selected(), str(first))
            self.assertEqual(selected({'YENEY_LENGTH_FIXTURE_DIR': ''}), '')
            self.assertEqual(selected({'YENEY_LENGTH_FIXTURE_DIR': '/explicit'}), '/explicit')
        print('PASS owner-file default selection: first existing directory, caller override and empty opt-out')

    def test_missing_empty_and_skip(self):
        with tempfile.TemporaryDirectory() as tmp:
            def check(value):
                return subprocess.run([sys.executable, 'tests/length_test.py', 'Lengths.test_private'],
                    cwd=ROOT/'third_party/yeney-core', env=dict(os.environ, YENEY_LENGTH_FIXTURE_DIR=value),
                    capture_output=True, text=True)
            for path in (tmp, str(Path(tmp)/'missing')):
                result = check(path)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('YENEY_LENGTH_FIXTURE_DIR', result.stderr)
            result = check('')
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stdout.count('SKIP owner-file length check:'), 1)
        print('PASS owner-file policy: configured missing/empty directory fails; disabled/no-default prints one reasoned SKIP')

if __name__ == '__main__':
    unittest.main()
