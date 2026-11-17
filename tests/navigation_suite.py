# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Run the loopback navigation contract and report its gate cost."""
import argparse
import os
import random
import subprocess
import sys
import time

started = time.monotonic()
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--soak', action='store_true')
parser.add_argument('--shortened-seed', type=int)
parser.add_argument('--measure-shortened', action='store_true')
parser.add_argument('--source', choices=('websocket', 'upnp'))
parser.add_argument('--part', choices=('base', 'encoded'))
args = parser.parse_args()
if args.shortened_seed is not None:
    if args.soak or not args.source or args.part:
        parser.error('the shortened-row profile needs one position source')
    cases = ['R-time', 'R-index', 'R-app', 'R-backward', 'R-forward', 'R-history-lms',
             'R-history-app', 'R-far', 'R-natural', 'R-producing']
    random.Random(args.shortened_seed).shuffle(cases)
    replacements = 0
    for case in cases:
        env = dict(os.environ, YENEY_POSITION_SOURCE=args.source,
                   YENEY_FAKE_WEBSOCKET='1' if args.source == 'websocket' else '0',
                   YENEY_REAL_LMS_NAV_INVARIANTS='1', YENEY_NAV_CASE=case,
                   YENEY_SEEK_RESTART_SEED=str(args.shortened_seed),
                   YENEY_SEEK_RESTART_TIMING=str(int(args.measure_shortened)))
        with subprocess.Popen([sys.executable, 'tests/real_lms_test.py'], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True) as child:
            for line in child.stdout:
                print(line, end='', flush=True)
                replacements += 'EVIDENCE yeney: queue window replace ' in line and 'reason=seek-partial edits=2' in line
            if child.wait():
                raise SystemExit(child.returncode)
    assert replacements >= len(cases), replacements
    print(f'PASS shortened-row endurance source={args.source} seed={args.shortened_seed} '
          f'steps={len(cases)} seek_partial_replacements={replacements} '
          'zero_relay_faults=yes zero_worker_faults=yes zero_process_exits=yes', flush=True)
    raise SystemExit(0)

soak = args.soak
if soak and (args.source or args.part):
    parser.error('targeted selectors cannot be used with soak')
cases = [('SOAK', str(seed)) for seed in (1729, 271828, 8675309)] if soak else [
    (case, '') for case in ('S1', 'S2', 'S3', 'S4', 'S4b', 'S4c', 'S4d', 'S5', 'A1', 'A2', 'A3', 'B1', 'B2', 'F1', 'F2', 'F3')]
matrix = [(case, seed, 'converted-flac-mixed' if soak and seed == '271828' else '') for case, seed in cases]
base_count = len(matrix)
if not soak:
    matrix += [(case, '', 'alac') for case in ('S1', 'S2', 'S5', 'A1', 'A2', 'A3')]
    matrix += [('D1', '', 'alac'), ('S1', '', 'trim')]
    matrix += [(case, '', media) for case in ('S1', 'S2', 'S4')
               for media in ('converted', 'converted-alac-192000', 'converted-flac-96000', 'converted-flac-192000')]
    matrix += [('S1', '', 'converted-flac-nonaligned-192000')]
    if os.environ.get('YENEY_LENGTH_FIXTURE_DIR'):
        matrix += [('S1', '', media) for media in ('private-mira', 'private-spell')]
    elif args.part != 'base':
        print('SKIP D5 private ALAC ReplayGain: YENEY_LENGTH_FIXTURE_DIR unset', flush=True)
if args.part == 'base':
    matrix = matrix[:base_count]
elif args.part == 'encoded':
    matrix = matrix[base_count:]
matrix = [(case, seed, media, ('upnp' if seed == '8675309' else 'websocket')) for case, seed, media in matrix] if soak else [
    (case, seed, media, source) for source in ([args.source] if args.source else ('websocket', 'upnp')) for case, seed, media in matrix]
for case, seed, media, source in matrix:
    case_started = time.monotonic()
    env = dict(os.environ, YENEY_POSITION_SOURCE=source, YENEY_FAKE_WEBSOCKET='1' if source == 'websocket' else '0', YENEY_REAL_LMS_NAV_INVARIANTS='1', YENEY_NAV_CASE=case,
               YENEY_NAV_SEED=seed, YENEY_NAV_MEDIA=media, YENEY_NAV_FAULT='mismatch' if case == 'D1' else '')
    result = subprocess.run([sys.executable, 'tests/real_lms_test.py'], env=env)
    if result.returncode:
        raise SystemExit(result.returncode)
    print(f'PASS navigation source={source} case={case} media={media or "wav-flac"} seed={seed or "-"} runtime_seconds={time.monotonic()-case_started:.3f}', flush=True)
print(f'PASS navigation {"soak" if soak else "targeted"} gate runtime_seconds={time.monotonic()-started:.3f}', flush=True)
