# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Deterministic CLI sampling without a live LMS or wall-clock sleeps."""
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("position_check", str(root / "scripts/yeney-lms-position-check"))
spec = importlib.util.spec_from_loader(loader.name, loader)
tool = importlib.util.module_from_spec(spec)
loader.exec_module(tool)
mac = "94:9f:3e:fa:ba:66"


class Clock:
    now = 100.0

    def read(self):
        return self.now

    def sleep(self, amount):
        self.now += amount


class Connection:
    def __init__(self, clock, excursions=False):
        self.clock, self.excursions = clock, excursions
        self.commands = []

    def makefile(self, mode):
        assert mode == "rb"
        return self

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass

    def sendall(self, command):
        assert command == f"{mac} time ?\n".encode()
        self.commands.append(command)

    def readline(self, limit):
        assert limit == 4097
        mono = self.clock.now + .005
        position = (mono - 100) * 1.000012 + 10
        if self.excursions and len(self.commands) % 16 in (0, 1):
            position += .5
        self.clock.now += .010
        return f"{mac.replace(':', '%3A')} time {position:.9f}\n".encode()


for excursions in (False, True):
    clock = Clock()
    connection = Connection(clock, excursions)
    samples = tool.collect(connection, mac, 60, clock.read, clock.sleep)
    slope, residuals = tool.fit(samples)
    assert len(samples) == 240
    assert all(abs(samples[i + 1][0] - samples[i][0] - .25) < 1e-8 for i in range(239))
    if excursions:
        assert max(abs(r) for r in residuals) * 1000 > 430
    else:
        assert abs((slope - 1) * 1e6 - 12) < .01
        assert max(abs(r) for r in residuals) * 1000 < .001
print("PASS: owner CLI tool queries only time ?, timestamps RTT midpoint, samples every 0.25 s, recovers 12 ppm and detects 0.5 s excursions")

for value, expected, warning in ((None, 1, False), ("1", 1, False), ("0", 0, False), ("invalid", 1, True), ("", 1, True)):
    env = dict(os.environ)
    env.pop("YENEY_LMS_POSITION_FROM_MODEL", None)
    if value is not None:
        env["YENEY_LMS_POSITION_FROM_MODEL"] = value
    result = subprocess.run([str(root / "lms-position-test"), "setting"], env=env, capture_output=True, text=True, check=True)
    assert f"model={expected}" in result.stdout
    assert result.stdout.count("invalid=") == int(warning)
print("PASS: model-position default/0/1 and invalid fallback parsing")

samples = [(100+i*.25, 10+i*.25 + (-.5 if i < 80 else 0)) for i in range(240)]
assert tool.lock_time(samples, 'yeney: timing-lock state=locked mono=120.000 udn=RINCON') == 20
assert tool.lock_time(samples, '') is None
assert tool.lock_time(samples, 'yeney: timing mono=99 state=locked') == 0
assert tool.lock_time(samples, 'yeney: timing mono=99 state=locked\nyeney: timing-lock state=acquiring mono=100\nyeney: timing-lock state=locked mono=120') == 20
import contextlib
import io
output = io.StringIO()
with contextlib.redirect_stdout(output):
    tool.summary(samples,20)
assert 'largest_step_after_10s_ms=750.000' in output.getvalue()
assert 'backwards_count=0' in output.getvalue()
assert 'before_lock_error samples=80 max_ms=500.000' in output.getvalue()
assert 'after_lock_error samples=160 max_ms=0.000' in output.getvalue()
samples[100] = (samples[100][0],samples[99][1]-.1)
with contextlib.redirect_stdout(output):
    tool.summary(samples,None)
assert 'backwards_count=1' in output.getvalue()
assert 'before_lock_error=unknown' in output.getvalue()
print('PASS: producer lock timestamp, largest step, backwards and separate startup/stable errors')

for kind in ('cold','warm'):
    fixture = root / 'tests/fixtures/field-study'
    samples = [tuple(map(float,line.split())) for line in (fixture/f'{kind}-positions.txt').read_text().splitlines()]
    journal = (fixture/f'{kind}-journal.txt').read_text()
    groups = tool.segments(samples)
    assert len(groups) == 2, (kind,len(groups))
    assert all(all(group[i][1]>=group[i-1][1] for i in range(1,len(group))) for group in groups)
    assert abs(tool.lock_time(samples,journal)-(325.213 if kind=='cold' else 6.703))<.002
    output = io.StringIO()
    with contextlib.redirect_stdout(output): tool.report(samples,journal)
    report = output.getvalue()
    assert 'overall segments=2' in report and report.count('ppm=') == 3
    assert 'backwards_count=0' in report
    assert 'stream_start_mono_s=' in report
    (Path('/tmp')/f'field-{kind}-checker.txt').write_text(report)
    print(f'PASS: {kind} capture: two track fits, stream-based lock={tool.lock_time(samples,journal):.3f} s')

assert len(tool.segments([(1,10),(2,11),(3,12)], ['a','a','b'])) == 2
assert tool.lock_time([(110,2),(111,3)], 'Creating new stream (1)\ncore: STMs jiffies=100000 elapsed_ms=0\nyeney: timing-lock state=locked mono=105') == 5
print('PASS: track identity change without decrease and capture beginning after lock')

class TrackConnection(Connection):
    def sendall(self, command):
        assert command in (f'{mac} time ?\n'.encode(), f'{mac} status - 1 tags:\n'.encode())
        self.last_command = command
        self.commands.append(command)

    def readline(self, limit):
        if b'status' in self.last_command:
            self.clock.now += .002
            identity = 1 if self.clock.now < 100.5 else 2
            return f'{mac} status - 1 time:{self.clock.now-100:.6f} playlist_cur_index:{identity} id:{identity}\n'.encode()
        return super().readline(limit)

clock = Clock(); connection = TrackConnection(clock); identities=[]
samples=tool.collect(connection,mac,1,clock.read,clock.sleep,tracks=identities)
assert len(samples)==4 and len(connection.commands)==4
assert len(tool.segments(samples,identities))==2
from unittest.mock import patch
records = '\n'.join(__import__('json').dumps({'_PID':pid,'MESSAGE':message}) for pid,message in (
    ('7','Creating new stream (1)'), ('7','core: STMs jiffies=100000 elapsed_ms=0'),
    ('8','unrelated RINCON_949F3EFABA6601400 in shared topology'), ('8','core: STMs jiffies=110000 elapsed_ms=0'), ('7','yeney: timing-lock state=locked mono=105 udn=RINCON_949F3EFABA6601400')))
with patch.object(tool.subprocess,'run',return_value=type('Result',(),{'stdout':records})()):
    journal=tool.journal_for(mac,0)
assert 'Creating new stream' in journal and 'unrelated' not in journal
assert tool.lock_time([(110,2),(111,3)],journal)==5
print('PASS: live read-only track identity sampling and complete producer process journal selection')
