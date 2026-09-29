#!/usr/bin/env bash
# Run after pulling the rename in the old checkout. --root is for isolated tests.
set -euo pipefail
exec python3 - "$@" <<'PY'
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

parser = argparse.ArgumentParser(description='Migrate sonos-lms rooms to YeneY')
parser.add_argument('--root', type=Path, default=Path('/'))
args = parser.parse_args()
root = args.root.resolve()
if root == Path('/') and os.geteuid() != 0:
    sys.exit('Error: run as root')

def run(*command, cwd=None):
    return subprocess.run(command, cwd=cwd, check=True, text=True, stdout=subprocess.PIPE).stdout.strip()

def step(message):
    print(message, flush=True)

old_repo, repo = root / 'opt/sonos-lms', root / 'opt/yeney'
old_config, config = root / 'etc/sonos-lms', root / 'etc/yeney'
units = root / 'etc/systemd/system'
journal = root / 'etc/yeney-migration.json'
names = {
    'SONOS_LMS_UPNP': 'YENEY_UPNP',
    'SONOS_LMS_PAUSE': 'YENEY_PAUSE',
    'SONOS_LMS_TITLE_FORMAT': 'YENEY_TITLE_FORMAT',
    'SONOS_LMS_STREAM_CONTENT': 'YENEY_STREAM_CONTENT',
    'SONOS_LMS_AUDIO': 'YENEY_AUDIO',
    'SONOS_LMS_YENEY_POLL': 'YENEY_POLL',
    'SONOS_LMS_YENEY_STOPPED_MEDIAINFO': 'YENEY_STOPPED_MEDIAINFO',
}
try:
    # Refuse collisions before stopping any room; never merge or overwrite directories.
    for old, new in ((old_repo, repo), (old_config, config)):
        if old.exists() and new.exists():
            raise ValueError(f'Both {old} and {new} exist; resolve the conflict first')
    if journal.exists():
        state = json.loads(journal.read_text())
        instances = state["instances"]
    else:
        listing = run('systemctl', 'list-unit-files', '--state=enabled,enabled-runtime',
                      '--no-legend', '--no-pager', 'sonos-lms@*.service')
        instances = sorted({line.split()[0] for line in listing.splitlines()
                            if line.startswith('sonos-lms@') and line.split()[0].endswith('.service')})
        if not instances and not old_repo.exists() and not old_config.exists() and not (units / 'sonos-lms@.service').exists() and not list(units.glob('sonos-lms@*.service.d')):
            step('Nothing to do; YeneY migration is already complete.')
            sys.exit(0)
        if not (old_repo if old_repo.exists() else repo).is_dir():
            raise ValueError('No checkout found in /opt/sonos-lms or /opt/yeney')
        state = {'instances': instances, 'stopped': False}
        journal.write_text(json.dumps(state) + '\n')
    rooms = [run('systemd-escape', '--unescape', '--instance', unit) for unit in instances]
    step(f'1. Recorded {len(instances)} enabled room(s): ' + ', '.join(rooms))
    if not state['stopped']:
        for unit in instances:
            run('systemctl', 'stop', unit)
            run('systemctl', 'disable', unit)
            step(f'1. Stopped and disabled {unit}')
        state['stopped'] = True
        journal.write_text(json.dumps(state) + '\n')
    for old, new in ((old_repo, repo), (old_config, config)):
        if old.exists():
            old.rename(new)
            step(f'2. Moved {old} -> {new}')
    for old in sorted(units.glob('sonos-lms@*.service.d')):
        room = run('systemd-escape', '--unescape', '--instance', old.name[:-2])
        new_unit = run('systemd-escape', '--template=yeney@.service', '--', room)
        new = units / (new_unit + '.d')
        if new.exists():
            raise ValueError(f'{new} already exists; refusing to overwrite it')
        old.rename(new)
        step(f'3. Moved {old.name} -> {new.name}')
    # Also revisit moved drop-ins after an interrupted run.
    for directory in sorted(units.glob('yeney@*.service.d')):
        for path in sorted(directory.glob('*.conf')):
            original = path.read_text()
            text = ''.join(line for line in original.splitlines(keepends=True)
                           if re.search(r'\bSONOS_LMS_EVENT_PORT\s*=', line) is None)
            for old, new in names.items():
                text = re.sub(r'\b' + old + r'\b', new, text)
            if text != original:
                path.write_text(text)
                step(f'3. Renamed settings and removed obsolete event port in {path}')
    run('git', 'remote', 'set-url', 'origin', 'https://github.com/ThaYapeMan/YeneY.git', cwd=repo)
    step('4. Set origin to https://github.com/ThaYapeMan/YeneY.git')
    (units / 'sonos-lms@.service').unlink(missing_ok=True)
    run('systemctl', 'daemon-reload')
    step('5. Removed old service template and reloaded systemd')
    subprocess.run(['make'], cwd=repo, check=True)
    (repo / 'sonos-lms').unlink(missing_ok=True)
    (repo / 'sonos-lms.o').unlink(missing_ok=True)
    step('5. Built YeneY and removed the old binary and main object')
    subprocess.run(['scripts/install-devices.sh', '--non-interactive', '--restart', *rooms], cwd=repo, check=True)
    step('5. Installed and started the recorded rooms as YeneY services')
    journal.unlink()
    step("6. Follow logs: journalctl -u 'yeney@*' -f")
except (OSError, ValueError, subprocess.CalledProcessError) as error:
    sys.exit(f'Error: {error}')
PY
