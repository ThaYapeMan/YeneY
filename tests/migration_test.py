"""Run the migration against isolated roots and stub host-changing commands."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

script = Path(__file__).resolve().parents[1] / 'scripts/migrate-from-sonos-lms.sh'
old_units = ['sonos-lms@Study.service', r'sonos-lms@Sonos\x20Port.service', 'sonos-lms@MBR.service']
settings = {
    'SONOS_LMS_UPNP': 'YENEY_UPNP', 'SONOS_LMS_PAUSE': 'YENEY_PAUSE',
    'SONOS_LMS_AUDIO': 'YENEY_AUDIO', 'SONOS_LMS_TITLE_FORMAT': 'YENEY_TITLE_FORMAT',
    'SONOS_LMS_STREAM_CONTENT': 'YENEY_STREAM_CONTENT',
    'SONOS_LMS_YENEY_POLL': 'YENEY_POLL',
    'SONOS_LMS_YENEY_STOPPED_MEDIAINFO': 'YENEY_STOPPED_MEDIAINFO',
}
with tempfile.TemporaryDirectory(prefix='yeney-migration-') as tmp:
    base = Path(tmp)
    tools = base / 'bin'
    tools.mkdir()
    stub = '''#!/usr/bin/env python3
import json, os, pathlib, sys
root = pathlib.Path(os.environ['TEST_ROOT'])
command = [pathlib.Path(sys.argv[0]).name, *sys.argv[1:]]
with (root / 'calls').open('a') as f: f.write(json.dumps(command) + '\\n')
if command[0] == 'systemctl' and command[1] == 'list-unit-files':
    sys.exit(1)
if command[0] == 'systemctl' and command[1] == 'list-units':
    print((root / 'active').read_text(), end='')
    if (root / 'list-error').exists():
        sys.stderr.write((root / 'list-stderr').read_text() if (root / 'list-stderr').exists() else '')
        sys.exit(int((root / 'list-error').read_text()))
if command[0] == 'systemctl' and command[1] == 'disable':
    for p in (root / 'etc/systemd/system').glob('*.wants/' + command[2]):
        p.unlink()
if command[0] == 'systemctl' and command[1] == 'stop':
    p = root / 'active'
    p.write_text(''.join(line for line in p.read_text().splitlines(True) if line.split()[0] != command[2]))
if command[0] == 'make':
    if (root / 'fail-build').exists(): sys.exit(7)
    pathlib.Path('yeney').write_text('built')
if command[0] == 'install-devices.sh':
    assert pathlib.Path('yeney').read_text() == 'built'
    assert sys.argv[1:3] == ['--non-interactive', '--restart']
    assert sorted(sys.argv[3:]) == ['MBR', 'Sonos Port', 'Study'], sys.argv
    for room in sys.argv[3:]:
        import subprocess
        unit = subprocess.check_output(['systemd-escape', '--template=yeney@.service', '--', room], text=True).strip()
        subprocess.run(['systemctl', 'enable', '--now', unit], check=True)
'''
    for name in ('systemctl', 'git', 'make'):
        p = tools / name
        p.write_text(stub)
        p.chmod(0o755)
    for scenario in ('normal', 'interrupted', 'active', 'empty-exit-one', 'no-rooms', 'explicit',
                     'list-failure', 'list-one-output', 'list-one-stderr', 'invalid-rooms', 'unescape-failure', 'empty-journal'):
        interrupt = scenario == 'interrupted'
        root = base / scenario
        repo, conf, units = root / 'opt/sonos-lms', root / 'etc/sonos-lms', root / 'etc/systemd/system'
        (repo / 'scripts').mkdir(parents=True)
        (repo / 'sonos-lms').write_text('old binary')
        (repo / 'sonos-lms.o').write_text('old object')
        conf.mkdir(parents=True)
        units.mkdir(parents=True)
        installer = repo / 'scripts/install-devices.sh'
        installer.write_text(stub)
        installer.chmod(0o755)
        (conf / 'config').write_text('LMS_SERVER=192.0.2.23\nroom.Sonos Port=yes\n')
        (conf / 'rooms').write_text('Study\nSonos Port\nMBR\n')
        (units / 'sonos-lms@.service').write_text('old unit')
        dropin = units / (old_units[1] + '.d')
        dropin.mkdir()
        (dropin / 'override.conf').write_text('[Service]\n' + ''.join(f'Environment="{key}=value"\n' for key in settings) +
                                            'Environment=SONOS_LMS_EVENT_PORT=1401\nEnvironment=KEEP_ME=yes\n')
        wants = units / 'multi-user.target.wants'
        wants.mkdir()
        enabled_units = old_units if scenario not in ('no-rooms', 'explicit', 'empty-journal') else []
        if scenario == 'active':
            enabled_units = old_units[:2]
        for unit in enabled_units:
            (wants / unit).symlink_to('../sonos-lms@.service')
        # Duplicate names, regular files, and dangling symlinks all count.
        if enabled_units:
            other = units / 'other.target.wants'
            other.mkdir()
            (other / old_units[0]).write_text('')
            (other / old_units[1]).symlink_to('../missing-template.service')
        active = ''.join(unit + ' loaded active running Room bridge\n' for unit in old_units) if scenario == 'active' else ''
        active += 'sonos-lms@Offline.service loaded inactive dead Offline room\n' if scenario in ('normal', 'no-rooms') else ''
        (root / 'active').write_text(active)
        if scenario in ('empty-exit-one', 'list-failure', 'list-one-output', 'list-one-stderr'):
            (root / 'list-error').write_text('2' if scenario == 'list-failure' else '1')
        if scenario == 'list-one-output':
            (root / 'active').write_text('sonos-lms@MBR.service loaded active running Room bridge\n')
        if scenario == 'list-one-stderr':
            (root / 'list-stderr').write_text('Failed to connect to bus\n')
        if scenario == 'unescape-failure':
            (wants / r'sonos-lms@bad\xZZ.service').symlink_to('../sonos-lms@.service')
        if scenario == 'empty-journal':
            (root / 'etc/yeney-migration.json').write_text(json.dumps({'instances': [], 'stopped': True}) + '\n')
        env = dict(os.environ, TEST_ROOT=str(root), PATH=str(tools) + os.pathsep + os.environ['PATH'])
        command = ['bash', str(script), '--root', str(root)]
        if scenario in ('explicit', 'empty-journal'):
            command += ['--rooms', 'Study,Sonos Port,MBR,Study']
        if scenario == 'invalid-rooms':
            command += ['--rooms', 'Study,,MBR']
        if scenario in ('no-rooms', 'list-failure', 'list-one-output', 'list-one-stderr', 'invalid-rooms', 'unescape-failure'):
            before_files = {str(p.relative_to(root)): (('link', str(p.readlink())) if p.is_symlink() else ('file', p.read_bytes()))
                            for p in root.rglob('*') if p.is_symlink() or p.is_file()}
            failed = subprocess.run(command, env=env, capture_output=True, text=True)
            assert failed.returncode != 0, failed.stdout
            if scenario == 'no-rooms':
                assert 'No enabled sonos-lms rooms found' in failed.stderr, failed.stderr
            after_files = {str(p.relative_to(root)): (('link', str(p.readlink())) if p.is_symlink() else ('file', p.read_bytes()))
                           for p in root.rglob('*') if (p.is_symlink() or p.is_file()) and p.name != 'calls'}
            assert before_files == after_files, scenario
            calls = [json.loads(line) for line in (root / 'calls').read_text().splitlines()]
            assert all(call[:2] == ['systemctl', 'list-units'] for call in calls), calls
            print(f'PASS: migration {scenario} fails before any file or service changes')
            continue
        if interrupt:
            (root / 'fail-build').touch()
            failed = subprocess.run(command, env=env, capture_output=True, text=True)
            assert failed.returncode != 0, failed.stdout
            assert (root / 'etc/yeney-migration.json').exists()
            assert not any(json.loads(line)[0] == 'install-devices.sh' for line in (root / 'calls').read_text().splitlines())
            (root / 'fail-build').unlink()
        result = subprocess.run(command, env=env, capture_output=True, text=True)
        assert result.returncode == 0, (result.stdout, result.stderr)
        if not interrupt:
            assert 'Recorded 3 room(s): MBR, Sonos Port, Study' in result.stdout, result.stdout
            if scenario == 'active':
                assert 'MBR: active' in result.stdout
                assert 'Sonos Port: active, enabled' in result.stdout
                assert 'Study: active, enabled' in result.stdout
            elif scenario in ('explicit', 'empty-journal'):
                assert all(f'{room}: --rooms' in result.stdout for room in ('Study', 'Sonos Port', 'MBR'))
            else:
                assert all(f'{room}: enabled' in result.stdout for room in ('Study', 'Sonos Port', 'MBR'))
        assert not repo.exists() and not conf.exists()
        assert not (root / 'opt/yeney/sonos-lms').exists()
        assert not (root / 'opt/yeney/sonos-lms.o').exists()
        assert (root / 'etc/yeney/rooms').read_text() == 'Study\nSonos Port\nMBR\n'
        assert (root / 'etc/yeney/config').read_text() == 'LMS_SERVER=192.0.2.23\nroom.Sonos Port=yes\n'
        renamed = units / r'yeney@Sonos\x20Port.service.d/override.conf'
        expected = '[Service]\n' + ''.join(f'Environment="{value}=value"\n' for value in settings.values()) + 'Environment=KEEP_ME=yes\n'
        assert renamed.read_text() == expected, renamed.read_text()
        assert not (units / 'sonos-lms@.service').exists()
        assert not (root / 'etc/yeney-migration.json').exists()
        calls = [json.loads(line) for line in (root / 'calls').read_text().splitlines()]
        for unit in old_units:
            assert calls.count(['systemctl', 'stop', unit]) == 1
            assert calls.count(['systemctl', 'disable', unit]) == 1
            assert ['systemctl', 'enable', '--now', unit.replace('sonos-lms@', 'yeney@')] in calls
        assert ['git', 'remote', 'set-url', 'origin', 'https://github.com/ThaYapeMan/YeneY.git'] in calls
        assert calls.index(['systemctl', 'daemon-reload']) < calls.index(['make'])
        assert "journalctl -u 'yeney@*' -f" in result.stdout
        before = len(calls)
        again = subprocess.run(command[:4], env=env, capture_output=True, text=True, check=True)
        assert 'Nothing to do' in again.stdout
        extra = [json.loads(line) for line in (root / 'calls').read_text().splitlines()][before:]
        assert len(extra) == 1 and extra[0][:2] == ['systemctl', 'list-units'], extra
        print(f'PASS: migration preserves three rooms including Sonos Port, settings, order and repeat-run safety (scenario={scenario})')

# Migration diagnostics: old settings must warn, never configure the new binary.
root = Path(__file__).resolve().parents[1]
names = {
    'SONOS_LMS_UPNP': 'YENEY_UPNP',
    'SONOS_LMS_PAUSE': 'YENEY_PAUSE',
    'SONOS_SQUEEZEBOX_PAUSE': 'YENEY_PAUSE',
    'SONOS_LMS_TITLE_FORMAT': 'YENEY_TITLE_FORMAT',
    'SONOS_LMS_STREAM_CONTENT': 'YENEY_STREAM_CONTENT',
    'SONOS_LMS_AUDIO': 'YENEY_AUDIO',
    'SONOS_LMS_YENEY_POLL': 'YENEY_POLL',
    'SONOS_LMS_YENEY_STOPPED_MEDIAINFO': 'YENEY_STOPPED_MEDIAINFO',
    'SONOS_LMS_EVENT_PORT': None,
    'SONOS_LMS_UNKNOWN': None,
    'SONOS_SQUEEZEBOX_UNKNOWN': None,
}
env = {k: v for k, v in os.environ.items() if not k.startswith(('SONOS_LMS_', 'SONOS_SQUEEZEBOX_', 'YENEY_'))}
env.update({name: 'pause' for name in names})
env['SONOS_LMS_UNKNOWN'] = ''  # Set but empty still warns.
env['UNRELATED'] = 'pause'
expected = sorted('yeney: ' + name + (' is no longer read; rename it to ' + new if new else ' is obsolete')
                  for name, new in names.items())
# The real startup path warns before parsing command options, without needing a device.
result = subprocess.run([str(root / 'yeney'), '--file'], env=env, capture_output=True, text=True)
assert result.returncode != 0
assert sorted(result.stderr.splitlines()[:-1]) == expected, result.stderr
assert result.stderr.splitlines()[-1] == '--file is no longer supported'
with tempfile.TemporaryDirectory(prefix='yeney-old-settings-') as tmp:
    source, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
    source.write_text('''
#include "pause_mode.h"
#include "upnp/title_format.h"
#include <cassert>
int main(int argc, char**) {
    assert((pauseMode() == PauseMode::Pause) == (argc > 1));
    assert(upnp::titleFormat() == upnp::TitleFormat::ArtistTitle);
}
''')
    subprocess.run(['g++', '-I', str(root), str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], env=env, check=True)
    env['YENEY_PAUSE'] = 'pause'
    subprocess.run([str(exe), 'new'], env=env, check=True)
print('PASS: old-variable warning table covers successors, obsolete/unknown/empty values; old pause fallback is ignored and new settings win')
