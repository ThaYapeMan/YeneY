"""Safe state paths and upgrade tests; no live services or endpoints."""
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from installer_test import installer, Commands, mock_lms, ROOT

NAMES = ['Study', 'MBR', 'Sonos Port', 'Living room (2)', 'Küche', 'Owner\'s room']

class StateTests(unittest.TestCase):
    def test_safe_stable_unique_and_offline_verify(self):
        with tempfile.TemporaryDirectory(prefix='yeney-state-') as tmp:
            units = Path(tmp)
            states = installer.service_states(NAMES, Commands())
            drops = installer.state_dropins(units, states)
            names = [installer.state_name(n) for n in NAMES + ['Kuche', 'Küche!']]
            self.assertEqual(len(names), len(set(names)))
            self.assertEqual(names, [installer.state_name(n) for n in NAMES + ['Kuche', 'Küche!']])
            for value in names: self.assertRegex(value, r'^[a-z0-9-]+$')
            template = (ROOT/'packaging/yeney@.service').read_text()
            self.assertIn('ExecStart=/opt/yeney/yeney --room=%I', template)
            # Offline verification uses a present harmless executable; the shipped command is asserted above.
            (units/'yeney@.service').write_text(template.replace('/opt/yeney/yeney', '/bin/true'))
            paths = []
            for room, (path, text) in drops.items():
                path.parent.mkdir()
                path.write_text(text)
                unit = units/states[room][0]
                unit.symlink_to('yeney@.service')
                paths.append(str(unit))
                value = text.split('StateDirectory=')[-1].strip()
                self.assertRegex(value, r'^yeney/[a-z0-9-]+$')
            env = dict(os.environ, SYSTEMD_UNIT_PATH=str(units)+':/usr/lib/systemd/system')
            result = subprocess.run(['systemd-analyze', 'verify', '--man=no', *paths], env=env,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        print('PASS installer safe stable unique state names and offline systemd verification')

if __name__ == '__main__':
    unittest.main(verbosity=2)
