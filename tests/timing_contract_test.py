# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import sys
sys.dont_write_bytecode = True
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import json

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'scripts/yeney-timing-read'
loader = importlib.machinery.SourceFileLoader('timing_reader', str(TOOL))
spec = importlib.util.spec_from_loader(loader.name, loader)
reader = importlib.util.module_from_spec(spec)
loader.exec_module(reader)

def settings(probe='1', publish='0', every=None, offset=None):
    env = os.environ.copy()
    for key, value in [('YENEY_TIMING_PROBE', probe), ('YENEY_TIMING_PUBLISH', publish),
                       ('YENEY_TIMING_LOCKED_EVERY_S', every), ('YENEY_AUDIBLE_OFFSET_MS', offset)]:
        env.pop(key, None)
        if value is not None:
            env[key] = value
    return subprocess.check_output([str(ROOT / 'timing-contract-test'), 'settings'], env=env, text=True)

for probe in [None, '0']:
    assert settings(probe, '1', 'bad', 'bad') == 'publish=0 every=5 offset=0\n'
for value in [None, '1', '5', '60']:
    text = settings(every=value)
    assert f'every={5 if value is None else int(value)}' in text and 'invalid=' not in text
for value in ['', '0', '61', '+5', '-5', ' 5', '5s', '99999999999999999999']:
    text = settings(every=value)
    assert 'every=5' in text and text.count('key=YENEY_TIMING_LOCKED_EVERY_S') == 1
for value in [None, '-500', '-1', '0', '500']:
    text = settings(publish='1', offset=value)
    assert f'offset={0 if value is None else int(value)}' in text and 'invalid=' not in text
for value in ['', '-501', '501', '+1', '1ms', ' 1', '99999999999999999999']:
    text = settings(publish='1', offset=value)
    assert 'offset=0' in text and text.count('key=YENEY_AUDIBLE_OFFSET_MS') == 1
for value in ['', '2', 'invalid']:
    text = settings(publish=value, offset='bad')
    assert 'publish=0' in text and text.count('key=YENEY_TIMING_PUBLISH') == 1
    assert 'key=YENEY_AUDIBLE_OFFSET_MS' not in text
print('PASS: publication/probe gating; locked interval and calibration strict parsing/fallbacks')

with tempfile.TemporaryDirectory(prefix='yeney-timing-reader-') as directory:
    mac = '48:a6:b8:20:39:64'
    timing = bytearray(128)
    audio = bytearray(32888)
    now = time.monotonic_ns()
    values = dict(magic=b'YNTM', version=1, flags=1, write_seq=4, size=128,
                  shm_generation=123, model_epoch=9, state=2, discontinuity=0,
                  anchor_abs_frame=1000, anchor_audible_mono_ns=now + 1_500_000_000,
                  sample_rate_hz=48000, drift_ppb=-10000, uncertainty_us=3000,
                  offset_us=-20000, updated_mono_ns=now, valid_from_abs_frame=1000,
                  valid_until_abs_frame=(1 << 64) - 1)
    for name, offset, fmt in reader.FIELDS:
        struct.pack_into('<'+fmt, timing, offset, values[name])
    struct.pack_into('<I', audio, 68, 48000)
    struct.pack_into('<I', audio, 32856, 2)
    struct.pack_into('<QQ', audio, 32860, 123, 49000)
    tp = Path(directory) / ('yeney-timing-'+mac)
    ap = Path(directory) / ('squeezelite-'+mac)
    tp.write_bytes(timing); ap.write_bytes(audio)
    result = json.loads(subprocess.check_output([str(TOOL), mac.upper(), '--once', '--directory', directory]))
    assert result['anchor_abs_frame'] == 1000 and result['shm_generation'] == 123
    assert result['abs_write_pos'] == 49000 and result['uncertainty_us'] == 3000
    expected = values['anchor_audible_mono_ns'] + 1e9 / .99999
    assert abs(result['audible_mono_ns'] - expected) <= 1
    assert tp.read_bytes() == timing and ap.read_bytes() == audio
    r = reader.decode(timing)
    assert reader.predict(r, 123, 999, 48000, now) is None
    assert reader.predict(r, 124, 49000, 48000, now) is None
    assert reader.predict(r, 123, 49000, 44100, now) is None
    assert reader.predict(r, 123, 49000, 48000, now+3_000_000_001) is None
    for state in [0, 1, 3]:
        r['state'] = state
        assert reader.predict(r, 123, 49000, 48000, now) is None
print('PASS: owner read-only tool fixture decoding, exact prediction, unchanged objects; stale/epoch/generation/rate fencing')

env = {**os.environ, 'YENEY_TIMING_PROBE': '1', 'YENEY_TIMING_PUBLISH': '1',
       'YENEY_AUDIBLE_OFFSET_MS': '-25'}
subprocess.run([str(ROOT / 'timing-contract-test'), 'publication'], env=env, check=True)
