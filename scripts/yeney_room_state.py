# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Shared systemd room-state lookup. Command failures never imply legacy layout."""
from pathlib import Path
import subprocess

LEGACY_ROOT = Path('/var/lib/yeney')
STATE_BASE = Path('/var/lib')


def command(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True, timeout=5).stdout.strip()


def state_directory(value, *, state_base=STATE_BASE, legacy_root=LEGACY_ROOT):
    value = value.strip()
    if not value:
        return Path(legacy_root)
    # YeneY owns one StateDirectory. Refuse ambiguous/malformed properties rather
    # than silently sampling an unrelated or stale legacy file.
    path = Path(value)
    if path.is_absolute() or any(c.isspace() or c in '\\"\'' for c in value) or ':' in value or '\0' in value or value == '.' or '..' in path.parts:
        raise ValueError('invalid or multiple room StateDirectory values: '+repr(value))
    return Path(state_base)/path


def room_state(room, run=command, *, state_base=STATE_BASE, legacy_root=LEGACY_ROOT):
    unit = run('systemd-escape', '--template=yeney@.service', '--', room).strip()
    if not unit.startswith('yeney@') or not unit.endswith('.service') or '/' in unit or '\n' in unit:
        raise ValueError('invalid escaped room unit')
    value = run('systemctl', 'show', unit, '-p', 'StateDirectory', '--value')
    return unit, state_directory(value, state_base=state_base, legacy_root=legacy_root)
