# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import os
import subprocess
for value, expected in [(None, 'wall 0'), ('wall', 'wall 0'), ('sonos', 'sonos 1'), ('bad', 'wall 0'), ('', 'wall 0')]:
    env = dict(os.environ, YENEY_TIMING_PROBE='0')
    env.pop('YENEY_STREAM_PACING', None)
    if value is not None:
        env['YENEY_STREAM_PACING'] = value
    p = subprocess.run(['./stream-pacing-test', 'setting'], env=env, text=True, capture_output=True, check=True)
    assert p.stdout.strip() == expected, (p.stdout, p.stderr)
    assert ('invalid' in p.stderr) == (value in ('bad', ''))
print('PASS stream pacing settings: wall default/fallback, sonos enables model, invalid warning')
