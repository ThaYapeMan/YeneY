# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import os
import subprocess
for value, expected in [(None, 'sonos 1'), ('wall', 'wall 0'), ('sonos', 'sonos 1'), ('bad', 'sonos 1'), ('', 'sonos 1')]:
    env = dict(os.environ, YENEY_TIMING_PROBE='0')
    env.pop('YENEY_STREAM_PACING', None)
    if value is not None:
        env['YENEY_STREAM_PACING'] = value
    p = subprocess.run(['./stream-pacing-test', 'setting'], env=env, text=True, capture_output=True, check=True)
    assert p.stdout.strip() == expected, (p.stdout, p.stderr)
    if value in ('bad', ''):
        assert 'using sonos' in p.stderr
    metrics = subprocess.run(['./stream-metrics-test'], env=env, text=True, capture_output=True, check=True)
    assert 'Stream pacing: mode=' + expected.split()[0] + ' ' in metrics.stdout
    assert ('invalid' in p.stderr) == (value in ('bad', ''))
print('PASS stream pacing settings: sonos default/fallback and status, wall opt-out, invalid warning')
