"""Safe state paths and upgrade tests; no live services or endpoints."""
import io
import hashlib
from unittest.mock import patch
import os
from pathlib import Path
import subprocess
import tempfile
tempfile.tempdir = (os.environ.get('TMPDIR') or '/var/tmp')
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

    def test_migrate_all_legacy_paths_and_exact_workaround(self):
        with tempfile.TemporaryDirectory(prefix='yeney-state-') as tmp:
            base = Path(tmp); config = base/'etc/yeney'; units = base/'units'; state = base/'state'
            config.mkdir(parents=True); state.mkdir()
            config.joinpath('config').write_text('LMS_SERVER=host\nroom.Sonos Port=yes\n')
            runner = Commands('Sonos Port\n')
            unit = installer.service_states(['Sonos Port'], runner)['Sonos Port'][0]
            runner.active.add(unit); runner.enabled.add(unit)
            override = units/(unit+'.d')/'statedir.conf'; override.parent.mkdir(parents=True)
            override.write_text('[Service]\nStateDirectory=\nStateDirectory=yeney/sonos-port\n')
            old_names = [r'Sonos\x20Port', 'Sonos Port', 'sonos-port']
            for n, old in enumerate(old_names):
                directory = state/old; directory.mkdir()
                (directory/f'history-{n}').mkdir(); (directory/f'history-{n}'/'asset').write_bytes(bytes([n]))
                (directory/'icon.json').write_text('{"svg":"sonos"}')
            output = io.StringIO()
            installer.install(ROOT, config, units, [], runner, output, io.StringIO(),
                              lms_check=mock_lms, state_root=state)
            new = state/installer.state_name('Sonos Port')
            self.assertEqual(new.joinpath('icon.json').read_text(), '{"svg":"sonos"}')
            for n, old in enumerate(old_names):
                self.assertFalse((state/old).exists())
                self.assertEqual((new/f'history-{n}'/'asset').read_bytes(), bytes([n]))
            self.assertFalse(override.exists())
            stop = runner.calls.index(['systemctl', 'stop', unit])
            restart = runner.calls.index(['systemctl', 'restart', unit])
            self.assertLess(stop, restart)
            for custom in ('[Service]\nStateDirectory=yeney/custom\n',
                           '[Service]\nStateDirectory=\nStateDirectory=yeney/sonos-port\n# custom\n'):
                override.write_text(custom); warnings = []
                self.assertEqual(installer.state_workarounds(units, { 'Sonos Port': (unit, True, True)}, warnings.append), [])
                self.assertEqual(len(warnings), 1)
                self.assertEqual(override.read_text(), custom)
            override.unlink(); target = base/'owner'; target.write_text('[Service]\nStateDirectory=\nStateDirectory=yeney/sonos-port\n')
            override.symlink_to(target); warnings = []
            self.assertFalse(installer.state_workarounds(units, {'Sonos Port': (unit, True, True)}, warnings.append))
            self.assertTrue(override.is_symlink()); self.assertEqual(len(warnings), 1)
        print('PASS installer migration preserves icon/history, stops writer, removes only exact workaround, preserves owner edits/symlinks')

    def test_migration_conflicts_preserve_both(self):
        with tempfile.TemporaryDirectory(prefix='yeney-state-') as tmp:
            state = Path(tmp)
            a = state/'Study'; b = state/installer.state_name('Study'); a.mkdir(); b.mkdir()
            (a/'icon').write_text('owner'); (b/'icon').write_text('new')
            with self.assertRaises(ValueError): installer.state_migrations(state, {'Study': ('yeney@Study.service', True, True)})
            self.assertEqual((a/'icon').read_text(), 'owner'); self.assertEqual((b/'icon').read_text(), 'new')
        print('PASS installer migration conflict preserves both state copies')

    def test_restart_every_running_room_for_binary_and_units(self):
        with tempfile.TemporaryDirectory(prefix='yeney-state-') as tmp:
            base = Path(tmp); config = base/'config'; units = base/'units'
            config.mkdir(); (config/'config').write_text('LMS_SERVER=host\n'+''.join(f'room.{r}=yes\n' for r in NAMES))
            runner = Commands('\n'.join(NAMES)+'\n')
            states = installer.service_states(NAMES, runner)
            runner.active = {v[0] for v in states.values()}; runner.enabled = set(runner.active)
            def apply():
                output = io.StringIO()
                installer.install(ROOT, config, units, [], runner, output, io.StringIO(),
                                  lms_check=mock_lms, state_root=base/'state')
                return output.getvalue()
            apply(); runner.calls.clear()
            self.assertIn('Nothing to do.', apply()); self.assertFalse(runner.actions())
            runner.build = 'binary-changed'; runner.calls.clear()
            text = apply()
            self.assertEqual({a[-1] for a in runner.actions() if a[1]=='restart'}, runner.active)
            self.assertEqual(runner.calls.count(['systemctl','daemon-reload']), 1)
            for room in NAMES:
                self.assertRegex(text, __import__('re').escape(room)+r' +restarted +LMS player')
            for change in ('template', 'dropin', 'pending'):
                if change == 'template': (units/'yeney@.service').write_text('# old unit\n')
                elif change == 'dropin': next(iter(installer.state_dropins(units, states).values()))[0].write_text('# changed\n')
                else: (config/'unit-update-pending').write_text('earlier restart failed\n')
                runner.calls.clear(); apply()
                self.assertEqual({a[-1] for a in runner.actions() if a[1]=='restart'}, runner.active)
                reload = runner.calls.index(['systemctl','daemon-reload'])
                self.assertTrue(all(reload < runner.calls.index(a) for a in runner.actions()))
                self.assertFalse((config/'unit-update-pending').exists())
                runner.calls.clear(); apply(); self.assertFalse(runner.actions())
        print('PASS installer changed binary/template/drop-in restarts every running room; reload before restart; unchanged install keeps rooms')

    def test_reload_failure_and_loaded_settings(self):
        with tempfile.TemporaryDirectory(prefix='yeney-state-') as tmp:
            base = Path(tmp); config = base/'config'; units = base/'units'
            config.mkdir(); (config/'config').write_text('LMS_SERVER=host\nroom.Study=yes\n')
            runner = Commands('Study\n')
            unit = 'yeney@Study.service'
            runner.active = {unit}; runner.enabled = {unit}
            def apply(run):
                installer.install(ROOT, config, units, [], run, io.StringIO(), io.StringIO(),
                                  lms_check=mock_lms, state_root=base/'state')
            def fail_reload(args, **kwargs):
                if args == ['systemctl', 'daemon-reload']:
                    self.assertEqual((units/'yeney@.service').read_text(), (ROOT/'packaging/yeney@.service').read_text())
                    path, text = installer.state_dropins(units, {'Study': (unit, True, True)})['Study']
                    self.assertEqual(path.read_text(), text)
                    raise subprocess.CalledProcessError(1, args)
                return runner(args, **kwargs)
            with self.assertRaisesRegex(ValueError, 'daemon-reload failed; no rooms restarted'):
                apply(fail_reload)
            self.assertFalse(runner.actions())
            self.assertFalse((config/'installed-build').exists())
            self.assertTrue((config/'unit-update-pending').exists())
            runner.calls.clear(); apply(runner)
            reload = runner.calls.index(['systemctl', 'daemon-reload'])
            restart = runner.calls.index(['systemctl', 'restart', unit])
            self.assertLess(reload, restart)
            self.assertEqual(runner.calls.count(['systemctl', 'daemon-reload']), 1)
            for prop in ('StateDirectory', 'Environment'):
                self.assertGreater(runner.calls.index(['systemctl', 'show', '--property', prop, '--value', unit]), restart)
            def stale(args, **kwargs):
                result = runner(args, **kwargs)
                if args[:4] == ['systemctl', 'show', '--property', 'StateDirectory']:
                    result.stdout = 'yeney/stale'
                return result
            runner.build = 'stale-settings-build'
            with self.assertRaisesRegex(ValueError, 'loaded StateDirectory'):
                apply(stale)
            def inactive(args, **kwargs):
                result = runner(args, **kwargs)
                if args[:2] == ['systemctl', 'is-active']:
                    result.returncode = 3
                return result
            with self.assertRaisesRegex(ValueError, 'not active'):
                apply(inactive)
        print('PASS installer writes all units before one reload before restart; failed reload prevents restart; active and loaded StateDirectory/Environment verified')

    def test_binary_bytes_and_failed_unit_restart_are_retried(self):
        with tempfile.TemporaryDirectory(prefix='yeney-state-') as tmp:
            base = Path(tmp); config = base/'config'; units = base/'units'; binary = base/'binary'
            config.mkdir(); (config/'config').write_text('LMS_SERVER=host\nroom.Study=yes\nroom.MBR=yes\n')
            binary.write_bytes(b'first binary')
            runner = Commands('Study\nMBR\n')
            runner.active = {'yeney@Study.service', 'yeney@MBR.service'}; runner.enabled = set(runner.active)
            def identity(*_): return 'same-commit', 'commit=same\nsha256='+hashlib.sha256(binary.read_bytes()).hexdigest()+'\n'
            def apply(run=runner):
                installer.install(ROOT, config, units, [], run, io.StringIO(), io.StringIO(),
                                  lms_check=mock_lms, state_root=base/'state')
            with patch.object(installer, 'build_identity', identity):
                apply(); runner.calls.clear(); binary.write_bytes(b'rebuilt binary at same commit')
                apply()
                self.assertEqual(len([a for a in runner.actions() if a[1]=='restart']), 2)
                previous = (config/'installed-build').read_bytes()
                (units/'yeney@.service').write_text('# stale unit\n')
                def fail(args, **kwargs):
                    if args[:2] == ['systemctl', 'restart']: raise subprocess.CalledProcessError(1, args)
                    return runner(args, **kwargs)
                with self.assertRaises(subprocess.CalledProcessError): apply(fail)
                self.assertTrue((config/'unit-update-pending').exists())
                self.assertEqual((config/'installed-build').read_bytes(), previous)
                runner.calls.clear(); apply()
                self.assertEqual(len([a for a in runner.actions() if a[1]=='restart']), 2)
                self.assertFalse((config/'unit-update-pending').exists())
                runner.calls.clear(); apply(); self.assertFalse(runner.actions())
        print('PASS installer rebuilt binary at same commit restarts all rooms; failed managed-unit restart retries on next install')

if __name__ == '__main__':
    unittest.main(verbosity=2)
