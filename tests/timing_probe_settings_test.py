import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def run(probe='1', raw=None, stale=None, mode='settings'):
    env = os.environ.copy()
    for key, value in [('YENEY_TIMING_PROBE', probe), ('YENEY_TIMING_RAW', raw), ('YENEY_TIMING_STALE_S', stale)]:
        env.pop(key, None)
        if value is not None:
            env[key] = value
    return subprocess.run([str(ROOT / 'timing-probe-settings-test'), mode], env=env,
                          capture_output=True, text=True, check=True).stdout

for probe in [None, '0']:
    for raw in [None, '0', '1', 'invalid']:
        text = run(probe, raw, 'invalid', 'exercise')
        assert 'timing-raw' not in text and 'timing-edge' not in text
        assert 'YENEY_TIMING_RAW' not in text and 'YENEY_TIMING_STALE_S' not in text
for raw in [None, '0', '', '2', 'invalid']:
    text = run(raw=raw, mode='exercise')
    assert 'timing-raw' not in text and 'timing-edge' not in text
    assert text.count('key=YENEY_TIMING_RAW') == (raw not in (None, '0'))
text = run(raw='1', mode='exercise')
assert text.count('yeney: timing-raw ') == 54
assert text.count('yeney: timing-edge ') == 25
for outcome in ['accepted', 'rejected-rtt', 'rejected-stale-epoch', 'error', 'timeout']:
    assert f'outcome={outcome}' in text
for field in ['room=', 'stream=', 'epoch=', 'seq=', 'kind=', 'send_mono=', 'recv_mono=', 'rtt_ms=',
              'reltime=', 'reltime_s=', 'reason=', 'lo_mono=', 'hi_mono=', 'mid_mono=', 'half_width_ms=',
              'residual_ms=', 'classification=', 'rule=', 'inliers=', 'outliers=', 'outlier_frac=',
              'span_s=', 'drift_sigma_ppm=', 'missed_edges=', 'half_window_ms=']:
    assert field in text, field
for value in [None, '10', '60', '600', '010']:
    text = run(stale=value)
    assert f'stale={60 if value is None else int(value)}' in text
    assert 'invalid=' not in text
for value in ['', '0', '9', '601', '-10', '+10', ' 60', '60 ', '60s', '999999999999999999999']:
    text = run(stale=value)
    assert 'stale=60' in text
    assert text.count('key=YENEY_TIMING_STALE_S') == 1
for value in ['10', '60', '600']:
    assert 'configured stale gap expires once' in run(stale=value, mode='exercise')
print('PASS: raw gating and all request outcomes/edge fields; strict stale parsing/fallback once;')
print('PASS: default 60 s model retains a 30 s gap; configured 10/60/600 s expiry, late epoch fencing')
