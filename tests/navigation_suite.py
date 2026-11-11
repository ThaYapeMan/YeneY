# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Run the loopback navigation contract and report its gate cost."""
import os
import subprocess
import sys
import time

started = time.monotonic()
soak = '--soak' in sys.argv
cases = [('SOAK', str(seed)) for seed in (1729, 271828, 8675309)] if soak else [
    (case, '') for case in ('S1', 'S2', 'S3', 'S4', 'S4b', 'S4c', 'S4d', 'S5', 'A1', 'A2', 'A3', 'B1', 'B2')]
matrix = [(case, seed, 'alac' if soak and seed == '271828' else '') for case, seed in cases]
if not soak:
    matrix += [(case, '', 'alac') for case in ('S1', 'S2', 'S5', 'A1', 'A2', 'A3')]
    matrix += [('D1', '', 'alac'), ('S1', '', 'trim'), ('S1', '', 'converted')]
    if os.environ.get('YENEY_LENGTH_FIXTURE_DIR'):
        matrix += [('S1', '', media) for media in ('private-mira', 'private-spell')]
    else:
        print('SKIP D5 private ALAC ReplayGain: YENEY_LENGTH_FIXTURE_DIR unset', flush=True)
for case, seed, media in matrix:
    case_started = time.monotonic()
    env = dict(os.environ, YENEY_REAL_LMS_NAV_INVARIANTS='1', YENEY_NAV_CASE=case,
               YENEY_NAV_SEED=seed, YENEY_NAV_MEDIA=media, YENEY_NAV_FAULT='mismatch' if case == 'D1' else '')
    result = subprocess.run([sys.executable, 'tests/real_lms_test.py'], env=env)
    if result.returncode:
        raise SystemExit(result.returncode)
    print(f'PASS navigation case={case} media={media or "wav-flac"} seed={seed or "-"} runtime_seconds={time.monotonic()-case_started:.3f}', flush=True)
print(f'PASS navigation {"soak" if soak else "targeted"} gate runtime_seconds={time.monotonic()-started:.3f}', flush=True)
