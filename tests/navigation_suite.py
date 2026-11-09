# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Run the loopback navigation contract and report its gate cost."""
import os
import subprocess
import sys
import time

started = time.monotonic()
soak = '--soak' in sys.argv
cases = [('SOAK', str(seed)) for seed in (1729, 271828, 8675309)] if soak else [
    (case, '') for case in ('A1', 'A2', 'A3', 'B1', 'B2')]
for case, seed in cases:
    env = dict(os.environ, YENEY_REAL_LMS_NAV_INVARIANTS='1', YENEY_NAV_CASE=case,
               YENEY_NAV_SEED=seed)
    result = subprocess.run([sys.executable, 'tests/real_lms_test.py'], env=env)
    if result.returncode:
        raise SystemExit(result.returncode)
print(f'PASS navigation {"soak" if soak else "targeted"} gate runtime_seconds={time.monotonic()-started:.3f}', flush=True)
